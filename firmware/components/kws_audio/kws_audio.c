#include "kws_audio.h"

#include <stdlib.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "kws_audio";

#define CHUNK_SAMPLES   320 /* 20 ms */
#define BUFFER_SAMPLES  (KWS_AUDIO_SAMPLE_RATE * CONFIG_KWS_AUDIO_BUFFER_MS / 1000)
#define READ_TIMEOUT_MS 200

static i2s_chan_handle_t s_rx;
static StreamBufferHandle_t s_stream;
static StaticStreamBuffer_t s_stream_struct;
static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static kws_audio_stats_t s_stats;

static esp_err_t i2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_KWS_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = CHUNK_SAMPLES;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx), TAG, "i2s_new_channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(KWS_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_KWS_I2S_BCLK_GPIO,
            .ws = CONFIG_KWS_I2S_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = CONFIG_KWS_I2S_DIN_GPIO,
        },
    };
#if CONFIG_KWS_MIC_SLOT_LEFT
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
#else
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_RIGHT;
#endif
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std_cfg), TAG, "i2s std mode");
    return i2s_channel_enable(s_rx);
}

/* 32-bit I2S word -> int16 with fixed gain, saturation and a one-pole DC blocker. */
static void convert(const int32_t *in, int16_t *out, size_t n, int16_t *peak)
{
    static int32_t x1, y1; /* Q0 history of the DC blocker */
    int16_t p = *peak;
    for (size_t i = 0; i < n; i++) {
        int32_t x = in[i] >> CONFIG_KWS_MIC_SHIFT;
#if CONFIG_KWS_MIC_DC_BLOCK
        /* y[n] = x[n] - x[n-1] + 0.995 * y[n-1] */
        int32_t y = x - x1 + (int32_t)(((int64_t)y1 * 32604) >> 15);
        x1 = x;
        y1 = y;
        x = y;
#endif
        if (x > INT16_MAX) {
            x = INT16_MAX;
        } else if (x < INT16_MIN) {
            x = INT16_MIN;
        }
        out[i] = (int16_t)x;
        int16_t a = (x < 0) ? (int16_t)((x == INT16_MIN) ? INT16_MAX : -x) : (int16_t)x;
        if (a > p) {
            p = a;
        }
    }
    *peak = p;
}

static void capture_task(void *arg)
{
    static int32_t raw[CHUNK_SAMPLES];
    static int16_t pcm[CHUNK_SAMPLES];
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    for (;;) {
        esp_task_wdt_reset();
        size_t bytes = 0;
        esp_err_t err = i2s_channel_read(s_rx, raw, sizeof(raw), &bytes, pdMS_TO_TICKS(READ_TIMEOUT_MS));
        if (err != ESP_OK || bytes != sizeof(raw)) {
            taskENTER_CRITICAL(&s_stats_lock);
            s_stats.read_errors++;
            taskEXIT_CRITICAL(&s_stats_lock);
            ESP_LOGW(TAG, "i2s read: %s (%u bytes)", esp_err_to_name(err), (unsigned)bytes);
            continue;
        }
        int16_t peak = s_stats.peak_abs;
        convert(raw, pcm, CHUNK_SAMPLES, &peak);
        size_t sent = xStreamBufferSend(s_stream, pcm, sizeof(pcm), 0);

        taskENTER_CRITICAL(&s_stats_lock);
        s_stats.samples_captured += CHUNK_SAMPLES;
        s_stats.samples_dropped += (sizeof(pcm) - sent) / sizeof(int16_t);
        if (peak > s_stats.peak_abs) {
            s_stats.peak_abs = peak;
        }
        taskEXIT_CRITICAL(&s_stats_lock);
    }
}

esp_err_t kws_audio_start(void)
{
    ESP_RETURN_ON_FALSE(!s_stream, ESP_ERR_INVALID_STATE, TAG, "already started");
    size_t bytes = BUFFER_SAMPLES * sizeof(int16_t);
    uint8_t *storage = heap_caps_malloc(bytes + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!storage) {
        ESP_LOGW(TAG, "no PSRAM, audio buffer in internal RAM");
        storage = heap_caps_malloc(bytes + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    ESP_RETURN_ON_FALSE(storage, ESP_ERR_NO_MEM, TAG, "audio buffer");
    s_stream = xStreamBufferCreateStatic(bytes + 1, CHUNK_SAMPLES * sizeof(int16_t), storage, &s_stream_struct);

    esp_err_t err = i2s_init();
    if (err != ESP_OK) {
        vStreamBufferDelete(s_stream);
        free(storage);
        s_stream = NULL;
        return err;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(capture_task, "kws_audio", 3072, NULL,
                                            CONFIG_KWS_AUDIO_TASK_PRIORITY, NULL, 1);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task create");
    ESP_LOGI(TAG, "I2S%d mic: BCLK=%d WS=%d DIN=%d, %d ms buffer", CONFIG_KWS_I2S_PORT,
             CONFIG_KWS_I2S_BCLK_GPIO, CONFIG_KWS_I2S_WS_GPIO, CONFIG_KWS_I2S_DIN_GPIO,
             CONFIG_KWS_AUDIO_BUFFER_MS);
    return ESP_OK;
}

size_t kws_audio_read(int16_t *dst, size_t samples, TickType_t timeout)
{
    uint8_t *p = (uint8_t *)dst;
    size_t want = samples * sizeof(int16_t);
    size_t got = 0;
    TimeOut_t t;
    vTaskSetTimeOutState(&t);
    while (got < want) {
        got += xStreamBufferReceive(s_stream, p + got, want - got, timeout);
        if (got < want && xTaskCheckForTimeOut(&t, &timeout) == pdTRUE) {
            break;
        }
    }
    return got / sizeof(int16_t);
}

void kws_audio_get_stats(kws_audio_stats_t *out, bool reset_peak)
{
    taskENTER_CRITICAL(&s_stats_lock);
    *out = s_stats;
    if (reset_peak) {
        s_stats.peak_abs = 0;
    }
    taskEXIT_CRITICAL(&s_stats_lock);
}
