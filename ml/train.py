"""Train the float DS-CNN on cached features.

Usage:
    python train.py --work work --epochs 40
Writes work/float_model.keras and work/history.json.
"""
import argparse
import json
from pathlib import Path

import keras
import numpy as np

import kws_config as cfg
from model import build_model


def load(work, split):
    d = np.load(work / f"features_{split}.npz")
    return d["X"].astype(np.float32)[..., None], d["y"].astype(np.int32)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", type=Path, default=Path("work"))
    ap.add_argument("--epochs", type=int, default=40)
    ap.add_argument("--batch", type=int, default=128)
    ap.add_argument("--lr", type=float, default=3e-3)
    ap.add_argument("--seed", type=int, default=1234)
    args = ap.parse_args()
    keras.utils.set_random_seed(args.seed)

    Xtr, ytr = load(args.work, "train")
    Xva, yva = load(args.work, "val")
    counts = np.bincount(ytr, minlength=len(cfg.LABELS))
    class_weight = {i: float(len(ytr) / (len(cfg.LABELS) * c)) for i, c in enumerate(counts)}
    print("train counts", dict(zip(cfg.LABELS, counts.tolist())))

    model = build_model()
    model.summary()
    steps = args.epochs * int(np.ceil(len(ytr) / args.batch))
    sched = keras.optimizers.schedules.CosineDecay(args.lr, steps, alpha=0.02)
    model.compile(optimizer=keras.optimizers.Adam(sched),
                  loss=keras.losses.SparseCategoricalCrossentropy(),
                  metrics=["accuracy"])
    ckpt = args.work / "float_model.keras"
    hist = model.fit(Xtr, ytr, validation_data=(Xva, yva), epochs=args.epochs,
                     batch_size=args.batch, class_weight=class_weight, verbose=2,
                     callbacks=[keras.callbacks.ModelCheckpoint(ckpt, monitor="val_accuracy",
                                                                save_best_only=True)])
    (args.work / "history.json").write_text(json.dumps(
        {k: [float(v) for v in vals] for k, vals in hist.history.items()}, indent=1))
    best = keras.models.load_model(ckpt)
    print("best val accuracy", best.evaluate(Xva, yva, verbose=0)[1])


if __name__ == "__main__":
    main()
