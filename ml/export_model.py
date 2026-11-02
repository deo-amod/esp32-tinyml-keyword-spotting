"""Quantise to int8, verify with the TFLM runtime, and package a signed .kwsm blob.

Usage:
    python export_model.py --work work --version 1 \
        --key ../secrets/model_signing_key.pem --data ~/datasets/speech_commands_v0.02

Outputs:
    firmware/components/kws_model/models/kws_factory.kwsm  (embedded factory model)
    work/kws_model_v<version>.kwsm                             (for model OTA)
    ml/reports/metrics.json                                    (accuracy / false-accept report)
    tests/host/golden/                                         (vectors for firmware tests)
"""
import argparse
import json
from pathlib import Path

import keras
import numpy as np
import tensorflow as tf
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, utils

import kws_config as cfg
import kwsm_format
from detector import Detector
from features import FeatureExtractor
from int8_model import Int8Model

def quantize(keras_model, rep_X, work):
    saved = work / "saved_model"
    keras_model.export(str(saved))
    conv = tf.lite.TFLiteConverter.from_saved_model(str(saved))
    conv.optimizations = [tf.lite.Optimize.DEFAULT]

    def rep():
        for x in rep_X:
            yield [x[None, ..., None].astype(np.float32)]

    conv.representative_dataset = rep
    conv.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    conv.inference_input_type = tf.int8
    conv.inference_output_type = tf.int8
    return conv.convert()


def evaluate(model, X, y):
    n = len(cfg.LABELS)
    conf = np.zeros((n, n), dtype=int)
    for x, t in zip(X, y):
        conf[t, int(np.argmax(model.probs(x)))] += 1
    return float(np.trace(conf) / conf.sum()), conf


def false_accepts_per_hour(model, noise, unknown_clips, seconds, seed=7):
    """Stream unknown (non-keyword) speech over background noise through the detector."""
    rng = np.random.default_rng(seed)
    total = seconds * cfg.SAMPLE_RATE
    stream = np.resize(noise, total).astype(np.float32) * 0.3
    pos = 0
    while pos + cfg.CLIP_SAMPLES < total:
        clip = unknown_clips[rng.integers(len(unknown_clips))]
        stream[pos:pos + cfg.CLIP_SAMPLES] += clip * rng.uniform(0.5, 1.2)
        pos += cfg.CLIP_SAMPLES + int(rng.integers(cfg.SAMPLE_RATE // 4, cfg.SAMPLE_RATE * 2))
    pcm = (np.clip(stream, -1, 1) * 32767).astype(np.int16)

    fe = FeatureExtractor()
    det = Detector(len(cfg.LABELS))
    frames = np.zeros((cfg.FEATURE_FRAMES, cfg.FEATURE_CHANNELS), np.int8)
    detections = 0
    n_frames = (total - cfg.WINDOW_SAMPLES) // cfg.STRIDE_SAMPLES
    for i in range(n_frames):
        s = i * cfg.STRIDE_SAMPLES
        frames = np.roll(frames, -1, axis=0)
        frames[-1] = fe.frame(pcm[s:s + cfg.WINDOW_SAMPLES])
        if i >= cfg.FEATURE_FRAMES and i % cfg.INFERENCE_EVERY_N_FRAMES == 0:
            if det.update(model.probs(frames), i * cfg.STRIDE_MS):
                detections += 1
    return detections * 3600.0 / seconds


def write_golden(model, test, out_dir):
    """Golden vectors so firmware/host tests can check the int8 pipeline end to end."""
    out_dir.mkdir(parents=True, exist_ok=True)
    picks = [int(np.flatnonzero(test["y"] == i)[0]) for i in range(len(cfg.LABELS))]
    cases = []
    for idx in picks:
        x = test["X"][idx]
        cases.append({"label": cfg.LABELS[int(test["y"][idx])],
                      "features": x.astype(int).ravel().tolist(),
                      "expected_output_int8": model.raw(x).astype(int).tolist()})
    (out_dir / "model_golden.json").write_text(json.dumps(cases))


def load_signer(path):
    if path is None:
        return None
    key = serialization.load_pem_private_key(Path(path).read_bytes(), password=None)
    if not isinstance(key, ec.EllipticCurvePrivateKey) or key.curve.name != "secp256r1":
        raise SystemExit("model signing key must be ECDSA P-256")
    return lambda digest: key.sign(digest, ec.ECDSA(utils.Prehashed(hashes.SHA256())))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", type=Path, default=Path("work"))
    ap.add_argument("--version", type=int, required=True)
    ap.add_argument("--key", type=Path, help="ECDSA P-256 private key (PEM) used to sign the model")
    ap.add_argument("--data", type=Path, help="Speech Commands root, enables the false-accept test")
    ap.add_argument("--fa-seconds", type=int, default=1800)
    args = ap.parse_args()

    keras_model = keras.models.load_model(args.work / "float_model.keras")
    train = np.load(args.work / "features_train.npz")
    test = np.load(args.work / "features_test.npz")
    rng = np.random.default_rng(0)
    rep = train["X"][rng.choice(len(train["X"]), 1000, replace=False)]

    tflite = quantize(keras_model, rep, args.work)
    model = Int8Model(tflite)
    acc, conf = evaluate(model, test["X"], test["y"])
    float_pred = np.argmax(keras_model.predict(test["X"][..., None].astype(np.float32), verbose=0), 1)
    float_acc = float(np.mean(float_pred == test["y"]))
    print(f"int8 test accuracy {acc:.4f} (float {float_acc:.4f}), tflite {len(tflite)} bytes")
    print("confusion (rows=true, cols=pred)", cfg.LABELS)
    print(conf)

    report = {
        "model_version": args.version,
        "tflite_bytes": len(tflite),
        "labels": cfg.LABELS,
        "test_clips": int(len(test["y"])),
        "test_accuracy_int8": round(acc, 4),
        "test_accuracy_float": round(float_acc, 4),
        "confusion_matrix_rows_true_cols_pred": conf.tolist(),
        "input_quantization": {"scale": model.in_scale, "zero_point": model.in_zp},
    }
    if args.data:
        import soundfile as sf
        test_list = (args.data / "testing_list.txt").read_text().split()
        unk = []
        for p in test_list:
            if p.split("/")[0] in cfg.KEYWORDS:
                continue
            a = sf.read(args.data / p, dtype="float32")[0]
            unk.append(np.pad(a, (0, max(0, cfg.CLIP_SAMPLES - len(a))))[:cfg.CLIP_SAMPLES])
        fa = false_accepts_per_hour(model, np.load(args.work / "noise.npy"), unk, args.fa_seconds)
        report["false_accept_test"] = {"stream_seconds": args.fa_seconds,
                                       "false_accepts_per_hour": round(fa, 2)}
        print(f"false accepts/hour on unknown-speech stream: {fa:.2f}")

    blob = kwsm_format.build(tflite, args.version, cfg.LABELS, cfg, sign=load_signer(args.key))
    kwsm_format.parse(blob)  # self-check
    cfg.FACTORY_MODEL_OUT.parent.mkdir(parents=True, exist_ok=True)
    cfg.FACTORY_MODEL_OUT.write_bytes(blob)
    (args.work / f"kws_model_v{args.version}.kwsm").write_bytes(blob)
    write_golden(model, test, cfg.REPO_ROOT / "tests/host/golden")
    cfg.REPORTS_DIR.mkdir(parents=True, exist_ok=True)
    (cfg.REPORTS_DIR / "metrics.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"wrote {cfg.FACTORY_MODEL_OUT.name} ({len(blob)} bytes, signed={args.key is not None})")


if __name__ == "__main__":
    main()
