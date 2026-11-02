#include "kws_net.h"

#include <stdio.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "sdkconfig.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"

static const char *TAG = "kws_wifi";

#define BACKOFF_MIN_MS    1000
#define BACKOFF_MAX_MS    60000
#define PROV_MAX_FAILURES 3

static kws_device_config_t *s_cfg;
static kws_net_state_cb_t s_cb;
static void *s_cb_ctx;
static esp_timer_handle_t s_reconnect_timer;
static uint32_t s_backoff_ms = BACKOFF_MIN_MS;
static bool s_provisioning;
static bool s_provisioned_at_boot;
static bool s_sntp_started;
static int s_prov_failures;
static wifi_prov_security2_params_t s_sec2;

static void set_state(kws_net_state_t st)
{
    if (s_cb) {
        s_cb(st, s_cb_ctx);
    }
}

static void reconnect_cb(void *arg)
{
    ESP_LOGI(TAG, "reconnecting");
    set_state(KWS_NET_CONNECTING);
    esp_wifi_connect();
}

static void schedule_reconnect(void)
{
    ESP_LOGW(TAG, "retry in %lu ms", (unsigned long)s_backoff_ms);
    esp_timer_stop(s_reconnect_timer);
    esp_timer_start_once(s_reconnect_timer, (uint64_t)s_backoff_ms * 1000);
    s_backoff_ms = (s_backoff_ms * 2 > BACKOFF_MAX_MS) ? BACKOFF_MAX_MS : s_backoff_ms * 2;
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_PROV_EVENT) {
        switch (id) {
        case WIFI_PROV_START:
            ESP_LOGI(TAG, "BLE provisioning started");
            set_state(KWS_NET_PROVISIONING);
            break;
        case WIFI_PROV_CRED_RECV: {
            const wifi_sta_config_t *sta = data;
            ESP_LOGI(TAG, "received credentials for SSID \"%s\"", (const char *)sta->ssid);
            set_state(KWS_NET_CONNECTING);
            break;
        }
        case WIFI_PROV_CRED_FAIL: {
            const wifi_prov_sta_fail_reason_t *reason = data;
            ESP_LOGE(TAG, "provisioning failed: %s",
                     *reason == WIFI_PROV_STA_AUTH_ERROR ? "authentication error" : "access point not found");
            if (++s_prov_failures >= PROV_MAX_FAILURES) {
                ESP_LOGW(TAG, "too many failures, resetting provisioning state");
                wifi_prov_mgr_reset_sm_state_on_failure();
                s_prov_failures = 0;
            }
            set_state(KWS_NET_PROVISIONING);
            break;
        }
        case WIFI_PROV_CRED_SUCCESS:
            ESP_LOGI(TAG, "provisioning successful");
            s_prov_failures = 0;
            break;
        case WIFI_PROV_END:
            /* Frees the BLE stack and controller memory (scheme handler FREE_BTDM). */
            wifi_prov_mgr_deinit();
            s_provisioning = false;
            break;
        default:
            break;
        }
    } else if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            set_state(KWS_NET_CONNECTING);
            esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            const wifi_event_sta_disconnected_t *d = data;
            ESP_LOGW(TAG, "disconnected (reason %d)", d->reason);
            set_state(s_provisioning ? KWS_NET_PROVISIONING : KWS_NET_DISCONNECTED);
            if (s_provisioning) {
                esp_wifi_connect();  /* the provisioning manager reports the outcome */
            } else {
                schedule_reconnect();
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ip = data;
        ESP_LOGI(TAG, "connected, IP " IPSTR, IP2STR(&ip->ip_info.ip));
        s_backoff_ms = BACKOFF_MIN_MS;
        if (!s_sntp_started) {
            esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_KWS_SNTP_SERVER);
            s_sntp_started = esp_netif_sntp_init(&sntp) == ESP_OK;
        }
        set_state(KWS_NET_CONNECTED);
    }
}

static esp_err_t start_provisioning(void)
{
    ESP_RETURN_ON_ERROR(kws_device_config_ensure_prov(s_cfg), TAG, "provisioning credentials");
    s_sec2.salt = (const char *)s_cfg->prov_salt;
    s_sec2.salt_len = s_cfg->prov_salt_len;
    s_sec2.verifier = (const char *)s_cfg->prov_verifier;
    s_sec2.verifier_len = s_cfg->prov_verifier_len;

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char service_name[16];
    snprintf(service_name, sizeof(service_name), "KWS_%02X%02X%02X", mac[3], mac[4], mac[5]);
    /* Custom 128-bit service UUID; the provisioning app discovers devices by name prefix. */
    static uint8_t uuid[16] = {0xb4, 0xdf, 0x5a, 0x1c, 0x3f, 0x6b, 0xf4, 0xbf,
                               0xea, 0x4a, 0x82, 0x03, 0x04, 0x90, 0x1a, 0x02};
    ESP_RETURN_ON_ERROR(wifi_prov_scheme_ble_set_service_uuid(uuid), TAG, "service uuid");
    s_provisioning = true;
    ESP_LOGW(TAG, "provisioning as \"%s\", user \"%s\"", service_name, s_cfg->prov_user);
    return wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_2, &s_sec2, service_name, NULL);
}

esp_err_t kws_net_start(kws_device_config_t *cfg, kws_net_state_cb_t cb, void *ctx)
{
    s_cfg = cfg;
    s_cb = cb;
    s_cb_ctx = ctx;

    /* Classic BT is never used: return its controller memory to the heap. */
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wcfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, on_event, NULL), TAG, "evt");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL), TAG, "evt");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL), TAG, "evt");
    const esp_timer_create_args_t targs = {.callback = reconnect_cb, .name = "wifi_reconnect"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&targs, &s_reconnect_timer), TAG, "timer");

    wifi_prov_mgr_config_t pcfg = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
    };
    ESP_RETURN_ON_ERROR(wifi_prov_mgr_init(pcfg), TAG, "prov init");
    ESP_RETURN_ON_ERROR(wifi_prov_mgr_is_provisioned(&s_provisioned_at_boot), TAG, "prov state");

    if (!s_provisioned_at_boot) {
        return start_provisioning();
    }
    ESP_LOGI(TAG, "already provisioned, BLE disabled");
    wifi_prov_mgr_deinit();
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_MIN_MODEM), TAG, "power save");
    return esp_wifi_start();
}

bool kws_net_was_provisioned_at_boot(void)
{
    return s_provisioned_at_boot;
}

int kws_net_rssi(void)
{
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

void kws_net_forget_wifi_and_restart(void)
{
    ESP_LOGW(TAG, "erasing Wi-Fi credentials");
    esp_wifi_restore();
    esp_restart();
}
