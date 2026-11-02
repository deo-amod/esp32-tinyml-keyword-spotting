#include "kws_detector.h"

#include <string.h>

void kws_detector_init(kws_detector_t *d, const kws_detector_config_t *cfg, uint8_t n_labels)
{
    memset(d, 0, sizeof(*d));
    d->cfg = *cfg;
    d->n_labels = n_labels > KWS_DETECTOR_MAX_LABELS ? KWS_DETECTOR_MAX_LABELS : n_labels;
}

void kws_detector_reset(kws_detector_t *d)
{
    d->head = 0;
    d->count = 0;
    d->has_last = false;
}

bool kws_detector_update(kws_detector_t *d, const float *probs, uint32_t now_ms, int *label, float *score)
{
    /* Append; the oldest entry is overwritten if the ring is full. */
    uint32_t slot = (d->head + d->count) % KWS_DETECTOR_MAX_HISTORY;
    if (d->count == KWS_DETECTOR_MAX_HISTORY) {
        d->head = (d->head + 1) % KWS_DETECTOR_MAX_HISTORY;
    } else {
        d->count++;
    }
    d->hist[slot].t_ms = now_ms;
    memcpy(d->hist[slot].p, probs, d->n_labels * sizeof(float));

    while (d->count > 0 && (uint32_t)(now_ms - d->hist[d->head].t_ms) > d->cfg.window_ms) {
        d->head = (d->head + 1) % KWS_DETECTOR_MAX_HISTORY;
        d->count--;
    }
    if (d->count < d->cfg.min_results) {
        return false;
    }

    float avg[KWS_DETECTOR_MAX_LABELS] = {0};
    for (uint32_t i = 0; i < d->count; i++) {
        const float *p = d->hist[(d->head + i) % KWS_DETECTOR_MAX_HISTORY].p;
        for (int c = 0; c < d->n_labels; c++) {
            avg[c] += p[c];
        }
    }
    int best = 0;
    for (int c = 0; c < d->n_labels; c++) {
        avg[c] /= (float)d->count;
        if (avg[c] > avg[best]) {
            best = c;
        }
    }
    if (best < d->cfg.first_keyword || avg[best] < d->cfg.threshold) {
        return false;
    }
    if (d->has_last && (uint32_t)(now_ms - d->last_ms) < d->cfg.suppression_ms) {
        return false;
    }
    d->has_last = true;
    d->last_ms = now_ms;
    *label = best;
    *score = avg[best];
    return true;
}
