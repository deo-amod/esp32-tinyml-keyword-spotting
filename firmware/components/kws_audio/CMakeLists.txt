idf_component_register(
    SRCS "kws_audio.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos
    PRIV_REQUIRES esp_driver_i2s esp_system heap log)
