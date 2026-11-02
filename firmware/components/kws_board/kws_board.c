#include "kws_board.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "kws_board";

#define TICK_MS  50
#define PULSE_MS 150

static volatile kws_led_mode_t s_mode = KWS_LED_STEADY;
static volatile bool s_level;
static volatile int s_pulse_ms;
static kws_button_cb_t s_cb;
static void *s_cb_ctx;

static void led_write(bool on)
{
#if CONFIG_KWS_LED_GPIO >= 0
#if CONFIG_KWS_LED_ACTIVE_HIGH
    gpio_set_level(CONFIG_KWS_LED_GPIO, on);
#else
    gpio_set_level(CONFIG_KWS_LED_GPIO, !on);
#endif
#endif
}

static void board_task(void *arg)
{
    uint32_t t = 0;
    uint32_t held_ms = 0;
    bool fired = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        t += TICK_MS;

        bool on;
        if (s_pulse_ms > 0) {
            s_pulse_ms -= TICK_MS;
            on = !s_level;
        } else if (s_mode == KWS_LED_BLINK_SLOW) {
            on = (t % 1000) < 500;
        } else if (s_mode == KWS_LED_BLINK_FAST) {
            on = (t % 200) < 100;
        } else {
            on = s_level;
        }
        led_write(on);

        if (gpio_get_level(CONFIG_KWS_BUTTON_GPIO) == 0) {
            held_ms += TICK_MS;
            if (!fired && held_ms >= CONFIG_KWS_FACTORY_RESET_HOLD_MS) {
                fired = true;
                ESP_LOGW(TAG, "button held %d ms", CONFIG_KWS_FACTORY_RESET_HOLD_MS);
                if (s_cb) {
                    s_cb(s_cb_ctx);
                }
            }
        } else {
            held_ms = 0;
            fired = false;
        }
    }
}

esp_err_t kws_board_init(kws_button_cb_t on_long_press, void *ctx)
{
    s_cb = on_long_press;
    s_cb_ctx = ctx;
    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << CONFIG_KWS_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&btn), TAG, "button gpio");
#if CONFIG_KWS_LED_GPIO >= 0
    gpio_config_t led = {
        .pin_bit_mask = 1ULL << CONFIG_KWS_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&led), TAG, "led gpio");
    led_write(false);
#endif
    BaseType_t ok = xTaskCreatePinnedToCore(board_task, "kws_board", 2560, NULL, 2, NULL, 0);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void kws_board_led_mode(kws_led_mode_t mode)
{
    s_mode = mode;
}

void kws_board_led_set(bool on)
{
    s_level = on;
}

void kws_board_led_pulse(void)
{
    s_pulse_ms = PULSE_MS;
}
