# Security model

## Summary

| Asset | Protection | Where |
|---|---|---|
| Firmware image | Secure Boot v2 (ECDSA P-256), signed OTA, A/B slots with rollback | `sdkconfig.release`, `kws_ota` |
| Model | ECDSA P-256 signature + CRC-32 + SHA-256 on download; A/B slots, factory fallback | `kws_model` |
| Flash contents | Flash encryption (release mode), NVS encryption | `sdkconfig.release` |
| Device identity | Per-device X.509 certificate from the project CA (mutual TLS) | `tools/provision_device.py`, `fctry` partition |
| Wi-Fi onboarding | BLE provisioning, protocomm Security 2 (SRP6a + AES-GCM), per-device password | `kws_net` |
| Broker access | TLS 1.2, `require_certificate`, per-device topic ACLs | `tools/mosquitto/` |
| Audio | Never leaves the device; only keyword events are published | design |

## Keys (`secrets/`, git-ignored)

Created by `python tools/gen_keys.py`:

| File | Purpose | If lost | If leaked |
|---|---|---|---|
| `model_signing_key.pem` | Signs `.kwsm` models | No new models can be deployed to fielded devices | Attacker can push models (still needs broker access) |
| `secure_boot_signing_key.pem` | Signs firmware (release builds) | **Release devices can never be updated** | Attacker can sign firmware |
| `ca_key.pem` / `ca_cert.pem` | Issues device and broker certificates | New devices cannot be provisioned | Attacker can impersonate devices or broker |

Back these up offline. For production, generate and keep them in an HSM or a cloud KMS
and adapt `ml/export_model.py --key`, `idf.py` signing and `tools/provision_device.py`
to call it. The development keys created here are suitable for pilots only.

## Development vs release builds

The default build (`sdkconfig.defaults`) is for development:

- Secure Boot and flash encryption are **off**, so devices can be reflashed over USB.
- Devices without factory data use the development BLE password from menuconfig
  (`KWS_PROV_DEV_PASSWORD`, default `kws-dev-pass`). Anyone nearby who knows it can
  provision an unprovisioned development board.

The release overlay (`sdkconfig.release`) enables Secure Boot v2, flash encryption in
**release mode** and NVS encryption, and clears the development password.

> **Warning: release mode burns eFuses on first boot. This cannot be undone.** After that,
> the device only runs firmware signed with `secure_boot_signing_key.pem`, and USB
> reflashing of plaintext images stops working. Try the full flow on a sacrificial board
> first, and keep the signing key safe.

Release-mode bring-up has **not been tested on hardware yet**. In particular, check
that the `fctry` partition flashed by `tools/provision_device.py` can be read once NVS
encryption is enabled. That partition is written in plaintext today; encrypted
manufacturing images need `nvs_partition_gen.py encrypt` together with the device's
`nvs_keys`.

## Threats considered

- **Malicious OTA command.** Updates need a broker connection with a valid device or
  back-end certificate (ACLs restrict `cmd` topics). They must use `https://`; firmware
  must be signed (release) and models must be signed. Firmware that cannot reach MQTT
  within `KWS_SELFTEST_CONNECT_TIMEOUT_S` is rolled back.
- **Corrupted or partial model download.** The model is streamed into the inactive slot.
  The SHA-256 from the command, the CRC-32 and the signature are all checked before the
  slot is marked pending. If loading fails at boot, the device falls back to the previous
  model, then the factory model.
- **Rogue provisioning phone.** Security 2 needs the per-device password. Without it the
  SRP6a handshake fails and no credentials are accepted.
- **Physical flash readout.** This is mitigated only in release builds (flash encryption).

## Privacy

Raw audio is processed in RAM and discarded; nothing is recorded or uploaded. Published
data is limited to keyword events (label, score, time) and device health. If you add
audio capture for data collection, you need explicit user consent. Review it with your
Data Privacy Officer first (GDPR: voice can be personal and biometric data).
