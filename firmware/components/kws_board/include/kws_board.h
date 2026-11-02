/*
 * Board I/O: status LED and the BOOT button (long press = forget Wi-Fi).
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KWS_LED_STEADY,      /* follows kws_board_led_set() */
    KWS_LED_BLINK_SLOW,  /* waiting for BLE provisioning */
    KWS_LED_BLINK_FAST,  /* connecting */
} kws_led_mode_t;

typedef void (*kws_button_cb_t)(void *ctx);

esp_err_t kws_board_init(kws_button_cb_t on_long_press, void *ctx);

void kws_board_led_mode(kws_led_mode_t mode);
void kws_board_led_set(bool on);
/* Short flash, then back to the current mode. */
void kws_board_led_pulse(void);

#ifdef __cplusplus
}
#endif
