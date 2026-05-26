#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build}"
COPY_BIN="${COPY_BIN:-${BUILD_DIR}/module/copy/copy}"
TRANS_BIN="${TRANS_BIN:-${BUILD_DIR}/module/trans/trans}"

DEVICES="${DEVICES:-8}"
COPY_BLOCKS="${COPY_BLOCKS:-1024}"
TRANS_BLOCKS="${TRANS_BLOCKS:-1024}"
ITERS="${ITERS:-128}"
HOST_KIND="${HOST_KIND:-normal}"
DEVICE_KIND="${DEVICE_KIND:-normal}"

COPY_SIZES=("4K" "16K" "32K" "64K" "128K")
TRANS_SIZES=("4096" "16384" "32768" "65536" "131072")

LOG_DIR="${LOG_DIR:-${REPO_ROOT}/logs/h2d-$(date +%Y%m%d-%H%M%S)}"
mkdir -p "${LOG_DIR}"

require_executable() {
    local path="$1"
    if [[ ! -x "${path}" ]]; then
        echo "[error] executable not found: ${path}" >&2
        echo "[error] run scripts/build.sh first, or set BUILD_DIR/COPY_BIN/TRANS_BIN." >&2
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
require_executable "${TRANS_BIN}"

echo "[test] repo root: ${REPO_ROOT}"
echo "[test] copy bin: ${COPY_BIN}"
echo "[test] trans bin: ${TRANS_BIN}"
echo "[test] logs: ${LOG_DIR}"
echo "[test] devices for all-to-all: ${DEVICES}"
echo "[test] copy blocks per device: ${COPY_BLOCKS}"
echo "[test] trans blocks per device: ${TRANS_BLOCKS}"
echo "[test] iterations: ${ITERS}"

echo
echo "========== single device H2D: copy host_to_device_ce =========="
for size in "${COPY_SIZES[@]}"; do
    run_and_log "copy-single-${size}" \
        "${COPY_BIN}" -t host_to_device_ce -s "${size}" -n "${COPY_BLOCKS}" -i "${ITERS}" -d 1
done

echo
echo "========== multi-device simultaneous H2D: copy all_host_to_all_device_ce =========="
for size in "${COPY_SIZES[@]}"; do
    run_and_log "copy-all2all-${size}-d${DEVICES}" \
        "${COPY_BIN}" -t all_host_to_all_device_ce -s "${size}" -n "${COPY_BLOCKS}" -i "${ITERS}" -d "${DEVICES}"
done

echo
echo "========== multi-device threaded-submit H2D: copy all_host_to_all_device_ce_multi_thread =========="
for size in "${COPY_SIZES[@]}"; do
    run_and_log "copy-all2all-mt-${size}-d${DEVICES}" \
        "${COPY_BIN}" -t all_host_to_all_device_ce_multi_thread -s "${size}" -n "${COPY_BLOCKS}" -i "${ITERS}" -d "${DEVICES}"
done

echo
echo "========== multi-stream H2D: trans ms_48 =========="
for size in "${TRANS_SIZES[@]}"; do
    run_and_log "trans-ms48-${size}" \
        "${TRANS_BIN}" -t H2D -H "${HOST_KIND}" -D "${DEVICE_KIND}" -M ms_48 -s "${size}" -n "${TRANS_BLOCKS}" -i "${ITERS}" -d 1
done

echo
echo "[test] done"
echo "[test] logs saved in ${LOG_DIR}"

