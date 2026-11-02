# esp32-tinyml-keyword-spotting

Always-on keyword spotting ("**on**" / "**off**") on an **ESP32-WROVER-E (N16R8)**: 16 MB
flash, 8 MB PSRAM, Wi-Fi and BLE. It runs TensorFlow Lite Micro with ESP-NN, uses BLE for
Wi-Fi provisioning, and talks MQTT over mutual TLS to a self-hosted Mosquitto broker.
Firmware and models update over the air, with A/B slots and rollback.

| | |
|---|---|
| Model | int8 DS-CNN, 4 classes (`_silence_`, `_unknown_`, `on`, `off`), 33.8 KB, about 13 k parameters, about 2.4 M MACs |
| Features | 49 × 40 log filterbank, 30 ms window / 20 ms stride, TFLM audio preprocessor (identical in training and on device) |
| Accuracy | 92.9 % int8 on the Speech Commands v2 test split, with background noise ([metrics](ml/reports/metrics.json)) |
| Streaming | 63 % recall at 4–6 false accepts / hour, default settings, two synthetic 30-minute streams ([sweep](ml/reports/detector_sweep.json), [metrics](ml/reports/metrics.json)) |
| Device | Not yet measured on hardware: see [Status](#status) |

Security model: [docs/SECURITY.md](docs/SECURITY.md).

## Repository layout

```
firmware/                ESP-IDF v5.5 project
  main/                  app_main, command handling, telemetry, Kconfig
  components/
    kws_audio/           I2S mic capture -> PSRAM stream buffer (core 1)
    kws_inference/       feature graph + KWS model + posterior smoothing (core 1)
    kws_model/           .kwsm format, A/B model slots, signature check, model OTA writer
    kws_net/             BLE provisioning (Security 2), Wi-Fi, MQTT/TLS, factory config
    kws_ota/             HTTPS firmware OTA and model OTA
    kws_board/           status LED, BOOT button
  partitions.csv         16 MB layout (A/B app, A/B model, fctry, coredump)
  sdkconfig.defaults     development configuration
  sdkconfig.release      Secure Boot v2 + flash encryption overlay (burns eFuses!)
ml/                      dataset prep, training, int8 export, detector tuning
tools/                   key generation, device provisioning, broker certs, Mosquitto config, CLI
tests/host/              host unit tests, C/Python parity, golden vectors
```

## Hardware

| INMP441 | ESP32 | | Board | ESP32 |
|---|---|---|---|---|
| SCK | GPIO26 | | Status LED | GPIO2 |
| WS | GPIO25 | | Button (BOOT) | GPIO0 |
| SD | GPIO33 | | | |
| L/R | GND | | | |
| VDD / GND | 3V3 / GND | | | |

All pins can be changed in `idf.py menuconfig` → *Keyword spotting*.
GPIO16/17 belong to PSRAM on WROVER modules, so don't use them.

## Quick start (development board)

Prerequisite: ESP-IDF **v5.5** ([install guide](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32/get-started/)).

```bash
cd firmware
idf.py set-target esp32
idf.py build
idf.py -p COM5 flash monitor
```

On first boot the device has no Wi-Fi credentials. It advertises over BLE as `KWS_xxxxxx`,
and the LED blinks slowly.

1. Install Espressif's **ESP BLE Provisioning** app (Android / iOS).
2. Choose *Provision device* → *I don't have a QR code* → select `KWS_xxxxxx`.
3. Enter the username `kws` and password `kws-dev-pass` (the development default; per-device
   passwords come from `tools/provision_device.py`).
4. Pick your Wi-Fi network. The device connects, BLE shuts down and its memory is released.

Say "on" or "off": the LED follows. Detections are logged on the serial console. Hold
BOOT for 5 s to forget Wi-Fi and provision again.

If the device hears nothing, check the `audio.peak` value in the health log. Then try
toggling *Microphone on left slot* or lowering *Right shift* (more gain) in menuconfig.

### MQTT broker (optional for a first test)

```bash
pip install -r tools/requirements.txt
python tools/gen_keys.py
python tools/gen_broker_cert.py --host 192.168.1.10
```

Copy `secrets/broker/*.pem` to `/mosquitto/certs/`, and `tools/mosquitto/{mosquitto.conf,acl}`
to `/mosquitto/config/`. Then run Mosquitto 2.x (for example the `eclipse-mosquitto` container)
and give the device its identity, from an ESP-IDF shell:

```bash
python tools/provision_device.py --broker-uri mqtts://192.168.1.10:8883 --port COM5
```

This writes a per-device certificate, broker URI and BLE password into the `fctry`
partition, and prints the password for the device label.

Topics (all under `devices/<device_id>/`):

| Topic | Direction | Payload |
|---|---|---|
| `status` | device → retained | `{"online":true,"fw":"…"}`; last will `{"online":false}` |
| `events` | device → | `{"keyword":"on","score":0.97,"uptime_ms":…,"ts":<unix ms>}` |
| `health` | device → every 60 s | heap, PSRAM, RSSI, model version, inference latency, audio drops |
| `cmd` | → device | `ping`, `get_health`, `set_threshold`, `ota_firmware`, `ota_model`, `reboot`, `forget_wifi` |
| `cmd/result` | device → | `{"id":…,"op":…,"status":"ok\|accepted\|error","detail":…}` |

Operator CLI (uses the `kws-backend` certificate created by `gen_broker_cert.py`):

```bash
python tools/kws_ctl.py --broker 192.168.1.10 watch
python tools/kws_ctl.py --broker 192.168.1.10 cmd kws-0123456789ab set_threshold --value 0.95
python tools/kws_ctl.py --broker 192.168.1.10 cmd kws-0123456789ab ota_model \
    --url https://updates.local/kws_model_v2.kwsm --file ~/kws-work/kws_model_v2.kwsm
```

## Training a model

Training runs on Linux or WSL2, because the `tflite-micro` runtime ships Linux wheels only.
A GPU is optional (about 2 minutes of training on an RTX-class laptop GPU).

```bash
python -m venv ~/.venvs/kws && . ~/.venvs/kws/bin/activate
pip install -r ml/requirements.txt
curl -LO https://storage.googleapis.com/download.tensorflow.org/data/speech_commands_v0.02.tar.gz
mkdir -p ~/datasets/speech_commands_v0.02 && tar -xzf speech_commands_v0.02.tar.gz -C ~/datasets/speech_commands_v0.02
python tools/gen_keys.py                      # once: model signing key
ml/run_pipeline.sh ~/datasets/speech_commands_v0.02 ~/kws-work 2
```

The pipeline:

1. `prepare_dataset.py` builds features with the **same TFLM preprocessor graph** the
   firmware runs, so features match the device bit for bit. It warms up the
   preprocessor's noise estimate with ambient audio, as on the device, and adds shift,
   gain and background noise augmentation.
2. `train.py` trains the float model.
3. `export_model.py` does int8 post-training quantisation, evaluates the model with the
   TFLM runtime, runs a 30-minute false-accept stream test, and packages a signed `.kwsm`.
   It updates the firmware's embedded factory model and the golden test vectors.
4. `tune_detector.py` sweeps threshold, window and minimum-results settings against recall
   and false accepts on a 30-minute stream with keywords at known positions.

To ship a new model to fielded devices, host `~/kws-work/kws_model_v2.kwsm` on an HTTPS
server and send `ota_model` with its SHA-256.

## Tests

```bash
pip install -r ml/requirements-runtime.txt
python tests/host/run_tests.py --golden   # Linux / WSL
```

- C unit tests: CRC-32, `.kwsm` corruption handling, detector behaviour including timer wrap.
- Format parity: the C and Python `.kwsm` parsers agree.
- Detector parity: 20,000 random steps through the C and Python detectors give identical detections.
- Golden vectors: the factory model's int8 outputs are unchanged.

CI (`.github/workflows/ci.yml`) builds the firmware in `espressif/idf:v5.5` and runs the host tests.

## Status

| Area | State |
|---|---|
| Firmware build (ESP-IDF v5.5, esp32) | Builds cleanly |
| Host tests | Passing (unit, C/Python parity, golden vectors) |
| Model | Trained, int8, signed; meets the size and clip-accuracy targets |
| Streaming false accepts | **Above target** (4–6 / hour against ≤ 1 / hour); see next steps below |
| On-device testing | **Not done**: no board was connected. Latency, arena use, heap, Wi-Fi/BLE coexistence and the OTA/rollback flows still need to be verified on hardware |
| Release security (Secure Boot v2, flash encryption) | Configured in `sdkconfig.release`; **not tried on hardware** (it burns eFuses) |

Stream results come from Speech Commands clips mixed with background noise. Expect
different numbers with your microphone, room and speakers. Tune `set_threshold` per
deployment and collect in-domain audio before relying on the false-accept rate.

Next steps:

- Measure on hardware: inference latency, tensor arena use, heap and current draw.
- Record in-domain audio (with consent, see `docs/SECURITY.md`) and add hard-negative
  mining from false-accept streams to bring false accepts towards 1 / hour.
- Try the release-mode security flow on a spare board.

## Licences and attribution

- Speech Commands v2 dataset: Pete Warden, Google, [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/).
- `audio_preprocessor_int8.tflite` comes from the TensorFlow Lite Micro `micro_speech`
  example (via espressif/esp-tflite-micro), Apache 2.0.
- ESP-IDF, esp-tflite-micro and ESP-NN: Apache 2.0.
