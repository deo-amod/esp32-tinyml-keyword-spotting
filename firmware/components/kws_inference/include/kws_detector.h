/*
 * Posterior smoothing: average class probabilities over a sliding time window,
 * then apply a threshold and a suppression period so one utterance yields one
 * detection. Pure C (host-testable). Mirrored in ml/detector.py.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KWS_DETECTOR_MAX_LABELS  8
#define KWS_DETECTOR_MAX_HISTORY 32

typedef struct {
    float threshold;          /* 0..1, on the averaged probability */
    uint32_t window_ms;
    uint32_t min_results;     /* results needed in the window before deciding */
    uint32_t suppression_ms;  /* minimum gap between detections */
    uint8_t first_keyword;    /* labels below this index (silence/unknown) never fire */
} kws_detector_config_t;

typedef struct {
    kws_detector_config_t cfg;
    uint8_t n_labels;
    uint32_t head;
    uint32_t count;
    bool has_last;
    uint32_t last_ms;
    struct {
        uint32_t t_ms;
        float p[KWS_DETECTOR_MAX_LABELS];
    } hist[KWS_DETECTOR_MAX_HISTORY];
} kws_detector_t;

void kws_detector_init(kws_detector_t *d, const kws_detector_config_t *cfg, uint8_t n_labels);
void kws_detector_reset(kws_detector_t *d);

/* Returns true and sets *label / *score when a keyword is detected. */
bool kws_detector_update(kws_detector_t *d, const float *probs, uint32_t now_ms, int *label, float *score);

#ifdef __cplusplus
}
#endif
