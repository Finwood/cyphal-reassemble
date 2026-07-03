#!/usr/bin/env bash
# Smoke-test an installed cyphal-reassemble wheel (used by cibuildwheel).
set -euo pipefail

PROJECT="${1:-.}"
cd "${PROJECT}"

PYTHON="${PYTHON:-python}"
if ! command -v "${PYTHON}" >/dev/null 2>&1; then
  PYTHON=python3
fi
PYTHON="$(command -v "${PYTHON}")"

unset CYPHAL_REASSEMBLE_BIN
export PATH="/usr/local/bin:/usr/bin:/bin"

"${PYTHON}" -m pip install pytest "pyarrow>=15"
"${PYTHON}" -m pytest python_tests/ -v

"${PYTHON}" - <<'PY'
from cyphal_reassemble._backend import resolve_binary

path = resolve_binary()
assert "_bin" in str(path), path
assert path.is_file(), path
print("resolve_binary:", path)
PY
