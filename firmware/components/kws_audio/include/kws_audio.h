/*
 * I2S MEMS microphone capture (INMP441 or similar, 24-bit in 32-bit slots).
 * A capture task on core 1 converts samples to 16 kHz int16 PCM and pushes them
 * into a PSRAM stream buffer; one consumer reads them with kws_audio_read().
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KWS_AUDIO_SAMPLE_RATE 16000

typedef struct {
    uint32_t samples_captured;
    uint32_t samples_dropped;   /* consumer too slow: stream buffer full */
    uint32_t read_errors;
    int16_t peak_abs;           /* since last kws_audio_get_stats(..., true) */
} kws_audio_stats_t;

esp_err_t kws_audio_start(void);

/* Read exactly `samples` samples; returns the number read (< samples on timeout). */
size_t kws_audio_read(int16_t *dst, size_t samples, TickType_t timeout);

void kws_audio_get_stats(kws_audio_stats_t *out, bool reset_peak);

#ifdef __cplusplus
}
#endif
