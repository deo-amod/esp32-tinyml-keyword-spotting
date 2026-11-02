/*
 * Log-mel style features from the TFLM audio preprocessor graph
 * (models/audio_preprocessor_int8.tflite): 30 ms windows, 20 ms stride,
 * 40 int8 channels. Training uses the same graph (ml/features.py).
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KWS_FEATURE_SAMPLE_RATE    16000
#define KWS_FEATURE_WINDOW_MS      30
#define KWS_FEATURE_STRIDE_MS      20
#define KWS_FEATURE_WINDOW_SAMPLES (KWS_FEATURE_SAMPLE_RATE * KWS_FEATURE_WINDOW_MS / 1000)
#define KWS_FEATURE_STRIDE_SAMPLES (KWS_FEATURE_SAMPLE_RATE * KWS_FEATURE_STRIDE_MS / 1000)
#define KWS_FEATURE_CHANNELS       40
#define KWS_FEATURE_FRAMES         49

esp_err_t kws_features_init(void);

/* Consume one stride of new samples, emit one frame of features. */
esp_err_t kws_features_process(const int16_t stride[KWS_FEATURE_STRIDE_SAMPLES],
                               int8_t out[KWS_FEATURE_CHANNELS]);

#ifdef __cplusplus
}
#endif
