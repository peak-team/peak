#!/usr/bin/env bash
set -euo pipefail
PY="${PYTHON_BIN:-python3}"

EXT="@PY_EXT_SUFFIX@"
MOD_SRC="customCpackage_staA_staB${EXT}"
MOD_DST="customCpackage${EXT}"

if [[ ! -f "$MOD_SRC" ]]; then
  echo "ERROR: $MOD_SRC not found in $(pwd)"; exit 2
fi

cp -f "$MOD_SRC" "$MOD_DST"

"$PY" - <<'PYCODE'
import sys, os
print("Imported customCpackage OK")  # anchor for PASS_REGULAR_EXPRESSION
import customCpackage as cp
cp.a_call()
print("Done.")
PYCODE