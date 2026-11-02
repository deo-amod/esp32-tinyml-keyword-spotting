#include "kws_model_store.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "kws_model";

#define NVS_NAMESPACE   "kws_model"
#define NVS_KEY_ACTIVE  "active"
#define NVS_KEY_PENDING "pending"
#define NO_SLOT         0xFF

extern const uint8_t factory_model_start[] asm("_binary_kws_factory_kwsm_start");
extern const uint8_t factory_model_end[] asm("_binary_kws_factory_kwsm_end");
extern const char signing_pubkey_pem[] asm("_binary_model_signing_pub_pem_start");

struct kws_model_writer {
    const esp_partition_t *part;
    kws_model_source_t slot;
    size_t total;
    size_t written;
    mbedtls_sha256_context sha;
};

static kws_model_source_t s_running = KWS_MODEL_SRC_FACTORY;

static const esp_partition_t *slot_partition(kws_model_source_t src)
{
    const char *label = (src == KWS_MODEL_SRC_SLOT_A) ? "model_a" : "model_b";
    esp_partition_subtype_t subtype = (src == KWS_MODEL_SRC_SLOT_A) ? 0x40 : 0x41;
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, subtype, label);
}

static uint8_t nvs_get_slot(const char *key)
{
    nvs_handle_t h;
    uint8_t v = NO_SLOT;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, key, &v);
        nvs_close(h);
    }
    return v;
}

static void nvs_set_slot(const char *key, uint8_t v)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = (v == NO_SLOT) ? nvs_erase_key(h, key) : nvs_set_u8(h, key, v);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to persist %s=%u: %s", key, v, esp_err_to_name(err));
    }
}

static bool is_slot(uint8_t v)
{
    return v == KWS_MODEL_SRC_SLOT_A || v == KWS_MODEL_SRC_SLOT_B;
}

esp_err_t kws_model_store_init(void)
{
    ESP_RETURN_ON_FALSE(slot_partition(KWS_MODEL_SRC_SLOT_A) && slot_partition(KWS_MODEL_SRC_SLOT_B),
                        ESP_ERR_NOT_FOUND, TAG, "model_a/model_b partitions missing from partition table");
    size_t factory_len = factory_model_end - factory_model_start;
    kws_model_fmt_result_t r = kws_model_format_check(factory_model_start, factory_len);
    ESP_RETURN_ON_FALSE(r == KWS_MODEL_FMT_OK, ESP_ERR_INVALID_STATE, TAG,
                        "embedded factory model invalid: %s", kws_model_fmt_result_str(r));
    return ESP_OK;
}

const char *kws_model_source_name(kws_model_source_t src)
{
    switch (src) {
    case KWS_MODEL_SRC_FACTORY: return "factory";
    case KWS_MODEL_SRC_SLOT_A:  return "slot_a";
    case KWS_MODEL_SRC_SLOT_B:  return "slot_b";
    }
    return "?";
}

size_t kws_model_boot_order(kws_model_source_t out[3])
{
    size_t n = 0;
    uint8_t pending = nvs_get_slot(NVS_KEY_PENDING);
    uint8_t active = nvs_get_slot(NVS_KEY_ACTIVE);
    if (is_slot(pending)) {
        out[n++] = (kws_model_source_t)pending;
    }
    if (is_slot(active) && active != pending) {
        out[n++] = (kws_model_source_t)active;
    }
    out[n++] = KWS_MODEL_SRC_FACTORY;
    return n;
}

static esp_err_t verify_signature(const uint8_t *blob, const kws_model_header_t *h)
{
    uint8_t hash[32];
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    mbedtls_sha256_update(&sha, blob, KWS_MODEL_SIGNED_PREFIX);
    mbedtls_sha256_update(&sha, blob + KWS_MODEL_HEADER_SIZE, h->model_size);
    mbedtls_sha256_finish(&sha, hash);
    mbedtls_sha256_free(&sha);

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int rc = mbedtls_pk_parse_public_key(&pk, (const unsigned char *)signing_pubkey_pem,
                                         strlen(signing_pubkey_pem) + 1);
    if (rc == 0) {
        rc = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), h->signature, h->sig_len);
    }
    mbedtls_pk_free(&pk);
    if (rc != 0) {
        ESP_LOGE(TAG, "model signature rejected (mbedtls -0x%04x)", (unsigned)-rc);
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

esp_err_t kws_model_open(kws_model_source_t src, kws_model_t *m)
{
    memset(m, 0, sizeof(*m));
    m->source = src;
    const uint8_t *blob;
    size_t len;

    if (src == KWS_MODEL_SRC_FACTORY) {
        blob = factory_model_start;
        len = factory_model_end - factory_model_start;
    } else {
        const esp_partition_t *part = slot_partition(src);
        ESP_RETURN_ON_FALSE(part, ESP_ERR_NOT_FOUND, TAG, "no partition for %s", kws_model_source_name(src));
        const void *ptr;
        ESP_RETURN_ON_ERROR(esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &ptr,
                                               &m->mmap_handle), TAG, "mmap %s", part->label);
        m->mapped = true;
        blob = ptr;
        len = part->size;
    }

    esp_err_t err = ESP_OK;
    kws_model_fmt_result_t r = kws_model_format_check(blob, len);
    if (r != KWS_MODEL_FMT_OK) {
        ESP_LOGW(TAG, "%s: %s", kws_model_source_name(src), kws_model_fmt_result_str(r));
        err = (r == KWS_MODEL_FMT_BAD_MAGIC) ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_CRC;
        goto fail;
    }
    m->header = (const kws_model_header_t *)blob;

    /* The factory model is part of the (secure-boot verified) app image. */
#if CONFIG_KWS_MODEL_REQUIRE_SIGNATURE
    if (src != KWS_MODEL_SRC_FACTORY) {
        err = verify_signature(blob, m->header);
        if (err != ESP_OK) {
            goto fail;
        }
    }
#endif

    m->tflite_size = m->header->model_size;
    m->tflite = blob + KWS_MODEL_HEADER_SIZE;
    if (((uintptr_t)m->tflite & 15u) != 0) {
        m->aligned_copy = heap_caps_aligned_alloc(16, m->tflite_size, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
        if (!m->aligned_copy) {
            m->aligned_copy = heap_caps_aligned_alloc(16, m->tflite_size, MALLOC_CAP_8BIT);
        }
        if (!m->aligned_copy) {
            err = ESP_ERR_NO_MEM;
            goto fail;
        }
        memcpy(m->aligned_copy, m->tflite, m->tflite_size);
        m->tflite = m->aligned_copy;
    }
    ESP_LOGI(TAG, "%s: model v%" PRIu32 ", %u bytes, %u labels", kws_model_source_name(src),
             m->header->model_version, (unsigned)m->tflite_size, m->header->num_labels);
    return ESP_OK;

fail:
    kws_model_close(m);
    return err;
}

void kws_model_close(kws_model_t *m)
{
    free(m->aligned_copy);
    if (m->mapped) {
        esp_partition_munmap(m->mmap_handle);
    }
    memset(m, 0, sizeof(*m));
}

void kws_model_mark_running(kws_model_source_t src)
{
    s_running = src;
    if (is_slot(src) && nvs_get_slot(NVS_KEY_PENDING) == src) {
        ESP_LOGI(TAG, "model in %s confirmed", kws_model_source_name(src));
        nvs_set_slot(NVS_KEY_ACTIVE, src);
        nvs_set_slot(NVS_KEY_PENDING, NO_SLOT);
    }
}

void kws_model_mark_failed(kws_model_source_t src)
{
    if (!is_slot(src)) {
        return;
    }
    ESP_LOGW(TAG, "dropping model in %s", kws_model_source_name(src));
    if (nvs_get_slot(NVS_KEY_PENDING) == src) {
        nvs_set_slot(NVS_KEY_PENDING, NO_SLOT);
    }
    if (nvs_get_slot(NVS_KEY_ACTIVE) == src) {
        nvs_set_slot(NVS_KEY_ACTIVE, NO_SLOT);
    }
}

kws_model_source_t kws_model_running_source(void)
{
    return s_running;
}

esp_err_t kws_model_writer_begin(size_t total_size, kws_model_writer_t **out)
{
    *out = NULL;
    kws_model_source_t slot = (s_running == KWS_MODEL_SRC_SLOT_A) ? KWS_MODEL_SRC_SLOT_B : KWS_MODEL_SRC_SLOT_A;
    const esp_partition_t *part = slot_partition(slot);
    ESP_RETURN_ON_FALSE(part, ESP_ERR_NOT_FOUND, TAG, "no partition for %s", kws_model_source_name(slot));
    ESP_RETURN_ON_FALSE(total_size > KWS_MODEL_HEADER_SIZE && total_size <= part->size, ESP_ERR_INVALID_SIZE,
                        TAG, "model size %u does not fit %s (%u)", (unsigned)total_size, part->label,
                        (unsigned)part->size);

    kws_model_writer_t *w = calloc(1, sizeof(*w));
    ESP_RETURN_ON_FALSE(w, ESP_ERR_NO_MEM, TAG, "no memory");
    w->part = part;
    w->slot = slot;
    w->total = total_size;
    size_t erase = (total_size + part->erase_size - 1) / part->erase_size * part->erase_size;
    esp_err_t err = esp_partition_erase_range(part, 0, erase);
    if (err != ESP_OK) {
        free(w);
        ESP_LOGE(TAG, "erase %s: %s", part->label, esp_err_to_name(err));
        return err;
    }
    /* A half-written slot must never be selected: pending/active are only set in finish(). */
    if (nvs_get_slot(NVS_KEY_ACTIVE) == slot) {
        nvs_set_slot(NVS_KEY_ACTIVE, NO_SLOT);
    }
    mbedtls_sha256_init(&w->sha);
    mbedtls_sha256_starts(&w->sha, 0);
    ESP_LOGI(TAG, "writing %u byte model to %s", (unsigned)total_size, part->label);
    *out = w;
    return ESP_OK;
}

esp_err_t kws_model_writer_write(kws_model_writer_t *w, const void *data, size_t len)
{
    ESP_RETURN_ON_FALSE(w->written + len <= w->total, ESP_ERR_INVALID_SIZE, TAG, "model larger than announced");
    ESP_RETURN_ON_ERROR(esp_partition_write(w->part, w->written, data, len), TAG, "flash write");
    mbedtls_sha256_update(&w->sha, data, len);
    w->written += len;
    return ESP_OK;
}

esp_err_t kws_model_writer_finish(kws_model_writer_t *w, const uint8_t expected_sha256[32])
{
    esp_err_t err = ESP_OK;
    uint8_t digest[32];
    mbedtls_sha256_finish(&w->sha, digest);
    mbedtls_sha256_free(&w->sha);

    if (w->written != w->total) {
        ESP_LOGE(TAG, "incomplete model: %u of %u bytes", (unsigned)w->written, (unsigned)w->total);
        err = ESP_ERR_INVALID_SIZE;
    } else if (expected_sha256 && memcmp(digest, expected_sha256, sizeof(digest)) != 0) {
        ESP_LOGE(TAG, "model SHA-256 mismatch");
        err = ESP_ERR_INVALID_CRC;
    } else {
        kws_model_t m;
        err = kws_model_open(w->slot, &m);
        kws_model_close(&m);
    }

    if (err == ESP_OK) {
        nvs_set_slot(NVS_KEY_PENDING, w->slot);
        ESP_LOGI(TAG, "model staged in %s; takes effect after reboot", kws_model_source_name(w->slot));
    } else {
        esp_partition_erase_range(w->part, 0, w->part->erase_size);
    }
    free(w);
    return err;
}

void kws_model_writer_abort(kws_model_writer_t *w)
{
    if (!w) {
        return;
    }
    mbedtls_sha256_free(&w->sha);
    esp_partition_erase_range(w->part, 0, w->part->erase_size);
    free(w);
}
