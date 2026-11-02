idf_component_register(
    SRCS "kws_device_config.c" "kws_wifi.c" "kws_mqtt.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_timer bt wifi_provisioning protocomm
                  mqtt mbedtls esp_app_format log)
