#!/usr/bin/env bash
set -euo pipefail

if [[ ! -x ./build/ffts_vs_acl_d2d_benchmark ]]; then
  echo "Missing ./build/ffts_vs_acl_d2d_benchmark. Run bash build.sh first." >&2
  exit 1
fi

if [[ -n "${ASCEND_ROOT:-}" ]]; then
  :
elif [[ -n "${ASCEND_HOME:-}" ]]; then
  ASCEND_ROOT=${ASCEND_HOME}
elif [[ -n "${ASCEND_HOME_PATH:-}" ]]; then
  ASCEND_ROOT=${ASCEND_HOME_PATH}
elif [[ -f /usr/local/Ascend/ascend-toolkit/latest/include/acl/acl.h ]]; then
  ASCEND_ROOT=/usr/local/Ascend/ascend-toolkit/latest
else
  ASCEND_ROOT=$(find /usr/local/Ascend -maxdepth 6 -type f -path "*/include/acl/acl.h" 2>/dev/null \
    | sed 's#/include/acl/acl.h$##' \
    | sort -V \
    | tail -n 1 || true)
fi

if [[ -z "${ASCEND_ROOT:-}" ]]; then
  echo "Cannot find CANN root. Set ASCEND_ROOT, ASCEND_HOME, or ASCEND_HOME_PATH." >&2
  exit 1
fi

LIB_PATHS=()
for dir in \
  "${ASCEND_ROOT}/lib64" \
  "${ASCEND_ROOT}/lib" \
  "${ASCEND_ROOT}/runtime/lib64" \
  "${ASCEND_ROOT}/runtime/lib" \
  "${ASCEND_ROOT}/aarch64-linux/lib64" \
  "${ASCEND_ROOT}/aarch64-linux/lib" \
  "/usr/local/Ascend/cann/aarch64-linux/lib64" \
  "/usr/local/Ascend/cann/aarch64-linux/lib"; do
  if [[ -d "${dir}" ]]; then
    LIB_PATHS+=("${dir}")
  fi
done

LD_PREFIX=$(IFS=:; echo "${LIB_PATHS[*]}")
if [[ -n "${LD_PREFIX}" ]]; then
  export LD_LIBRARY_PATH="${LD_PREFIX}:${LD_LIBRARY_PATH:-}"
fi

DEVICE_ID=${DEVICE_ID:-0}
BUFFER_COUNT=${BUFFER_COUNT:-1024}
ITERATIONS=${ITERATIONS:-128}
IO_SIZES=${IO_SIZES:-2K,8K,37K,64K,128K,256K,512K,1024K}

STREAM_COUNT=${STREAM_COUNT:-4}

IFS=',' read -r -a SIZE_LIST <<< "${IO_SIZES}"

for io_size in "${SIZE_LIST[@]}"; do
  echo "=== io_size=${io_size} buffer_count=${BUFFER_COUNT} iterations=${ITERATIONS} streams=${STREAM_COUNT} ==="
  ./build/ffts_vs_acl_d2d_benchmark \
    -d "${DEVICE_ID}" \
    -t all \
    -p both \
    -s "${io_size}" \
    -n "${BUFFER_COUNT}" \
    -i "${ITERATIONS}" \
    -m "${STREAM_COUNT}"
  echo ""
done