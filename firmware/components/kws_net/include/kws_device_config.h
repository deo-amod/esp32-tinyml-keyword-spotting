/*
 * Per-device identity and credentials, read from the `fctry` NVS partition that
 * tools/provision_device.py writes at manufacturing time. Development boards
 * without factory data fall back to menuconfig values (see Kconfig).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char device_id[33];
    char *broker_uri;    /* mqtts://host:8883 */
    char *ca_cert;       /* PEM: CA that signed the broker (and OTA server) certificate */
    char *client_cert;   /* PEM: device certificate */
    char *client_key;    /* PEM: device private key */
    char prov_user[33];  /* BLE provisioning (Security 2 / SRP6a) */
    uint8_t *prov_salt;
    size_t prov_salt_len;
    uint8_t *prov_verifier;
    size_t prov_verifier_len;
    bool from_factory;   /* false: development fallback values */
} kws_device_config_t;

esp_err_t kws_device_config_load(kws_device_config_t *cfg);

/*
 * Make sure SRP6a salt/verifier are available for BLE provisioning, deriving them
 * from the development password if the device has no factory credentials.
 */
esp_err_t kws_device_config_ensure_prov(kws_device_config_t *cfg);

#ifdef __cplusplus
}
#endif
