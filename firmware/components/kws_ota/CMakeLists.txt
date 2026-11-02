idf_component_register(
    SRCS "kws_ota.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES kws_model esp_https_ota esp_http_client app_update esp_app_format mbedtls log)
