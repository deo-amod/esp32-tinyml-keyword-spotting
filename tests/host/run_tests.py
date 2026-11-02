"""Build and run the host tests (Linux / WSL / macOS with a C compiler).

    python tests/host/run_tests.py           # C unit tests + C/Python parity
    python tests/host/run_tests.py --golden  # also check the factory model against golden vectors
                                             # (needs ml/requirements.txt installed)
"""
import argparse
import random
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
COMP = ROOT / "firmware/components"
FACTORY = COMP / "kws_model/models/kws_factory.kwsm"
sys.path.insert(0, str(ROOT / "ml"))


def build():
    out = HERE / "build"
    out.mkdir(exist_ok=True)
    exe = out / "test_host"
    subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-I", str(COMP / "kws_model/include"), "-I", str(COMP / "kws_inference/include"),
                    str(HERE / "test_host.c"), str(COMP / "kws_model/kws_model_format.c"),
                    str(COMP / "kws_inference/kws_detector.c"), "-o", str(exe)], check=True)
    return exe


def test_unit(exe):
    subprocess.run([str(exe), "unit", str(FACTORY)], check=True)


def test_python_format_matches_c(exe):
    import kwsm_format
    hdr, _ = kwsm_format.parse(FACTORY.read_bytes())
    out = subprocess.run([str(exe), "check-model", str(FACTORY)], check=True, capture_output=True,
                         text=True).stdout
    assert f"version={hdr.model_version} size={hdr.model_size}" in out, out
    assert "labels=" + ",".join(hdr.labels) in out, out
    print("kwsm format: C and Python agree")


def test_detector_parity(exe):
    """Random probability streams through the C and Python detectors must give identical detections."""
    from detector import Detector
    rng = random.Random(3)
    lines, t = [], 0
    burst_left, burst_label = 0, 0
    for _ in range(20000):
        t += 60
        if burst_left == 0 and rng.random() < 0.05:
            burst_left, burst_label = rng.randint(2, 20), rng.choice([1, 2, 3])
        p = [rng.random() * 0.1 for _ in range(4)]
        if burst_left:
            p[burst_label] += rng.uniform(0.4, 3.0)
            burst_left -= 1
        s = sum(p)
        lines.append([t] + [round(x / s, 4) for x in p])
    text = "\n".join(" ".join(str(v) for v in row) for row in lines) + "\n"
    c_out = subprocess.run([str(exe), "replay"], input=text, check=True, capture_output=True, text=True).stdout
    c_hits = [(int(a), int(b)) for a, b, _ in (l.split() for l in c_out.splitlines())]

    det = Detector(4, threshold=0.8, window_ms=600, min_results=3, suppression_ms=1200)  # = default_cfg() in C
    py_hits = [(row[0], r[0]) for row in lines if (r := det.update(row[1:], row[0]))]
    assert c_hits == py_hits, f"C {len(c_hits)} vs Python {len(py_hits)} detections"
    print(f"detector parity: {len(c_hits)} identical detections")


def test_golden():
    import json
    import numpy as np
    import kwsm_format
    from int8_model import Int8Model
    _, tflite = kwsm_format.parse(FACTORY.read_bytes())
    model = Int8Model(tflite)
    for case in json.loads((HERE / "golden/model_golden.json").read_text()):
        x = np.array(case["features"], dtype=np.int8).reshape(49, 40)
        got = model.raw(x).astype(int).tolist()
        assert got == case["expected_output_int8"], (case["label"], got)
    print("golden vectors: factory model output unchanged")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--golden", action="store_true")
    args = ap.parse_args()
    exe = build()
    test_unit(exe)
    test_python_format_matches_c(exe)
    test_detector_parity(exe)
    if args.golden:
        test_golden()


if __name__ == "__main__":
    main()
