#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"

if command -v nproc >/dev/null 2>&1; then
    DEFAULT_JOBS="$(nproc)"
else
    DEFAULT_JOBS="8"
fi
JOBS="${JOBS:-${DEFAULT_JOBS}}"

echo "[build] repo root: ${REPO_ROOT}"
echo "[build] build dir: ${BUILD_DIR}"
echo "[build] type: ${BUILD_TYPE}"
echo "[build] jobs: ${JOBS}"

cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
cmake --build "${BUILD_DIR}" -j "${JOBS}"

echo "[build] done"

