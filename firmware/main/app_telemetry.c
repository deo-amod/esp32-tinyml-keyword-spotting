#include <inttypes.h>
#include <stdio.h>

#include "app.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "kws_audio.h"
#include "kws_inference.h"
#include "kws_mqtt.h"
#include "kws_net.h"

static const char *TAG = "telemetry";

void app_publish_health(const app_ctx_t *app)
{
    kws_inference_stats_t inf;
    kws_audio_stats_t audio;
    kws_inference_get_stats(&inf);
    kws_audio_get_stats(&audio, true);

    char json[640];
    int n = snprintf(json, sizeof(json),
        "{\"fw\":\"%s\",\"uptime_s\":%" PRId64 ","
        "\"heap\":{\"internal_free\":%u,\"internal_min\":%u,\"psram_free\":%u},"
        "\"rssi\":%d,"
        "\"model\":{\"version\":%" PRIu32 ",\"source\":\"%s\",\"arena_bytes\":%" PRIu32 ",\"threshold\":%.2f},"
        "\"inference\":{\"count\":%" PRIu32 ",\"avg_us\":%" PRIu32 ",\"max_us\":%" PRIu32 ",\"detections\":%" PRIu32 "},"
        "\"audio\":{\"captured\":%" PRIu32 ",\"dropped\":%" PRIu32 ",\"errors\":%" PRIu32 ",\"peak\":%d}}",
        esp_app_get_description()->version, (esp_timer_get_time() - app->boot_us) / 1000000,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), kws_net_rssi(),
        inf.model_version, inf.model_source ? inf.model_source : "none", inf.arena_used, (double)inf.threshold,
        inf.inferences, inf.avg_us, inf.max_us, inf.detections,
        audio.samples_captured, audio.samples_dropped, audio.read_errors, audio.peak_abs);
    if (n <= 0 || n >= (int)sizeof(json)) {
        ESP_LOGE(TAG, "health JSON truncated");
        return;
    }
    ESP_LOGI(TAG, "%s", json);
    kws_mqtt_publish("health", json, 0, false);
}
