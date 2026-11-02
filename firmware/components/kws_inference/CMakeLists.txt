idf_component_register(
    SRCS "kws_detector.c" "kws_features.cc" "kws_inference.cc"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES kws_audio kws_model esp-tflite-micro esp_timer esp_system heap log
    EMBED_FILES "models/audio_preprocessor_int8.tflite")
