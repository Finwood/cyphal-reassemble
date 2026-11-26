#!/usr/bin/env bash
# Build cyphal-reassemble and stage the executable into py/cyphal_reassemble/_bin/.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STAGING="${ROOT}/py/cyphal_reassemble/_bin"

cd "${ROOT}"
make

rm -rf "${STAGING}"
mkdir -p "${STAGING}"
cp "${ROOT}/build/cyphal-reassemble" "${STAGING}/cyphal-reassemble"
chmod +x "${STAGING}/cyphal-reassemble"
: > "${STAGING}/.gitkeep"

echo "==> staged executable (pre-auditwheel)"
ls -la "${STAGING}"
ldd "${STAGING}/cyphal-reassemble"
"${STAGING}/cyphal-reassemble" --help
