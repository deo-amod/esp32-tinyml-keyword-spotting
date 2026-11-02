/*
 * Over-the-air updates over HTTPS, one at a time, on a background task.
 *  - firmware: esp_https_ota into the inactive app slot; bootloader rollback protects it
 *  - model:    .kwsm into the inactive model slot; SHA-256 + signature checked before use
 * Both take effect after a reboot, which the caller performs from the done callback.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KWS_OTA_FIRMWARE,
    KWS_OTA_MODEL,
} kws_ota_kind_t;

typedef void (*kws_ota_done_cb_t)(kws_ota_kind_t kind, esp_err_t result, const char *request_id, void *ctx);

/* `ca_pem` (may be NULL: use the IDF certificate bundle) must outlive all updates. */
void kws_ota_init(const char *ca_pem, kws_ota_done_cb_t cb, void *ctx);

esp_err_t kws_ota_start_firmware(const char *url, const char *request_id);
esp_err_t kws_ota_start_model(const char *url, const char *sha256_hex, const char *request_id);

bool kws_ota_busy(void);

#ifdef __cplusplus
}
#endif
