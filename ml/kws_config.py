"""Single source of truth for audio/feature/model settings shared by training and firmware.

The firmware checks these values against the header of every model it loads
(see firmware/components/kws_model/include/kws_model_format.h).
"""
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

SAMPLE_RATE = 16000
CLIP_SAMPLES = SAMPLE_RATE  # 1 s clips
WINDOW_MS = 30
STRIDE_MS = 20
WINDOW_SAMPLES = SAMPLE_RATE * WINDOW_MS // 1000  # 480
STRIDE_SAMPLES = SAMPLE_RATE * STRIDE_MS // 1000  # 320
FEATURE_FRAMES = 49
FEATURE_CHANNELS = 40

# Output class order is part of the model contract; index 0/1 are never reported as detections.
LABELS = ["_silence_", "_unknown_", "on", "off"]
KEYWORDS = ["on", "off"]

# Must match CONFIG_KWS_INFERENCE_EVERY_N_FRAMES in firmware/main/Kconfig.projbuild.
INFERENCE_EVERY_N_FRAMES = 3

# Firmware detector defaults (Kconfig KWS_DETECTION_THRESHOLD_PCT, KWS_AVERAGE_WINDOW_MS,
# KWS_MIN_RESULTS, KWS_SUPPRESSION_MS), chosen from ml/reports/detector_sweep.json.
DETECTOR_DEFAULTS = dict(threshold=0.93, window_ms=800, min_results=2, suppression_ms=1200)

PREPROCESSOR_MODEL = REPO_ROOT / "firmware/components/kws_inference/models/audio_preprocessor_int8.tflite"
FACTORY_MODEL_OUT = REPO_ROOT / "firmware/components/kws_model/models/kws_factory.kwsm"
REPORTS_DIR = REPO_ROOT / "ml/reports"
