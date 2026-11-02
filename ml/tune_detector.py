"""Sweep detector settings on a continuous stream to trade recall against false accepts.

Builds one long stream (background noise + unknown words + test-set keywords at known
positions), runs features and the int8 model once, then replays the probabilities
through ml/detector.py for each parameter combination.

Usage:
    python tune_detector.py --work work --data ~/datasets/speech_commands_v0.02 \
        --model work/kws_model_v1.kwsm --minutes 30
Writes ml/reports/detector_sweep.json.
"""
import argparse
import itertools
import json
from pathlib import Path

import numpy as np
import soundfile as sf

import kws_config as cfg
import kwsm_format
from detector import Detector
from features import FeatureExtractor
from int8_model import Int8Model


def load_clips(data, names):
    out = []
    for p in names:
        a = sf.read(data / p, dtype="float32")[0]
        out.append(np.pad(a, (0, max(0, cfg.CLIP_SAMPLES - len(a))))[:cfg.CLIP_SAMPLES])
    return out


def build_stream(noise, unknown, keywords, seconds, rng, keyword_prob=0.25):
    total = seconds * cfg.SAMPLE_RATE
    stream = np.resize(noise, total).astype(np.float32) * 0.3
    events = []  # (label_index, centre_ms)
    pos = 0
    while pos + cfg.CLIP_SAMPLES < total:
        if rng.random() < keyword_prob:
            label, clip = keywords[rng.integers(len(keywords))]
            events.append((label, (pos + cfg.CLIP_SAMPLES // 2) * 1000 // cfg.SAMPLE_RATE))
        else:
            clip = unknown[rng.integers(len(unknown))]
        stream[pos:pos + cfg.CLIP_SAMPLES] += clip * rng.uniform(0.5, 1.2)
        pos += cfg.CLIP_SAMPLES + int(rng.integers(cfg.SAMPLE_RATE // 4, cfg.SAMPLE_RATE * 2))
    return (np.clip(stream, -1, 1) * 32767).astype(np.int16), events


def run_model(model, pcm):
    fe = FeatureExtractor()
    frames = np.zeros((cfg.FEATURE_FRAMES, cfg.FEATURE_CHANNELS), np.int8)
    results = []  # (now_ms, probs)
    for i in range((len(pcm) - cfg.WINDOW_SAMPLES) // cfg.STRIDE_SAMPLES):
        s = i * cfg.STRIDE_SAMPLES
        frames = np.roll(frames, -1, axis=0)
        frames[-1] = fe.frame(pcm[s:s + cfg.WINDOW_SAMPLES])
        if i >= cfg.FEATURE_FRAMES and i % cfg.INFERENCE_EVERY_N_FRAMES == 0:
            results.append((i * cfg.STRIDE_MS, model.probs(frames)))
    return results


def score(results, events, seconds, **params):
    det = Detector(len(cfg.LABELS), **params)
    detections = [(t, r[0]) for t, p in results if (r := det.update(p, t))]
    matched = set()
    false_accepts = 0
    for t, label in detections:
        # A detection counts as a hit if it lands within 1.5 s after the keyword centre.
        hit = next((k for k, (lab, c) in enumerate(events)
                    if k not in matched and lab == label and 0 <= t - c <= 1500), None)
        if hit is None:
            false_accepts += 1
        else:
            matched.add(hit)
    return len(matched) / max(1, len(events)), false_accepts * 3600.0 / seconds


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", type=Path, default=Path("work"))
    ap.add_argument("--data", type=Path, required=True)
    ap.add_argument("--model", type=Path, required=True)
    ap.add_argument("--minutes", type=int, default=30)
    args = ap.parse_args()

    _, tflite = kwsm_format.parse(args.model.read_bytes())
    model = Int8Model(tflite)
    test_list = (args.data / "testing_list.txt").read_text().split()
    unknown = load_clips(args.data, [p for p in test_list if p.split("/")[0] not in cfg.KEYWORDS])
    keywords = [(cfg.LABELS.index(p.split("/")[0]), c) for p in test_list
                if p.split("/")[0] in cfg.KEYWORDS for c in load_clips(args.data, [p])]
    rng = np.random.default_rng(11)
    seconds = args.minutes * 60
    pcm, events = build_stream(np.load(args.work / "noise.npy"), unknown, keywords, seconds, rng)
    results = run_model(model, pcm)
    print(f"stream {args.minutes} min, {len(events)} keywords, {len(results)} inferences")

    rows = []
    for th, win, mn in itertools.product([0.80, 0.85, 0.90, 0.93, 0.95, 0.97],
                                         [400, 600, 800, 1000], [2, 3, 4, 5, 6]):
        recall, fa = score(results, events, seconds, threshold=th, window_ms=win,
                           min_results=mn, suppression_ms=1200)
        rows.append({"threshold": th, "window_ms": win, "min_results": mn,
                     "recall": round(recall, 4), "false_accepts_per_hour": round(fa, 2)})
    rows.sort(key=lambda r: (r["false_accepts_per_hour"], -r["recall"]))
    best = [r for r in rows if r["false_accepts_per_hour"] <= 2.0]
    best.sort(key=lambda r: -r["recall"])
    for r in (best or rows)[:10]:
        print(r)
    cfg.REPORTS_DIR.mkdir(parents=True, exist_ok=True)
    (cfg.REPORTS_DIR / "detector_sweep.json").write_text(json.dumps(
        {"stream_minutes": args.minutes, "keywords_in_stream": len(events), "results": rows}, indent=1) + "\n")


if __name__ == "__main__":
    main()
