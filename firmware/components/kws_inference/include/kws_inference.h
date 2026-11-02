/*
 * Keyword spotting engine: audio -> features -> int8 DS-CNN -> smoothing -> callback.
 * Runs as one task pinned to core 1, next to audio capture; the radio stacks own core 0.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int label;            /* index into the model's labels */
    const char *keyword;  /* label name, valid for the lifetime of the program */
    float score;          /* smoothed probability 0..1 */
    int64_t uptime_us;
} kws_detection_t;

typedef void (*kws_detection_cb_t)(const kws_detection_t *det, void *ctx);

typedef struct {
    uint32_t model_version;
    const char *model_source;
    uint32_t inferences;
    uint32_t detections;
    uint32_t last_us;
    uint32_t avg_us;      /* exponential moving average */
    uint32_t max_us;
    uint32_t arena_used;
    float threshold;
} kws_inference_stats_t;

/* Load the best available model (pending -> active -> factory) and the feature graph. */
esp_err_t kws_inference_init(void);

esp_err_t kws_inference_start(kws_detection_cb_t cb, void *ctx);

void kws_inference_set_threshold(float threshold);

void kws_inference_get_stats(kws_inference_stats_t *out);

#ifdef __cplusplus
}
#endif
