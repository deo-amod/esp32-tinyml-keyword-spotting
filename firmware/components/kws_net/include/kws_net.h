/*
 * Wi-Fi station with BLE provisioning (NimBLE, Security 2). When the device is
 * already provisioned the Bluetooth controller memory is released at boot and
 * BLE is never started. Reconnects use exponential back-off.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "kws_device_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KWS_NET_PROVISIONING,
    KWS_NET_CONNECTING,
    KWS_NET_CONNECTED,
    KWS_NET_DISCONNECTED,
} kws_net_state_t;

typedef void (*kws_net_state_cb_t)(kws_net_state_t state, void *ctx);

/* `cfg` must stay valid for the lifetime of the program. */
esp_err_t kws_net_start(kws_device_config_t *cfg, kws_net_state_cb_t cb, void *ctx);

bool kws_net_was_provisioned_at_boot(void);
int kws_net_rssi(void);

/* Erase stored Wi-Fi credentials and restart into provisioning mode. */
void kws_net_forget_wifi_and_restart(void);

#ifdef __cplusplus
}
#endif
