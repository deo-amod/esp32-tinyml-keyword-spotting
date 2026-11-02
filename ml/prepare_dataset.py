"""Build int8 feature caches from Google Speech Commands v2.

Usage:
    python prepare_dataset.py --data ~/datasets/speech_commands_v0.02 --out work

Writes work/features_{train,val,test}.npz (X: int8 [N,49,40], y: uint8 [N]) plus
work/noise.npy (background noise, used by the streaming false-accept test).
"""
import argparse
import multiprocessing as mp
import random
from pathlib import Path

import numpy as np
import soundfile as sf

import kws_config as cfg

UNKNOWN_RATIO = 1.5     # unknown-word clips per keyword clip
SILENCE_RATIO = 0.15    # silence clips per keyword clip
TRAIN_AUGMENTS = 3      # augmented copies of each training clip
MAX_SHIFT = cfg.SAMPLE_RATE // 5
BACKGROUND_MAX_VOL = 0.3
WARMUP_SAMPLES = cfg.SAMPLE_RATE  # ambient audio fed before each clip (see FeatureExtractor.clip)

_extractor = None
_noise = None


def _read(path):
    audio, sr = sf.read(path, dtype="float32")
    if sr != cfg.SAMPLE_RATE:
        raise ValueError(f"{path}: expected {cfg.SAMPLE_RATE} Hz, got {sr}")
    if audio.shape[0] < cfg.CLIP_SAMPLES:
        audio = np.pad(audio, (0, cfg.CLIP_SAMPLES - audio.shape[0]))
    return audio[:cfg.CLIP_SAMPLES]


def _noise_slice(rng, n):
    clip = _noise[rng.randrange(len(_noise))]
    start = rng.randrange(len(clip) - n)
    return clip[start:start + n]


def _augment(audio, rng):
    shift = rng.randint(-MAX_SHIFT, MAX_SHIFT)
    audio = np.roll(audio, shift)
    if shift > 0:
        audio[:shift] = 0
    elif shift < 0:
        audio[shift:] = 0
    return audio * rng.uniform(0.6, 1.4)


def _to_int16(audio):
    return (np.clip(audio, -1.0, 1.0) * 32767).astype(np.int16)


def _init_worker(noise):
    global _extractor, _noise
    from features import FeatureExtractor
    _extractor = FeatureExtractor()
    _noise = noise


def _process(job):
    path, label, augment, seed = job
    rng = random.Random(seed)
    # Continuous ambient background: the first second warms up the preprocessor
    # state, the second second is mixed under the clip.
    vol = rng.uniform(0, BACKGROUND_MAX_VOL) if rng.random() < 0.9 else 0.0
    bg = _noise_slice(rng, WARMUP_SAMPLES + cfg.CLIP_SAMPLES) * vol
    if path is None:  # silence
        audio = np.zeros(cfg.CLIP_SAMPLES, np.float32)
    else:
        audio = _read(path)
        if augment:
            audio = _augment(audio, rng)
    audio = audio + bg[WARMUP_SAMPLES:]
    return _extractor.clip(_to_int16(audio), warmup=_to_int16(bg[:WARMUP_SAMPLES])), label


def _split_lists(root):
    val = set((root / "validation_list.txt").read_text().split())
    test = set((root / "testing_list.txt").read_text().split())
    splits = {"train": [], "val": [], "test": []}
    for wav in sorted(root.glob("*/*.wav")):
        word = wav.parent.name
        if word.startswith("_"):
            continue
        rel = f"{word}/{wav.name}"
        split = "val" if rel in val else "test" if rel in test else "train"
        splits[split].append((wav, word))
    return splits


def _jobs(items, rng, augment):
    kw = [(p, cfg.LABELS.index(w)) for p, w in items if w in cfg.KEYWORDS]
    unk = [(p, 1) for p, w in items if w not in cfg.KEYWORDS]
    rng.shuffle(unk)
    unk = unk[:int(len(kw) * UNKNOWN_RATIO)]
    sil = [(None, 0)] * int(len(kw) * SILENCE_RATIO)
    base = kw + unk + sil
    copies = TRAIN_AUGMENTS if augment else 1
    return [(str(p) if p else None, y, augment, rng.getrandbits(32)) for _ in range(copies) for p, y in base]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", type=Path, required=True)
    ap.add_argument("--out", type=Path, default=Path("work"))
    ap.add_argument("--seed", type=int, default=1234)
    ap.add_argument("--workers", type=int, default=mp.cpu_count())
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    noise = [_read_full(p) for p in sorted((args.data / "_background_noise_").glob("*.wav"))]
    np.save(args.out / "noise.npy", np.concatenate(noise))
    rng = random.Random(args.seed)
    splits = _split_lists(args.data)

    with mp.Pool(args.workers, initializer=_init_worker, initargs=(noise,)) as pool:
        for name, items in splits.items():
            jobs = _jobs(items, rng, augment=(name == "train"))
            results = pool.map(_process, jobs, chunksize=64)
            X = np.stack([r[0] for r in results])
            y = np.array([r[1] for r in results], dtype=np.uint8)
            np.savez_compressed(args.out / f"features_{name}.npz", X=X, y=y)
            counts = {cfg.LABELS[i]: int((y == i).sum()) for i in range(len(cfg.LABELS))}
            print(f"{name}: {len(y)} clips {counts}")


def _read_full(path):
    audio, sr = sf.read(path, dtype="float32")
    assert sr == cfg.SAMPLE_RATE, path
    return audio


if __name__ == "__main__":
    main()
