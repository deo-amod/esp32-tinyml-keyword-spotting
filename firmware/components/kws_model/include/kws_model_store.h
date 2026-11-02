/*
 * Model storage: an embedded factory model plus two OTA-updatable flash slots.
 *
 * Boot order is pending slot -> active slot -> factory. A newly installed model
 * is "pending" until the inference engine has loaded and run it once
 * (kws_model_mark_running); a model that fails is dropped (kws_model_mark_failed).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_partition.h"
#include "kws_model_format.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KWS_MODEL_SRC_FACTORY = 0,
    KWS_MODEL_SRC_SLOT_A = 1,
    KWS_MODEL_SRC_SLOT_B = 2,
} kws_model_source_t;

typedef struct {
    kws_model_source_t source;
    const kws_model_header_t *header;
    const uint8_t *tflite;    /* 16-byte aligned */
    size_t tflite_size;
    /* private */
    esp_partition_mmap_handle_t mmap_handle;
    bool mapped;
    void *aligned_copy;
} kws_model_t;

typedef struct kws_model_writer kws_model_writer_t;

esp_err_t kws_model_store_init(void);

const char *kws_model_source_name(kws_model_source_t src);

/* Fill `out` with sources to try, in order. Returns the count (1..3). */
size_t kws_model_boot_order(kws_model_source_t out[3]);

/* Map and fully validate (structure, CRCs, and signature for flash slots). */
esp_err_t kws_model_open(kws_model_source_t src, kws_model_t *out);
void kws_model_close(kws_model_t *m);

void kws_model_mark_running(kws_model_source_t src);
void kws_model_mark_failed(kws_model_source_t src);
kws_model_source_t kws_model_running_source(void);

/* Model OTA: stream a .kwsm blob into the slot that is not running. */
esp_err_t kws_model_writer_begin(size_t total_size, kws_model_writer_t **out);
esp_err_t kws_model_writer_write(kws_model_writer_t *w, const void *data, size_t len);
/* Verifies SHA-256 (if expected_sha256 != NULL), structure and signature, then marks pending. */
esp_err_t kws_model_writer_finish(kws_model_writer_t *w, const uint8_t expected_sha256[32]);
void kws_model_writer_abort(kws_model_writer_t *w);

#ifdef __cplusplus
}
#endif
