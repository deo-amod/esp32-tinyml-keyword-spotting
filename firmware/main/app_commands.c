/*
 * Remote commands on devices/<id>/cmd, results on devices/<id>/cmd/result.
 *
 *   {"id":"42","op":"ping"}
 *   {"id":"43","op":"set_threshold","value":0.92}
 *   {"id":"44","op":"ota_firmware","url":"https://host/esp32_tinyml_keyword_spotting.bin"}
 *   {"id":"45","op":"ota_model","url":"https://host/kws_model_v2.kwsm","sha256":"<64 hex>"}
 *   {"id":"46","op":"get_health"} | {"op":"reboot"} | {"op":"forget_wifi"}
 */
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kws_inference.h"
#include "kws_mqtt.h"
#include "kws_net.h"
#include "kws_ota.h"

static const char *TAG = "commands";

void app_publish_result(const char *id, const char *op, const char *status, const char *detail)
{
    cJSON *r = cJSON_CreateObject();
    if (!r) {
        return;
    }
    cJSON_AddStringToObject(r, "id", id ? id : "");
    cJSON_AddStringToObject(r, "op", op ? op : "");
    cJSON_AddStringToObject(r, "status", status);
    if (detail) {
        cJSON_AddStringToObject(r, "detail", detail);
    }
    char *s = cJSON_PrintUnformatted(r);
    if (s) {
        ESP_LOGI(TAG, "result %s", s);
        kws_mqtt_publish("cmd/result", s, 1, false);
        cJSON_free(s);
    }
    cJSON_Delete(r);
}

static const char *str_field(const cJSON *o, const char *name)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, name);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static void reply_err(const char *id, const char *op, esp_err_t err)
{
    app_publish_result(id, op, "error", esp_err_to_name(err));
}

/* Give the result a moment to leave the outbox before restarting. */
static void delayed_restart(void)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

void app_handle_command(app_ctx_t *app, const char *json, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!cJSON_IsObject(root)) {
        ESP_LOGW(TAG, "malformed command");
        app_publish_result(NULL, NULL, "error", "malformed JSON");
        cJSON_Delete(root);
        return;
    }
    const char *id = str_field(root, "id");
    const char *op = str_field(root, "op");
    ESP_LOGI(TAG, "command op=%s id=%s", op ? op : "?", id ? id : "");

    if (!op) {
        app_publish_result(id, NULL, "error", "missing op");
    } else if (strcmp(op, "ping") == 0) {
        app_publish_result(id, op, "ok", "pong");
    } else if (strcmp(op, "get_health") == 0) {
        app_publish_health(app);
        app_publish_result(id, op, "ok", NULL);
    } else if (strcmp(op, "set_threshold") == 0) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "value");
        esp_err_t err = cJSON_IsNumber(v) ? app_settings_set_threshold((float)v->valuedouble) : ESP_ERR_INVALID_ARG;
        if (err == ESP_OK) {
            kws_inference_set_threshold((float)v->valuedouble);
            app_publish_result(id, op, "ok", NULL);
        } else {
            app_publish_result(id, op, "error", "value must be a number in [0.5, 0.99]");
        }
    } else if (strcmp(op, "ota_firmware") == 0) {
        esp_err_t err = kws_ota_start_firmware(str_field(root, "url"), id);
        err == ESP_OK ? app_publish_result(id, op, "accepted", NULL) : reply_err(id, op, err);
    } else if (strcmp(op, "ota_model") == 0) {
        esp_err_t err = kws_ota_start_model(str_field(root, "url"), str_field(root, "sha256"), id);
        err == ESP_OK ? app_publish_result(id, op, "accepted", NULL) : reply_err(id, op, err);
    } else if (strcmp(op, "reboot") == 0) {
        app_publish_result(id, op, "ok", NULL);
        delayed_restart();
    } else if (strcmp(op, "forget_wifi") == 0) {
        app_publish_result(id, op, "ok", NULL);
        vTaskDelay(pdMS_TO_TICKS(1000));
        kws_net_forget_wifi_and_restart();
    } else {
        app_publish_result(id, op, "error", "unknown op");
    }
    cJSON_Delete(root);
}
