#include "kws_ota.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kws_model_store.h"

static const char *TAG = "kws_ota";

#define URL_MAX    256
#define ID_MAX     40
#define CHUNK      2048
#define TIMEOUT_MS 15000

typedef struct {
    kws_ota_kind_t kind;
    char url[URL_MAX];
    char id[ID_MAX];
    uint8_t sha256[32];
} ota_job_t;

static const char *s_ca_pem;
static kws_ota_done_cb_t s_cb;
static void *s_cb_ctx;
static atomic_bool s_busy;

static void http_config(esp_http_client_config_t *c, const char *url)
{
    memset(c, 0, sizeof(*c));
    c->url = url;
    c->timeout_ms = TIMEOUT_MS;
    c->keep_alive_enable = true;
    c->buffer_size = CHUNK;
    if (s_ca_pem) {
        c->cert_pem = s_ca_pem;
    } else {
        c->crt_bundle_attach = esp_crt_bundle_attach;
    }
}

static esp_err_t validate_image(esp_https_ota_handle_t h)
{
    esp_app_desc_t incoming;
    ESP_RETURN_ON_ERROR(esp_https_ota_get_img_desc(h, &incoming), TAG, "image header");
    const esp_app_desc_t *running = esp_app_get_description();
    ESP_RETURN_ON_FALSE(strncmp(incoming.project_name, running->project_name, sizeof(incoming.project_name)) == 0,
                        ESP_ERR_INVALID_VERSION, TAG, "image is for project '%s'", incoming.project_name);
    ESP_LOGI(TAG, "firmware %s -> %s", running->version, incoming.version);
    return ESP_OK;
}

static esp_err_t do_firmware(const ota_job_t *job)
{
    esp_http_client_config_t http;
    http_config(&http, job->url);
    esp_https_ota_config_t cfg = {.http_config = &http};
    esp_https_ota_handle_t h = NULL;
    ESP_RETURN_ON_ERROR(esp_https_ota_begin(&cfg, &h), TAG, "begin");

    esp_err_t err = validate_image(h);
    while (err == ESP_OK) {
        err = esp_https_ota_perform(h);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
    }
    if (err == ESP_OK && !esp_https_ota_is_complete_data_received(h)) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err != ESP_OK) {
        esp_https_ota_abort(h);
        return err;
    }
    /* Verifies the image (and its Secure Boot signature in release builds) and sets the boot slot. */
    return esp_https_ota_finish(h);
}

static esp_err_t do_model(const ota_job_t *job)
{
    esp_http_client_config_t http;
    http_config(&http, job->url);
    esp_http_client_handle_t client = esp_http_client_init(&http);
    ESP_RETURN_ON_FALSE(client, ESP_FAIL, TAG, "http init");

    kws_model_writer_t *w = NULL;
    uint8_t *buf = malloc(CHUNK);
    esp_err_t err = buf ? esp_http_client_open(client, 0) : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        int64_t len = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status != 200 || len <= 0) {
            ESP_LOGE(TAG, "model download: HTTP %d, length %lld", status, (long long)len);
            err = ESP_ERR_INVALID_RESPONSE;
        } else {
            err = kws_model_writer_begin((size_t)len, &w);
        }
    }
    while (err == ESP_OK) {
        int n = esp_http_client_read(client, (char *)buf, CHUNK);
        if (n < 0) {
            err = ESP_FAIL;
        } else if (n == 0) {
            break;
        } else {
            err = kws_model_writer_write(w, buf, n);
        }
    }
    if (err == ESP_OK && !esp_http_client_is_complete_data_received(client)) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (w) {
        if (err == ESP_OK) {
            err = kws_model_writer_finish(w, job->sha256);
        } else {
            kws_model_writer_abort(w);
        }
    }
    free(buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

static void ota_task(void *arg)
{
    ota_job_t *job = arg;
    ESP_LOGI(TAG, "%s update from %s", job->kind == KWS_OTA_FIRMWARE ? "firmware" : "model", job->url);
    esp_err_t err = (job->kind == KWS_OTA_FIRMWARE) ? do_firmware(job) : do_model(job);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "update staged");
    } else {
        ESP_LOGE(TAG, "update failed: %s", esp_err_to_name(err));
    }
    if (s_cb) {
        s_cb(job->kind, err, job->id, s_cb_ctx);
    }
    free(job);
    atomic_store(&s_busy, false);
    vTaskDelete(NULL);
}

static esp_err_t start(ota_job_t *job, const char *url, const char *request_id)
{
    ESP_RETURN_ON_FALSE(url && strncmp(url, "https://", 8) == 0 && strlen(url) < URL_MAX, ESP_ERR_INVALID_ARG, TAG,
                        "update URL must be https:// and shorter than %d", URL_MAX);
    bool expected = false;
    ESP_RETURN_ON_FALSE(atomic_compare_exchange_strong(&s_busy, &expected, true), ESP_ERR_INVALID_STATE, TAG,
                        "update already in progress");
    strlcpy(job->url, url, sizeof(job->url));
    strlcpy(job->id, request_id ? request_id : "", sizeof(job->id));
    if (xTaskCreatePinnedToCore(ota_task, "kws_ota", 8192, job, 4, NULL, 0) != pdPASS) {
        atomic_store(&s_busy, false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void kws_ota_init(const char *ca_pem, kws_ota_done_cb_t cb, void *ctx)
{
    s_ca_pem = ca_pem;
    s_cb = cb;
    s_cb_ctx = ctx;
}

esp_err_t kws_ota_start_firmware(const char *url, const char *request_id)
{
    ota_job_t *job = calloc(1, sizeof(*job));
    ESP_RETURN_ON_FALSE(job, ESP_ERR_NO_MEM, TAG, "no memory");
    job->kind = KWS_OTA_FIRMWARE;
    esp_err_t err = start(job, url, request_id);
    if (err != ESP_OK) {
        free(job);
    }
    return err;
}

esp_err_t kws_ota_start_model(const char *url, const char *sha256_hex, const char *request_id)
{
    ESP_RETURN_ON_FALSE(sha256_hex && strlen(sha256_hex) == 64, ESP_ERR_INVALID_ARG, TAG, "sha256 must be 64 hex chars");
    ota_job_t *job = calloc(1, sizeof(*job));
    ESP_RETURN_ON_FALSE(job, ESP_ERR_NO_MEM, TAG, "no memory");
    job->kind = KWS_OTA_MODEL;
    for (int i = 0; i < 32; i++) {
        int hi = hexval(sha256_hex[2 * i]), lo = hexval(sha256_hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            free(job);
            ESP_LOGE(TAG, "sha256 is not hex");
            return ESP_ERR_INVALID_ARG;
        }
        job->sha256[i] = (uint8_t)(hi << 4 | lo);
    }
    esp_err_t err = start(job, url, request_id);
    if (err != ESP_OK) {
        free(job);
    }
    return err;
}

bool kws_ota_busy(void)
{
    return atomic_load(&s_busy);
}
