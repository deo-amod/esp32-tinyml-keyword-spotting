/*
 * esp32-tinyml-keyword-spotting: always-on "on"/"off" keyword spotting on ESP32-WROVER-E (N16R8).
 *
 * Core 1: audio capture -> features -> int8 DS-CNN (kws_audio, kws_inference)
 * Core 0: Wi-Fi / BLE provisioning / MQTT / OTA, and this application loop.
 *
 * Every producer posts an app_msg_t to one queue; the loop below owns all
 * side effects (LED, MQTT, settings, restarts), so callbacks never block.
 */
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "app.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "kws_audio.h"
#include "kws_board.h"
#include "kws_inference.h"
#include "kws_model_store.h"
#include "kws_mqtt.h"
#include "kws_net.h"
#include "kws_ota.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "app";

#define QUEUE_LEN          16
#define SELFTEST_MIN_INFER 20

typedef enum {
    MSG_DETECTION,
    MSG_COMMAND,
    MSG_NET_STATE,
    MSG_MQTT_STATE,
    MSG_OTA_DONE,
    MSG_BUTTON,
} msg_type_t;

typedef struct {
    msg_type_t type;
    union {
        kws_detection_t det;
        struct {
            char *json;
            size_t len;
        } cmd;
        kws_net_state_t net;
        bool mqtt_up;
        struct {
            kws_ota_kind_t kind;
            esp_err_t err;
            char id[40];
        } ota;
    };
} app_msg_t;

static app_ctx_t s_app;
static QueueHandle_t s_queue;

static void post(const app_msg_t *m)
{
    if (xQueueSend(s_queue, m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full, dropping message %d", m->type);
        if (m->type == MSG_COMMAND) {
            free(m->cmd.json);
        }
    }
}

/* ---- callbacks from other tasks: copy and post, never block ---- */

static void on_detection(const kws_detection_t *det, void *ctx)
{
    app_msg_t m = {.type = MSG_DETECTION, .det = *det};
    post(&m);
}

static void on_command(const char *payload, size_t len, void *ctx)
{
    char *copy = malloc(len + 1);
    if (!copy) {
        return;
    }
    memcpy(copy, payload, len);
    copy[len] = '\0';
    app_msg_t m = {.type = MSG_COMMAND, .cmd = {copy, len}};
    post(&m);
}

static void on_net_state(kws_net_state_t st, void *ctx)
{
    app_msg_t m = {.type = MSG_NET_STATE, .net = st};
    post(&m);
}

static void on_mqtt_state(bool up, void *ctx)
{
    app_msg_t m = {.type = MSG_MQTT_STATE, .mqtt_up = up};
    post(&m);
}

static void on_ota_done(kws_ota_kind_t kind, esp_err_t err, const char *id, void *ctx)
{
    app_msg_t m = {.type = MSG_OTA_DONE, .ota = {.kind = kind, .err = err}};
    strlcpy(m.ota.id, id ? id : "", sizeof(m.ota.id));
    post(&m);
}

static void on_button(void *ctx)
{
    app_msg_t m = {.type = MSG_BUTTON};
    post(&m);
}

/* ---- handlers on the application loop ---- */

static void handle_detection(const kws_detection_t *det)
{
    ESP_LOGI(TAG, "detected \"%s\" (%.2f)", det->keyword, (double)det->score);
    if (strcmp(det->keyword, "on") == 0) {
        kws_board_led_set(true);
    } else if (strcmp(det->keyword, "off") == 0) {
        kws_board_led_set(false);
    } else {
        kws_board_led_pulse();
    }

    char json[160];
    struct timeval tv;
    gettimeofday(&tv, NULL);
    bool time_valid = tv.tv_sec > 1700000000; /* SNTP has synced */
    snprintf(json, sizeof(json), "{\"keyword\":\"%s\",\"score\":%.3f,\"uptime_ms\":%" PRId64 ",\"ts\":%" PRId64 "}",
             det->keyword, (double)det->score, det->uptime_us / 1000,
             time_valid ? (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000 : (int64_t)0);
    if (kws_mqtt_publish("events", json, 1, false) != ESP_OK) {
        ESP_LOGD(TAG, "event not published (offline)");
    }
}

static void handle_net_state(kws_net_state_t st)
{
    switch (st) {
    case KWS_NET_PROVISIONING:
        kws_board_led_mode(KWS_LED_BLINK_SLOW);
        break;
    case KWS_NET_CONNECTING:
    case KWS_NET_DISCONNECTED:
        kws_board_led_mode(KWS_LED_BLINK_FAST);
        break;
    case KWS_NET_CONNECTED:
        kws_board_led_mode(KWS_LED_STEADY);
        if (!s_app.mqtt_started) {
            s_app.mqtt_started = true;
            esp_err_t err = kws_mqtt_start(&s_app.device, on_command, on_mqtt_state, NULL);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "MQTT not started: %s", esp_err_to_name(err));
            }
        }
        break;
    }
}

static void handle_ota_done(kws_ota_kind_t kind, esp_err_t err, const char *id)
{
    const char *op = kind == KWS_OTA_FIRMWARE ? "ota_firmware" : "ota_model";
    if (err != ESP_OK) {
        app_publish_result(id, op, "error", esp_err_to_name(err));
        return;
    }
    app_publish_result(id, op, "ok", "rebooting");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

/*
 * After a firmware OTA the bootloader runs the new image in PENDING_VERIFY state.
 * Confirm it once inference runs and (if the device has Wi-Fi credentials) MQTT
 * has connected; otherwise roll back to the previous image.
 */
typedef enum { SELFTEST_NONE, SELFTEST_RUNNING, SELFTEST_PASSED } selftest_t;
static selftest_t s_selftest = SELFTEST_NONE;

static void selftest_init(void)
{
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "new firmware: self-test running");
        s_selftest = SELFTEST_RUNNING;
    }
}

static void selftest_check(void)
{
    if (s_selftest != SELFTEST_RUNNING) {
        return;
    }
    kws_inference_stats_t inf;
    kws_inference_get_stats(&inf);
    bool need_mqtt = kws_net_was_provisioned_at_boot() && s_app.device.broker_uri;
    if (inf.inferences >= SELFTEST_MIN_INFER && (!need_mqtt || kws_mqtt_connected())) {
        ESP_LOGI(TAG, "self-test passed, firmware confirmed");
        esp_ota_mark_app_valid_cancel_rollback();
        s_selftest = SELFTEST_PASSED;
    } else if (esp_timer_get_time() - s_app.boot_us > (int64_t)CONFIG_KWS_SELFTEST_CONNECT_TIMEOUT_S * 1000000) {
        ESP_LOGE(TAG, "self-test failed (inferences %" PRIu32 ", mqtt %d): rolling back", inf.inferences,
                 kws_mqtt_connected());
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

static void fatal(const char *what, esp_err_t err)
{
    ESP_LOGE(TAG, "%s failed: %s", what, esp_err_to_name(err));
    if (s_selftest == SELFTEST_RUNNING) {
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
    vTaskDelay(pdMS_TO_TICKS(5000));
    esp_restart();
}

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS layout changed, erasing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    s_app.boot_us = esp_timer_get_time();
    const esp_app_desc_t *desc = esp_app_get_description();
    ESP_LOGI(TAG, "%s %s (IDF %s), reset reason %d", desc->project_name, desc->version, desc->idf_ver,
             esp_reset_reason());

    init_nvs();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_queue = xQueueCreate(QUEUE_LEN, sizeof(app_msg_t));
    ESP_ERROR_CHECK(s_queue ? ESP_OK : ESP_ERR_NO_MEM);
    selftest_init();

    esp_err_t err;
    if ((err = kws_board_init(on_button, NULL)) != ESP_OK) {
        fatal("board", err);
    }
    if ((err = kws_device_config_load(&s_app.device)) != ESP_OK) {
        fatal("device config", err);
    }
    if ((err = kws_model_store_init()) != ESP_OK) {
        fatal("model store", err);
    }
    if ((err = kws_audio_start()) != ESP_OK) {
        fatal("audio", err);
    }
    if ((err = kws_inference_init()) != ESP_OK) {
        fatal("inference", err);
    }
    kws_inference_set_threshold(app_settings_threshold());
    if ((err = kws_inference_start(on_detection, NULL)) != ESP_OK) {
        fatal("inference task", err);
    }

    kws_ota_init(s_app.device.ca_cert, on_ota_done, NULL);
    if ((err = kws_net_start(&s_app.device, on_net_state, NULL)) != ESP_OK) {
        /* Keep spotting keywords offline; connectivity is not required for the core function. */
        ESP_LOGE(TAG, "network start failed: %s (running offline)", esp_err_to_name(err));
    }

    int64_t next_health = esp_timer_get_time() + 10 * 1000000LL;
    for (;;) {
        app_msg_t m;
        if (xQueueReceive(s_queue, &m, pdMS_TO_TICKS(1000)) == pdTRUE) {
            switch (m.type) {
            case MSG_DETECTION:
                handle_detection(&m.det);
                break;
            case MSG_COMMAND:
                app_handle_command(&s_app, m.cmd.json, m.cmd.len);
                free(m.cmd.json);
                break;
            case MSG_NET_STATE:
                handle_net_state(m.net);
                break;
            case MSG_MQTT_STATE:
                if (m.mqtt_up) {
                    app_publish_health(&s_app);
                }
                break;
            case MSG_OTA_DONE:
                handle_ota_done(m.ota.kind, m.ota.err, m.ota.id);
                break;
            case MSG_BUTTON:
                kws_net_forget_wifi_and_restart();
                break;
            }
        }
        selftest_check();
        int64_t now = esp_timer_get_time();
        if (now >= next_health) {
            app_publish_health(&s_app);
            next_health = now + (int64_t)CONFIG_KWS_HEALTH_INTERVAL_S * 1000000;
        }
    }
}
