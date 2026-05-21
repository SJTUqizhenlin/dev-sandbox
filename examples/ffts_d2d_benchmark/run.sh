#!/usr/bin/env bash
set -euo pipefail

if command -v git >/dev/null 2>&1 && git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "git HEAD=$(git rev-parse --short HEAD)"
fi

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

DEVICE_ID=${DEVICE_ID:-0}
TEST_TYPE=${TEST_TYPE:-all}
COPY_PATH=${COPY_PATH:-both}
IO_SIZE=${IO_SIZE:-64K}
BUFFER_COUNT=${BUFFER_COUNT:-1024}
ITERATIONS=${ITERATIONS:-128}

LD_PREFIX=$(IFS=:; echo "${LIB_PATHS[*]}")
if [[ -n "${LD_PREFIX}" ]]; then
  export LD_LIBRARY_PATH="${LD_PREFIX}:${LD_LIBRARY_PATH:-}"
fi

./build/ffts_vs_acl_d2d_benchmark \
  -d "${DEVICE_ID}" \
  -t "${TEST_TYPE}" \
  -p "${COPY_PATH}" \
  -s "${IO_SIZE}" \
  -n "${BUFFER_COUNT}" \
  -i "${ITERATIONS}"