#include "app.h"

#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "settings";

#define NVS_NAMESPACE "kws_app"
#define KEY_THRESHOLD "threshold_bp" /* basis points: 9000 = 0.90 */

float app_settings_threshold(void)
{
    uint16_t bp = CONFIG_KWS_DETECTION_THRESHOLD_PCT * 100;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u16(h, KEY_THRESHOLD, &bp);
        nvs_close(h);
    }
    return bp / 10000.0f;
}

esp_err_t app_settings_set_threshold(float threshold)
{
    if (threshold < 0.5f || threshold > 0.99f) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u16(h, KEY_THRESHOLD, (uint16_t)(threshold * 10000.0f + 0.5f));
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "persist threshold: %s", esp_err_to_name(err));
    }
    return err;
}
