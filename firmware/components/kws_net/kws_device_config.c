#include "kws_device_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_srp.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "kws_devcfg";

#define FCTRY_PARTITION "fctry"
#define FCTRY_NAMESPACE "kws"

static char *get_str(nvs_handle_t h, const char *key)
{
    size_t len = 0;
    if (nvs_get_str(h, key, NULL, &len) != ESP_OK || len == 0) {
        return NULL;
    }
    char *s = malloc(len);
    if (s && nvs_get_str(h, key, s, &len) != ESP_OK) {
        free(s);
        s = NULL;
    }
    return s;
}

static uint8_t *get_blob(nvs_handle_t h, const char *key, size_t *len)
{
    *len = 0;
    if (nvs_get_blob(h, key, NULL, len) != ESP_OK || *len == 0) {
        return NULL;
    }
    uint8_t *b = malloc(*len);
    if (b && nvs_get_blob(h, key, b, len) != ESP_OK) {
        free(b);
        b = NULL;
        *len = 0;
    }
    return b;
}

static esp_err_t init_fctry(void)
{
#if CONFIG_NVS_ENCRYPTION
    nvs_sec_cfg_t sec;
    const nvs_sec_scheme_t *scheme = nvs_flash_get_default_security_scheme();
    ESP_RETURN_ON_FALSE(scheme, ESP_ERR_INVALID_STATE, TAG, "no NVS security scheme");
    ESP_RETURN_ON_ERROR(nvs_flash_read_security_cfg_v2(scheme, &sec), TAG, "read NVS keys");
    return nvs_flash_secure_init_partition(FCTRY_PARTITION, &sec);
#else
    return nvs_flash_init_partition(FCTRY_PARTITION);
#endif
}

static void load_factory(kws_device_config_t *cfg)
{
    esp_err_t err = init_fctry();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "fctry partition unavailable: %s", esp_err_to_name(err));
        return;
    }
    nvs_handle_t h;
    if (nvs_open_from_partition(FCTRY_PARTITION, FCTRY_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "no factory data: using development defaults");
        return;
    }
    size_t len = sizeof(cfg->device_id);
    if (nvs_get_str(h, "device_id", cfg->device_id, &len) == ESP_OK) {
        cfg->from_factory = true;
    }
    len = sizeof(cfg->prov_user);
    nvs_get_str(h, "prov_user", cfg->prov_user, &len);
    cfg->broker_uri = get_str(h, "broker_uri");
    cfg->ca_cert = get_str(h, "ca_cert");
    cfg->client_cert = get_str(h, "client_cert");
    cfg->client_key = get_str(h, "client_key");
    cfg->prov_salt = get_blob(h, "prov_salt", &cfg->prov_salt_len);
    cfg->prov_verifier = get_blob(h, "prov_verifier", &cfg->prov_verifier_len);
    nvs_close(h);
}

esp_err_t kws_device_config_ensure_prov(kws_device_config_t *cfg)
{
    if (cfg->prov_salt && cfg->prov_verifier) {
        return ESP_OK;
    }
    free(cfg->prov_salt);
    free(cfg->prov_verifier);
    cfg->prov_salt = cfg->prov_verifier = NULL;
    const char *pwd = CONFIG_KWS_PROV_DEV_PASSWORD;
    ESP_RETURN_ON_FALSE(pwd[0] != '\0', ESP_ERR_NOT_FOUND, TAG,
                        "no provisioning credentials: provision the device or set KWS_PROV_DEV_PASSWORD");
    ESP_LOGW(TAG, "using DEVELOPMENT provisioning password from menuconfig");
    char *salt = NULL, *verifier = NULL;
    int verifier_len = 0;
    ESP_RETURN_ON_ERROR(esp_srp_gen_salt_verifier(cfg->prov_user, strlen(cfg->prov_user), pwd, strlen(pwd),
                                                  &salt, 16, &verifier, &verifier_len), TAG, "SRP verifier");
    cfg->prov_salt = (uint8_t *)salt;
    cfg->prov_salt_len = 16;
    cfg->prov_verifier = (uint8_t *)verifier;
    cfg->prov_verifier_len = verifier_len;
    return ESP_OK;
}

esp_err_t kws_device_config_load(kws_device_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    load_factory(cfg);

    if (!cfg->from_factory) {
        uint8_t mac[6];
        ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_STA), TAG, "read MAC");
        snprintf(cfg->device_id, sizeof(cfg->device_id), "kws-%02x%02x%02x%02x%02x%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    if (cfg->prov_user[0] == '\0') {
        strlcpy(cfg->prov_user, CONFIG_KWS_PROV_DEV_USERNAME, sizeof(cfg->prov_user));
    }
    if (!cfg->broker_uri && CONFIG_KWS_MQTT_BROKER_URI[0] != '\0') {
        cfg->broker_uri = strdup(CONFIG_KWS_MQTT_BROKER_URI);
    }
    ESP_LOGI(TAG, "device %s (%s), broker %s, client cert %s", cfg->device_id,
             cfg->from_factory ? "factory data" : "dev defaults", cfg->broker_uri ? cfg->broker_uri : "<none>",
             cfg->client_cert ? "yes" : "no");
    return ESP_OK;
}
