#include "kws_inference.h"

#include <cmath>
#include <cstring>
#include <new>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kws_audio.h"
#include "kws_detector.h"
#include "kws_features.h"
#include "kws_model_store.h"
#include "sdkconfig.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

namespace {

const char *TAG = "kws_inference";

constexpr size_t kArenaSize = CONFIG_KWS_TENSOR_ARENA_KB * 1024;
constexpr int kFirstKeyword = 2;  // labels 0/1 are _silence_ / _unknown_

using KwsOpResolver = tflite::MicroMutableOpResolver<10>;
KwsOpResolver g_resolver;
alignas(tflite::MicroInterpreter) uint8_t g_interpreter_storage[sizeof(tflite::MicroInterpreter)];
tflite::MicroInterpreter *g_interpreter = nullptr;
uint8_t *g_arena = nullptr;

kws_model_t g_model;
int8_t g_requant[256];  // feature int8 -> model input int8
float g_out_scale;
int g_out_zp;
int g_num_labels;

int8_t g_frames[KWS_FEATURE_FRAMES][KWS_FEATURE_CHANNELS];  // ring of requantised frames
int g_frame_head;  // index of the oldest frame

kws_detector_t g_detector;
portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;
kws_inference_stats_t g_stats;
kws_detection_cb_t g_cb;
void *g_cb_ctx;

TfLiteStatus register_ops(KwsOpResolver &r)
{
    TF_LITE_ENSURE_STATUS(r.AddConv2D());
    TF_LITE_ENSURE_STATUS(r.AddDepthwiseConv2D());
    TF_LITE_ENSURE_STATUS(r.AddAveragePool2D());
    TF_LITE_ENSURE_STATUS(r.AddMaxPool2D());
    TF_LITE_ENSURE_STATUS(r.AddMean());
    TF_LITE_ENSURE_STATUS(r.AddReshape());
    TF_LITE_ENSURE_STATUS(r.AddFullyConnected());
    TF_LITE_ENSURE_STATUS(r.AddSoftmax());
    TF_LITE_ENSURE_STATUS(r.AddQuantize());
    TF_LITE_ENSURE_STATUS(r.AddDequantize());
    return kTfLiteOk;
}

void destroy_interpreter()
{
    if (g_interpreter) {
        g_interpreter->~MicroInterpreter();
        g_interpreter = nullptr;
    }
}

/* Feature values are int8 in [-128, 127]; the model input has its own scale/zero-point. */
void build_requant_lut(float scale, int zero_point)
{
    for (int v = -128; v <= 127; v++) {
        long q = lroundf((float)v / scale) + zero_point;
        g_requant[(uint8_t)(int8_t)v] = (int8_t)(q < -128 ? -128 : q > 127 ? 127 : q);
    }
}

esp_err_t check_header(const kws_model_header_t *h)
{
    ESP_RETURN_ON_FALSE(h->feature_frames == KWS_FEATURE_FRAMES && h->feature_channels == KWS_FEATURE_CHANNELS &&
                        h->window_ms == KWS_FEATURE_WINDOW_MS && h->stride_ms == KWS_FEATURE_STRIDE_MS &&
                        h->sample_rate == KWS_FEATURE_SAMPLE_RATE,
                        ESP_ERR_NOT_SUPPORTED, TAG, "model feature config %ux%u %u/%u ms %lu Hz does not match firmware",
                        h->feature_frames, h->feature_channels, h->window_ms, h->stride_ms,
                        (unsigned long)h->sample_rate);
    ESP_RETURN_ON_FALSE(h->num_labels > kFirstKeyword && h->num_labels <= KWS_DETECTOR_MAX_LABELS,
                        ESP_ERR_NOT_SUPPORTED, TAG, "model has %u labels", h->num_labels);
    return ESP_OK;
}

esp_err_t try_load(kws_model_source_t src)
{
    ESP_RETURN_ON_ERROR(kws_model_open(src, &g_model), TAG, "open %s", kws_model_source_name(src));
    esp_err_t err = check_header(g_model.header);
    const tflite::Model *model = nullptr;
    if (err == ESP_OK) {
        model = tflite::GetModel(g_model.tflite);
        if (model->version() != TFLITE_SCHEMA_VERSION) {
            ESP_LOGE(TAG, "schema version %lu", (unsigned long)model->version());
            err = ESP_ERR_INVALID_VERSION;
        }
    }
    if (err == ESP_OK) {
        g_interpreter = new (g_interpreter_storage) tflite::MicroInterpreter(model, g_resolver, g_arena, kArenaSize);
        if (g_interpreter->AllocateTensors() != kTfLiteOk) {
            ESP_LOGE(TAG, "AllocateTensors failed (unsupported op or arena < model needs)");
            err = ESP_ERR_NO_MEM;
        }
    }
    if (err == ESP_OK) {
        TfLiteTensor *in = g_interpreter->input(0);
        TfLiteTensor *out = g_interpreter->output(0);
        if (in->type != kTfLiteInt8 || in->bytes != sizeof(g_frames) || out->type != kTfLiteInt8 ||
            out->bytes != g_model.header->num_labels) {
            ESP_LOGE(TAG, "unexpected tensors: in %d/%u bytes, out %d/%u bytes", in->type, (unsigned)in->bytes,
                     out->type, (unsigned)out->bytes);
            err = ESP_ERR_INVALID_SIZE;
        } else {
            build_requant_lut(in->params.scale, in->params.zero_point);
            g_out_scale = out->params.scale;
            g_out_zp = out->params.zero_point;
            memset(in->data.int8, 0, in->bytes);
            if (g_interpreter->Invoke() != kTfLiteOk) {
                ESP_LOGE(TAG, "test inference failed");
                err = ESP_FAIL;
            }
        }
    }
    if (err != ESP_OK) {
        destroy_interpreter();
        kws_model_close(&g_model);
    }
    return err;
}

void inference_task(void *)
{
    static int16_t pcm[KWS_FEATURE_STRIDE_SAMPLES];
    int8_t feat[KWS_FEATURE_CHANNELS];
    float probs[KWS_DETECTOR_MAX_LABELS];
    uint32_t frames_seen = 0;
    uint32_t since_inference = 0;
    TfLiteTensor *input = g_interpreter->input(0);
    TfLiteTensor *output = g_interpreter->output(0);
    ESP_ERROR_CHECK(esp_task_wdt_add(nullptr));

    for (;;) {
        esp_task_wdt_reset();
        if (kws_audio_read(pcm, KWS_FEATURE_STRIDE_SAMPLES, pdMS_TO_TICKS(1000)) != KWS_FEATURE_STRIDE_SAMPLES) {
            ESP_LOGW(TAG, "no audio for 1 s");
            continue;
        }
        if (kws_features_process(pcm, feat) != ESP_OK) {
            ESP_LOGE(TAG, "feature extraction failed");
            continue;
        }
        int8_t *slot = g_frames[g_frame_head];
        for (int c = 0; c < KWS_FEATURE_CHANNELS; c++) {
            slot[c] = g_requant[(uint8_t)feat[c]];
        }
        g_frame_head = (g_frame_head + 1) % KWS_FEATURE_FRAMES;
        if (frames_seen < KWS_FEATURE_FRAMES) {
            frames_seen++;  // the window is full once a second of audio has been seen
            continue;
        }
        if (++since_inference < CONFIG_KWS_INFERENCE_EVERY_N_FRAMES) {
            continue;
        }
        since_inference = 0;

        // Oldest frame first, as in training.
        size_t tail = (KWS_FEATURE_FRAMES - g_frame_head) * KWS_FEATURE_CHANNELS;
        memcpy(input->data.int8, g_frames[g_frame_head], tail);
        memcpy(input->data.int8 + tail, g_frames[0], g_frame_head * KWS_FEATURE_CHANNELS);

        int64_t t0 = esp_timer_get_time();
        if (g_interpreter->Invoke() != kTfLiteOk) {
            ESP_LOGE(TAG, "Invoke failed");
            continue;
        }
        int64_t now = esp_timer_get_time();
        uint32_t dt = (uint32_t)(now - t0);
        for (int i = 0; i < g_num_labels; i++) {
            probs[i] = (output->data.int8[i] - g_out_zp) * g_out_scale;
        }

        int label;
        float score;
        taskENTER_CRITICAL(&g_lock);
        bool hit = kws_detector_update(&g_detector, probs, (uint32_t)(now / 1000), &label, &score);
        g_stats.inferences++;
        g_stats.last_us = dt;
        g_stats.avg_us = g_stats.avg_us ? (g_stats.avg_us * 15 + dt) / 16 : dt;
        if (dt > g_stats.max_us) {
            g_stats.max_us = dt;
        }
        if (hit) {
            g_stats.detections++;
        }
        taskEXIT_CRITICAL(&g_lock);

        if (hit && g_cb) {
            kws_detection_t det = {label, g_model.header->labels[label], score, now};
            g_cb(&det, g_cb_ctx);
        }
    }
}

}  // namespace

extern "C" esp_err_t kws_inference_init(void)
{
    ESP_RETURN_ON_FALSE(!g_interpreter, ESP_ERR_INVALID_STATE, TAG, "already initialised");
    ESP_RETURN_ON_ERROR(kws_features_init(), TAG, "features");
    ESP_RETURN_ON_FALSE(register_ops(g_resolver) == kTfLiteOk, ESP_FAIL, TAG, "register ops");
    // Activations live in internal RAM: PSRAM is several times slower for the hot loop.
    g_arena = (uint8_t *)heap_caps_aligned_alloc(16, kArenaSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(g_arena, ESP_ERR_NO_MEM, TAG, "tensor arena (%u KB)", CONFIG_KWS_TENSOR_ARENA_KB);

    kws_model_source_t order[3];
    size_t n = kws_model_boot_order(order);
    for (size_t i = 0; i < n; i++) {
        if (try_load(order[i]) == ESP_OK) {
            kws_model_mark_running(order[i]);
            g_num_labels = g_model.header->num_labels;
            kws_detector_config_t dcfg = {
                .threshold = CONFIG_KWS_DETECTION_THRESHOLD_PCT / 100.0f,
                .window_ms = CONFIG_KWS_AVERAGE_WINDOW_MS,
                .min_results = CONFIG_KWS_MIN_RESULTS,
                .suppression_ms = CONFIG_KWS_SUPPRESSION_MS,
                .first_keyword = kFirstKeyword,
            };
            kws_detector_init(&g_detector, &dcfg, (uint8_t)g_num_labels);
            g_stats.model_version = g_model.header->model_version;
            g_stats.model_source = kws_model_source_name(order[i]);
            g_stats.arena_used = g_interpreter->arena_used_bytes();
            g_stats.threshold = dcfg.threshold;
            ESP_LOGI(TAG, "model v%lu from %s, arena %lu/%u bytes", (unsigned long)g_stats.model_version,
                     g_stats.model_source, (unsigned long)g_stats.arena_used, (unsigned)kArenaSize);
            return ESP_OK;
        }
        kws_model_mark_failed(order[i]);
    }
    return ESP_ERR_NOT_FOUND;
}

extern "C" esp_err_t kws_inference_start(kws_detection_cb_t cb, void *ctx)
{
    ESP_RETURN_ON_FALSE(g_interpreter, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    g_cb = cb;
    g_cb_ctx = ctx;
    BaseType_t ok = xTaskCreatePinnedToCore(inference_task, "kws_infer", 6144, nullptr,
                                            CONFIG_KWS_INFERENCE_TASK_PRIORITY, nullptr, 1);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

extern "C" void kws_inference_set_threshold(float threshold)
{
    taskENTER_CRITICAL(&g_lock);
    g_detector.cfg.threshold = threshold;
    g_stats.threshold = threshold;
    taskEXIT_CRITICAL(&g_lock);
}

extern "C" void kws_inference_get_stats(kws_inference_stats_t *out)
{
    taskENTER_CRITICAL(&g_lock);
    *out = g_stats;
    taskEXIT_CRITICAL(&g_lock);
}
