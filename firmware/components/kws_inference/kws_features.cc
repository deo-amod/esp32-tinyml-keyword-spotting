#include "kws_features.h"

#include <algorithm>
#include <cstring>
#include <new>

#include "esp_check.h"
#include "esp_log.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

extern const uint8_t preprocessor_model_start[] asm("_binary_audio_preprocessor_int8_tflite_start");
extern const uint8_t preprocessor_model_end[] asm("_binary_audio_preprocessor_int8_tflite_end");

namespace {

const char *TAG = "kws_features";

constexpr size_t kArenaSize = 16 * 1024;
alignas(16) uint8_t g_arena[kArenaSize];
alignas(16) uint8_t g_model_copy[9 * 1024];

using PreprocessorOpResolver = tflite::MicroMutableOpResolver<18>;
PreprocessorOpResolver g_resolver;
tflite::MicroInterpreter *g_interpreter = nullptr;
alignas(tflite::MicroInterpreter) uint8_t g_interpreter_storage[sizeof(tflite::MicroInterpreter)];

int16_t g_window[KWS_FEATURE_WINDOW_SAMPLES];

TfLiteStatus register_ops(PreprocessorOpResolver &r)
{
    TF_LITE_ENSURE_STATUS(r.AddReshape());
    TF_LITE_ENSURE_STATUS(r.AddCast());
    TF_LITE_ENSURE_STATUS(r.AddStridedSlice());
    TF_LITE_ENSURE_STATUS(r.AddConcatenation());
    TF_LITE_ENSURE_STATUS(r.AddMul());
    TF_LITE_ENSURE_STATUS(r.AddAdd());
    TF_LITE_ENSURE_STATUS(r.AddDiv());
    TF_LITE_ENSURE_STATUS(r.AddMinimum());
    TF_LITE_ENSURE_STATUS(r.AddMaximum());
    TF_LITE_ENSURE_STATUS(r.AddWindow());
    TF_LITE_ENSURE_STATUS(r.AddFftAutoScale());
    TF_LITE_ENSURE_STATUS(r.AddRfft());
    TF_LITE_ENSURE_STATUS(r.AddEnergy());
    TF_LITE_ENSURE_STATUS(r.AddFilterBank());
    TF_LITE_ENSURE_STATUS(r.AddFilterBankSquareRoot());
    TF_LITE_ENSURE_STATUS(r.AddFilterBankSpectralSubtraction());
    TF_LITE_ENSURE_STATUS(r.AddPCAN());
    TF_LITE_ENSURE_STATUS(r.AddFilterBankLog());
    return kTfLiteOk;
}

}  // namespace

extern "C" esp_err_t kws_features_init(void)
{
    ESP_RETURN_ON_FALSE(!g_interpreter, ESP_ERR_INVALID_STATE, TAG, "already initialised");
    size_t len = preprocessor_model_end - preprocessor_model_start;
    ESP_RETURN_ON_FALSE(len <= sizeof(g_model_copy), ESP_ERR_INVALID_SIZE, TAG, "preprocessor too large");
    // Embedded files are only 4-byte aligned; flatbuffers want 16.
    memcpy(g_model_copy, preprocessor_model_start, len);

    const tflite::Model *model = tflite::GetModel(g_model_copy);
    ESP_RETURN_ON_FALSE(model->version() == TFLITE_SCHEMA_VERSION, ESP_ERR_INVALID_VERSION, TAG,
                        "preprocessor schema %lu", (unsigned long)model->version());
    ESP_RETURN_ON_FALSE(register_ops(g_resolver) == kTfLiteOk, ESP_FAIL, TAG, "register ops");

    g_interpreter = new (g_interpreter_storage) tflite::MicroInterpreter(model, g_resolver, g_arena, kArenaSize);
    ESP_RETURN_ON_FALSE(g_interpreter->AllocateTensors() == kTfLiteOk, ESP_FAIL, TAG, "AllocateTensors");

    TfLiteTensor *in = g_interpreter->input(0);
    TfLiteTensor *out = g_interpreter->output(0);
    ESP_RETURN_ON_FALSE(in->type == kTfLiteInt16 && in->bytes == sizeof(g_window) &&
                        out->type == kTfLiteInt8 && out->bytes == KWS_FEATURE_CHANNELS,
                        ESP_ERR_INVALID_SIZE, TAG, "unexpected preprocessor tensors");
    memset(g_window, 0, sizeof(g_window));
    ESP_LOGI(TAG, "audio preprocessor ready (arena %u bytes)", (unsigned)g_interpreter->arena_used_bytes());
    return ESP_OK;
}

extern "C" esp_err_t kws_features_process(const int16_t stride[KWS_FEATURE_STRIDE_SAMPLES],
                                          int8_t out[KWS_FEATURE_CHANNELS])
{
    constexpr int kKeep = KWS_FEATURE_WINDOW_SAMPLES - KWS_FEATURE_STRIDE_SAMPLES;
    memmove(g_window, g_window + KWS_FEATURE_STRIDE_SAMPLES, kKeep * sizeof(int16_t));
    memcpy(g_window + kKeep, stride, KWS_FEATURE_STRIDE_SAMPLES * sizeof(int16_t));

    std::copy_n(g_window, KWS_FEATURE_WINDOW_SAMPLES, tflite::GetTensorData<int16_t>(g_interpreter->input(0)));
    if (g_interpreter->Invoke() != kTfLiteOk) {
        return ESP_FAIL;
    }
    std::copy_n(tflite::GetTensorData<int8_t>(g_interpreter->output(0)), KWS_FEATURE_CHANNELS, out);
    return ESP_OK;
}
