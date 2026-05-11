#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${BIN:-${SCRIPT_DIR}/h2d_d2h_async_memcpy}"

IO_SIZE="${IO_SIZE:-64K}"
ITERS="${ITERS:-128}"
TEST_TYPE="${TEST_TYPE:-single_stream}"
LOG_DIR="${LOG_DIR:-${SCRIPT_DIR}/logs/n-sweep-${TEST_TYPE}-$(date +%Y%m%d-%H%M%S)}"

if [[ ! -x "${BIN}" ]]; then
    echo "[error] executable not found: ${BIN}" >&2
    echo "[error] build h2d_d2h_async_memcpy first, or set BIN=/path/to/binary." >&2
    exit 1
fi

mkdir -p "${LOG_DIR}"

echo "[sweep] bin=${BIN}"
echo "[sweep] test_type=${TEST_TYPE}, io_size=${IO_SIZE}, iterations=${ITERS}"
echo "[sweep] logs=${LOG_DIR}"

for n in 10 50 100 300 500 1000 2000 3000 5000 7500 10000; do
    log_file="${LOG_DIR}/n-${n}.log"
    echo
    echo "[run] -t ${TEST_TYPE} -s ${IO_SIZE} -n ${n} -i ${ITERS}"
    "${BIN}" -t "${TEST_TYPE}" -s "${IO_SIZE}" -n "${n}" -i "${ITERS}" 2>&1 | tee "${log_file}"
done

echo
echo "[done] logs saved in ${LOG_DIR}"
