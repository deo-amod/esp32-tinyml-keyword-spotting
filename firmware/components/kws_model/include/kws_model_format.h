/*
 * .kwsm model container: a 256-byte header followed by a TFLite flatbuffer.
 * Must match ml/kwsm_format.py. Pure C, no ESP-IDF dependencies (host-testable).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KWS_MODEL_MAGIC          0x4D53574Bu /* "KWSM" */
#define KWS_MODEL_HEADER_VERSION 1
#define KWS_MODEL_HEADER_SIZE    256
#define KWS_MODEL_MAX_LABELS     8
#define KWS_MODEL_LABEL_LEN      16
#define KWS_MODEL_SIG_MAX        80
/* Bytes of the header covered by the signature (everything before `signature`). */
#define KWS_MODEL_SIGNED_PREFIX  164

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t header_version;
    uint16_t header_size;
    uint32_t model_version;
    uint32_t model_size;      /* TFLite bytes following the header */
    uint32_t model_crc32;     /* CRC-32 (zlib) of the TFLite bytes */
    uint16_t feature_frames;
    uint16_t feature_channels;
    uint16_t window_ms;
    uint16_t stride_ms;
    uint32_t sample_rate;
    uint8_t  num_labels;
    uint8_t  sig_len;
    uint8_t  reserved0[2];
    char     labels[KWS_MODEL_MAX_LABELS][KWS_MODEL_LABEL_LEN];
    uint8_t  signature[KWS_MODEL_SIG_MAX]; /* DER ECDSA-P256 over SHA-256(prefix || model) */
    uint8_t  reserved1[8];
    uint32_t header_crc32;    /* CRC-32 of bytes [0, 252) */
} kws_model_header_t;

#ifdef __cplusplus
#define KWS_STATIC_ASSERT static_assert
#else
#define KWS_STATIC_ASSERT _Static_assert
#endif
KWS_STATIC_ASSERT(sizeof(kws_model_header_t) == KWS_MODEL_HEADER_SIZE, "header layout");
KWS_STATIC_ASSERT(offsetof(kws_model_header_t, signature) == KWS_MODEL_SIGNED_PREFIX, "signature offset");

typedef enum {
    KWS_MODEL_FMT_OK = 0,
    KWS_MODEL_FMT_TOO_SMALL,
    KWS_MODEL_FMT_BAD_MAGIC,
    KWS_MODEL_FMT_BAD_VERSION,
    KWS_MODEL_FMT_BAD_HEADER_CRC,
    KWS_MODEL_FMT_BAD_SIZE,
    KWS_MODEL_FMT_BAD_MODEL_CRC,
    KWS_MODEL_FMT_BAD_LABELS,
} kws_model_fmt_result_t;

/* CRC-32 compatible with zlib.crc32(); pass 0 as the initial value. */
uint32_t kws_crc32(uint32_t crc, const void *data, size_t len);

/*
 * Check structure and CRCs of a blob of at most `len` bytes (a partition may be
 * larger than the model it holds). Does not check the signature.
 */
kws_model_fmt_result_t kws_model_format_check(const uint8_t *blob, size_t len);

const char *kws_model_fmt_result_str(kws_model_fmt_result_t r);

#ifdef __cplusplus
}
#endif
