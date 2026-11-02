#!/usr/bin/env bash
# End-to-end model build: features -> train -> int8 export (signed) -> detector sweep.
#
#   ml/run_pipeline.sh <speech_commands_dir> <work_dir> <model_version>
#
# Requires the Python environment from ml/requirements.txt (Linux / WSL2) and
# secrets/model_signing_key.pem (python tools/gen_keys.py).
set -euo pipefail

DATA=${1:?speech commands directory}
WORK=${2:?work directory}
VERSION=${3:?model version (integer)}
cd "$(dirname "$0")"
export TF_CPP_MIN_LOG_LEVEL=2

python prepare_dataset.py --data "$DATA" --out "$WORK"
python train.py --work "$WORK" --epochs 40 > "$WORK/train.log" 2>&1
grep "best val" "$WORK/train.log"
python export_model.py --work "$WORK" --version "$VERSION" \
    --key ../secrets/model_signing_key.pem --data "$DATA" 2>&1 | grep -A6 "int8 test\|false accepts\|wrote"
python tune_detector.py --work "$WORK" --data "$DATA" --model "$WORK/kws_model_v$VERSION.kwsm" --minutes 30 \
    2>&1 | tail -11
