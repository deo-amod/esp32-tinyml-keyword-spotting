/*
 * MQTT over TLS with mutual authentication. Topics are under devices/<device_id>/:
 *   status (retained, last will)  events  health  cmd (subscribed)  cmd/result
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "kws_device_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called on the MQTT task: copy what you need and return quickly. */
typedef void (*kws_mqtt_cmd_cb_t)(const char *payload, size_t len, void *ctx);
typedef void (*kws_mqtt_conn_cb_t)(bool connected, void *ctx);

esp_err_t kws_mqtt_start(const kws_device_config_t *cfg, kws_mqtt_cmd_cb_t cmd_cb, kws_mqtt_conn_cb_t conn_cb,
                         void *ctx);

bool kws_mqtt_connected(void);

/* Non-blocking publish to devices/<id>/<subtopic>. Fails if not connected. */
esp_err_t kws_mqtt_publish(const char *subtopic, const char *payload, int qos, bool retain);

#ifdef __cplusplus
}
#endif
