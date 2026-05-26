#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build}"
COPY_BIN="${COPY_BIN:-${BUILD_DIR}/module/copy/copy}"

DEVICES="${DEVICES:-8}"
COPY_BLOCKS="${COPY_BLOCKS:-1024}"
ITERS="${ITERS:-128}"
LOG_DIR="${LOG_DIR:-${REPO_ROOT}/logs/all2all-ce-submit-$(date +%Y%m%d-%H%M%S)}"

if [[ -n "${SIZES:-}" ]]; then
    read -r -a COPY_SIZES <<< "${SIZES}"
else
    COPY_SIZES=("4K" "16K" "32K" "64K" "128K")
fi

require_executable() {
    local path="$1"
    if [[ ! -x "${path}" ]]; then
        echo "[error] executable not found: ${path}" >&2
        echo "[error] run scripts/build.sh first, or set BUILD_DIR/COPY_BIN." >&2
        exit 1
    fi
}

run_and_log() {
    local name="$1"
    shift
    local log_file="${LOG_DIR}/${name}.log"
    echo
    echo "[run] ${name}"
    echo "[cmd] $*"
    "$@" 2>&1 | tee "${log_file}"
}

require_executable "${COPY_BIN}"
mkdir -p "${LOG_DIR}"

echo "[compare] repo root: ${REPO_ROOT}"
echo "[compare] copy bin: ${COPY_BIN}"
echo "[compare] logs: ${LOG_DIR}"
echo "[compare] devices: ${DEVICES}"
echo "[compare] blocks per device: ${COPY_BLOCKS}"
echo "[compare] iterations: ${ITERS}"
echo "[compare] sizes: ${COPY_SIZES[*]}"

for size in "${COPY_SIZES[@]}"; do
    run_and_log "all2all-ce-seq-${size}-d${DEVICES}" \
        "${COPY_BIN}" -t all_host_to_all_device_ce -s "${size}" -n "${COPY_BLOCKS}" -i "${ITERS}" -d "${DEVICES}"

    run_and_log "all2all-ce-mt-${size}-d${DEVICES}" \
        "${COPY_BIN}" -t all_host_to_all_device_ce_multi_thread -s "${size}" -n "${COPY_BLOCKS}" -i "${ITERS}" -d "${DEVICES}"
done

echo
echo "[compare] done"
echo "[compare] logs saved in ${LOG_DIR}"
