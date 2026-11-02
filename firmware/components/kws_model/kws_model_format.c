#include "kws_model_format.h"

#include <string.h>

uint32_t kws_crc32(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = data;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

kws_model_fmt_result_t kws_model_format_check(const uint8_t *blob, size_t len)
{
    kws_model_header_t h;
    if (len < sizeof(h)) {
        return KWS_MODEL_FMT_TOO_SMALL;
    }
    memcpy(&h, blob, sizeof(h)); /* blob may be unaligned */
    if (h.magic != KWS_MODEL_MAGIC) {
        return KWS_MODEL_FMT_BAD_MAGIC;
    }
    if (h.header_version != KWS_MODEL_HEADER_VERSION || h.header_size != KWS_MODEL_HEADER_SIZE) {
        return KWS_MODEL_FMT_BAD_VERSION;
    }
    if (kws_crc32(0, blob, offsetof(kws_model_header_t, header_crc32)) != h.header_crc32) {
        return KWS_MODEL_FMT_BAD_HEADER_CRC;
    }
    if (h.model_size == 0 || h.model_size > len - sizeof(h) || h.sig_len > KWS_MODEL_SIG_MAX) {
        return KWS_MODEL_FMT_BAD_SIZE;
    }
    if (h.num_labels == 0 || h.num_labels > KWS_MODEL_MAX_LABELS) {
        return KWS_MODEL_FMT_BAD_LABELS;
    }
    for (int i = 0; i < h.num_labels; i++) {
        if (memchr(h.labels[i], '\0', KWS_MODEL_LABEL_LEN) == NULL) {
            return KWS_MODEL_FMT_BAD_LABELS;
        }
    }
    if (kws_crc32(0, blob + sizeof(h), h.model_size) != h.model_crc32) {
        return KWS_MODEL_FMT_BAD_MODEL_CRC;
    }
    return KWS_MODEL_FMT_OK;
}

const char *kws_model_fmt_result_str(kws_model_fmt_result_t r)
{
    switch (r) {
    case KWS_MODEL_FMT_OK:             return "ok";
    case KWS_MODEL_FMT_TOO_SMALL:      return "too small";
    case KWS_MODEL_FMT_BAD_MAGIC:      return "no model (bad magic)";
    case KWS_MODEL_FMT_BAD_VERSION:    return "unsupported header version";
    case KWS_MODEL_FMT_BAD_HEADER_CRC: return "header CRC mismatch";
    case KWS_MODEL_FMT_BAD_SIZE:       return "invalid size";
    case KWS_MODEL_FMT_BAD_MODEL_CRC:  return "model CRC mismatch";
    case KWS_MODEL_FMT_BAD_LABELS:     return "invalid labels";
    }
    return "unknown";
}
