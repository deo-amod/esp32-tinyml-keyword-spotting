#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "kws_device_config.h"

typedef struct {
    kws_device_config_t device;
    int64_t boot_us;
    bool mqtt_started;
} app_ctx_t;

/* app_commands.c: handle one JSON command from devices/<id>/cmd. */
void app_handle_command(app_ctx_t *app, const char *json, size_t len);
void app_publish_result(const char *id, const char *op, const char *status, const char *detail);

/* app_telemetry.c */
void app_publish_health(const app_ctx_t *app);

/* app_settings.c: persisted runtime settings */
float app_settings_threshold(void);
esp_err_t app_settings_set_threshold(float threshold);
