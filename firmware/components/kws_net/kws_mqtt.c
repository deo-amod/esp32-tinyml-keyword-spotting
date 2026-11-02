#include "kws_mqtt.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "sdkconfig.h"

static const char *TAG = "kws_mqtt";

#define TOPIC_MAX   96
#define CMD_MAX_LEN 1024

static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;
static char s_prefix[48];
static char s_cmd_topic[TOPIC_MAX];
static char s_status_topic[TOPIC_MAX];
static kws_mqtt_cmd_cb_t s_cmd_cb;
static kws_mqtt_conn_cb_t s_conn_cb;
static void *s_ctx;

static const char kOfflineMsg[] = "{\"online\":false}";

static void on_mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t ev = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "connected");
        s_connected = true;
        char online[96];
        snprintf(online, sizeof(online), "{\"online\":true,\"fw\":\"%s\"}", esp_app_get_description()->version);
        esp_mqtt_client_publish(s_client, s_status_topic, online, 0, 1, 1);
        esp_mqtt_client_subscribe(s_client, s_cmd_topic, 1);
        if (s_conn_cb) {
            s_conn_cb(true, s_ctx);
        }
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        if (s_connected) {
            ESP_LOGW(TAG, "disconnected");
        }
        s_connected = false;
        if (s_conn_cb) {
            s_conn_cb(false, s_ctx);
        }
        break;
    case MQTT_EVENT_DATA:
        if (ev->topic_len != (int)strlen(s_cmd_topic) || strncmp(ev->topic, s_cmd_topic, ev->topic_len) != 0) {
            break;
        }
        if (ev->current_data_offset != 0 || ev->data_len != ev->total_data_len || ev->total_data_len > CMD_MAX_LEN) {
            ESP_LOGW(TAG, "dropping fragmented/oversized command (%d bytes)", ev->total_data_len);
            break;
        }
        if (s_cmd_cb) {
            s_cmd_cb(ev->data, ev->data_len, s_ctx);
        }
        break;
    case MQTT_EVENT_ERROR:
        if (ev->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGW(TAG, "transport error: tls 0x%x, stack 0x%x, errno %d", ev->error_handle->esp_tls_last_esp_err,
                     ev->error_handle->esp_tls_stack_err, ev->error_handle->esp_transport_sock_errno);
        } else if (ev->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGE(TAG, "connection refused (code %d)", ev->error_handle->connect_return_code);
        }
        break;
    default:
        break;
    }
}

esp_err_t kws_mqtt_start(const kws_device_config_t *cfg, kws_mqtt_cmd_cb_t cmd_cb, kws_mqtt_conn_cb_t conn_cb,
                         void *ctx)
{
    ESP_RETURN_ON_FALSE(!s_client, ESP_ERR_INVALID_STATE, TAG, "already started");
    ESP_RETURN_ON_FALSE(cfg->broker_uri, ESP_ERR_NOT_FOUND, TAG, "no broker configured; MQTT disabled");
    bool tls = strncmp(cfg->broker_uri, "mqtts://", 8) == 0 || strncmp(cfg->broker_uri, "wss://", 6) == 0;
#if !CONFIG_KWS_ALLOW_INSECURE_MQTT
    ESP_RETURN_ON_FALSE(tls, ESP_ERR_NOT_SUPPORTED, TAG, "refusing non-TLS broker %s", cfg->broker_uri);
#endif
    if (!tls) {
        ESP_LOGW(TAG, "INSECURE: MQTT without TLS (development only)");
    }

    snprintf(s_prefix, sizeof(s_prefix), "devices/%s", cfg->device_id);
    snprintf(s_cmd_topic, sizeof(s_cmd_topic), "%s/cmd", s_prefix);
    snprintf(s_status_topic, sizeof(s_status_topic), "%s/status", s_prefix);
    s_cmd_cb = cmd_cb;
    s_conn_cb = conn_cb;
    s_ctx = ctx;

    esp_mqtt_client_config_t mc = {
        .broker.address.uri = cfg->broker_uri,
        .credentials.client_id = cfg->device_id,
        .session.keepalive = 30,
        .session.last_will = {
            .topic = s_status_topic,
            .msg = kOfflineMsg,
            .msg_len = sizeof(kOfflineMsg) - 1,
            .qos = 1,
            .retain = 1,
        },
        .network.reconnect_timeout_ms = 5000,
        .network.timeout_ms = 10000,
        .buffer.size = 2048,
        .task.stack_size = 6144,
    };
    if (tls) {
        if (cfg->ca_cert) {
            mc.broker.verification.certificate = cfg->ca_cert;
        } else {
            mc.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
        }
        if (cfg->client_cert && cfg->client_key) {
            mc.credentials.authentication.certificate = cfg->client_cert;
            mc.credentials.authentication.key = cfg->client_key;
        } else {
            ESP_LOGW(TAG, "no client certificate: broker must allow this device without mTLS");
        }
    }

    s_client = esp_mqtt_client_init(&mc);
    ESP_RETURN_ON_FALSE(s_client, ESP_FAIL, TAG, "client init");
    ESP_RETURN_ON_ERROR(esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_mqtt_event, NULL), TAG, "evt");
    ESP_LOGI(TAG, "connecting to %s as %s", cfg->broker_uri, cfg->device_id);
    return esp_mqtt_client_start(s_client);
}

bool kws_mqtt_connected(void)
{
    return s_connected;
}

esp_err_t kws_mqtt_publish(const char *subtopic, const char *payload, int qos, bool retain)
{
    if (!s_client || !s_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    char topic[TOPIC_MAX];
    snprintf(topic, sizeof(topic), "%s/%s", s_prefix, subtopic);
    int id = esp_mqtt_client_enqueue(s_client, topic, payload, 0, qos, retain, true);
    return id < 0 ? ESP_FAIL : ESP_OK;
}
