#!/usr/bin/env bash
# Build a py3-none-manylinux platform wheel with bundled _bin/ payload.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTPUT_DIR="${1:-${ROOT}/dist}"
REPAIRED_DIR="${OUTPUT_DIR}/repaired"

cd "${ROOT}"
mkdir -p "${OUTPUT_DIR}"
rm -rf "${REPAIRED_DIR}"

if command -v uv >/dev/null 2>&1; then
  uv pip install build wheel hatchling auditwheel
else
  python -m pip install --upgrade pip build wheel hatchling auditwheel
fi

./scripts/prepare_wheel_bundle.sh

if [[ -n "${CIBUILDWHEEL:-}" ]]; then
  python -m pip install --upgrade pip build wheel hatchling
  python -m build --wheel --outdir "${OUTPUT_DIR}"
elif command -v uv >/dev/null 2>&1; then
  uv build --wheel --out-dir "${OUTPUT_DIR}"
else
  python -m pip install --upgrade pip build wheel hatchling
  python -m build --wheel --outdir "${OUTPUT_DIR}"
fi

WHEEL=( "${OUTPUT_DIR}"/cyphal_reassemble-*.whl )
if [[ ! -f "${WHEEL[0]}" ]]; then
  echo "wheel not found in ${OUTPUT_DIR}" >&2
  exit 1
fi

auditwheel_cmd() {
  if command -v auditwheel >/dev/null 2>&1; then
    auditwheel "$@"
  else
    python -m auditwheel "$@"
  fi
}

echo "==> auditwheel repair (wheel)"
auditwheel_cmd repair -w "${REPAIRED_DIR}" "${WHEEL[@]}"

rm -f "${WHEEL[@]}"
mv "${REPAIRED_DIR}/"*.whl "${OUTPUT_DIR}/"
rmdir "${REPAIRED_DIR}"

WHEEL=( "${OUTPUT_DIR}"/cyphal_reassemble-*.whl )
if [[ $(echo "${WHEEL[@]}" | wc -w) -ne 1 ]]; then
  echo "expected exactly one repaired wheel in ${OUTPUT_DIR}" >&2
  ls -la "${OUTPUT_DIR}"
  exit 1
fi

echo "==> built wheel"
ls -la "${OUTPUT_DIR}"
unzip -l "${WHEEL[@]}" | grep '_bin/cyphal-reassemble'
