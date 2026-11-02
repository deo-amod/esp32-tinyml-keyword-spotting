idf_component_register(
    SRCS "main.c" "app_commands.c" "app_settings.c" "app_telemetry.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES kws_audio kws_board kws_inference kws_model kws_net kws_ota
                  nvs_flash esp_event esp_timer app_update esp_app_format json heap log)
