/*
 * Host tests for the hardware-independent firmware modules.
 *
 *   test_host unit                 built-in unit tests
 *   test_host check-model <file>   validate a .kwsm file, print its header
 *   test_host replay               stdin: "t_ms p0 p1 ..." lines; stdout: "t_ms label score"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kws_detector.h"
#include "kws_model_format.h"

static int g_failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                                 \
        }                                                                 \
    } while (0)

static kws_detector_config_t default_cfg(void)
{
    kws_detector_config_t c = {.threshold = 0.8f, .window_ms = 600, .min_results = 3, .suppression_ms = 1200,
                               .first_keyword = 2};
    return c;
}

static void test_crc32(void)
{
    /* Standard check value for CRC-32/ISO-HDLC (zlib). */
    CHECK(kws_crc32(0, "123456789", 9) == 0xCBF43926u);
    CHECK(kws_crc32(0, "", 0) == 0);
}

static void test_detector_fires_once(void)
{
    kws_detector_t d;
    kws_detector_config_t c = default_cfg();
    kws_detector_init(&d, &c, 4);
    float on[4] = {0.0f, 0.05f, 0.95f, 0.0f};
    int label = -1, hits = 0;
    float score;
    for (uint32_t t = 0; t < 1000; t += 60) {
        hits += kws_detector_update(&d, on, t, &label, &score);
    }
    CHECK(hits == 1);
    CHECK(label == 2);
    CHECK(score > 0.9f);
}

static void test_detector_ignores_unknown_and_low(void)
{
    kws_detector_t d;
    kws_detector_config_t c = default_cfg();
    kws_detector_init(&d, &c, 4);
    float unk[4] = {0.0f, 0.99f, 0.01f, 0.0f};
    float low[4] = {0.2f, 0.1f, 0.7f, 0.0f};
    int label;
    float score;
    int hits = 0;
    for (uint32_t t = 0; t < 2000; t += 60) {
        hits += kws_detector_update(&d, t < 1000 ? unk : low, t, &label, &score);
    }
    CHECK(hits == 0);
}

static void test_detector_suppression_and_wrap(void)
{
    kws_detector_t d;
    kws_detector_config_t c = default_cfg();
    kws_detector_init(&d, &c, 4);
    float off[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    int label;
    float score;
    int hits = 0;
    /* Start just below the uint32 wrap: time differences must still work. */
    uint32_t t0 = 0xFFFFFF00u;
    for (uint32_t i = 0; i < 100; i++) {
        hits += kws_detector_update(&d, off, t0 + i * 60, &label, &score);
    }
    /* 100 * 60 ms = 6 s of continuous keyword, 1.2 s suppression -> 5 detections. */
    CHECK(hits == 5);
}

static void test_format_rejects_corruption(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        g_failures++;
        return;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *blob = malloc(n);
    CHECK(fread(blob, 1, n, f) == (size_t)n);
    fclose(f);

    CHECK(kws_model_format_check(blob, n) == KWS_MODEL_FMT_OK);
    CHECK(kws_model_format_check(blob, 100) == KWS_MODEL_FMT_TOO_SMALL);
    CHECK(kws_model_format_check(blob, n - 1) == KWS_MODEL_FMT_BAD_SIZE);
    blob[n / 2] ^= 0x01;
    CHECK(kws_model_format_check(blob, n) == KWS_MODEL_FMT_BAD_MODEL_CRC);
    blob[n / 2] ^= 0x01;
    blob[8] ^= 0x01; /* model_version */
    CHECK(kws_model_format_check(blob, n) == KWS_MODEL_FMT_BAD_HEADER_CRC);
    blob[8] ^= 0x01;
    memset(blob, 0xFF, 4); /* erased flash */
    CHECK(kws_model_format_check(blob, n) == KWS_MODEL_FMT_BAD_MAGIC);
    free(blob);
}

static int check_model(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return 2;
    }
    static uint8_t blob[512 * 1024];
    size_t n = fread(blob, 1, sizeof(blob), f);
    fclose(f);
    kws_model_fmt_result_t r = kws_model_format_check(blob, n);
    if (r != KWS_MODEL_FMT_OK) {
        printf("invalid: %s\n", kws_model_fmt_result_str(r));
        return 1;
    }
    kws_model_header_t h;
    memcpy(&h, blob, sizeof(h));
    printf("version=%u size=%u frames=%u channels=%u window=%u stride=%u rate=%u sig=%u labels=",
           (unsigned)h.model_version, (unsigned)h.model_size, h.feature_frames, h.feature_channels, h.window_ms,
           h.stride_ms, (unsigned)h.sample_rate, h.sig_len);
    for (int i = 0; i < h.num_labels; i++) {
        printf("%s%s", i ? "," : "", h.labels[i]);
    }
    printf("\n");
    return 0;
}

static int replay(void)
{
    kws_detector_t d;
    kws_detector_config_t c = default_cfg();
    kws_detector_init(&d, &c, 4);
    char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        unsigned t;
        float p[4];
        if (sscanf(line, "%u %f %f %f %f", &t, &p[0], &p[1], &p[2], &p[3]) != 5) {
            continue;
        }
        int label;
        float score;
        if (kws_detector_update(&d, p, t, &label, &score)) {
            printf("%u %d %.4f\n", t, label, score);
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "check-model") == 0) {
        return check_model(argv[2]);
    }
    if (argc >= 2 && strcmp(argv[1], "replay") == 0) {
        return replay();
    }
    test_crc32();
    test_detector_fires_once();
    test_detector_ignores_unknown_and_low();
    test_detector_suppression_and_wrap();
    if (argc >= 3 && strcmp(argv[1], "unit") == 0) {
        test_format_rejects_corruption(argv[2]);
    }
    printf("%s (%d failures)\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
