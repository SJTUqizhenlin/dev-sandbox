#!/usr/bin/env bash
set -euo pipefail

mkdir -p build

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

INCLUDE_DIR=${ASCEND_ROOT}/include
if [[ ! -f "${INCLUDE_DIR}/acl/acl.h" ]]; then
  echo "Cannot find acl/acl.h under ${INCLUDE_DIR}." >&2
  exit 1
fi

INCLUDE_DIRS=("${INCLUDE_DIR}")
FFTS_INCLUDE_DIR=""

add_include_dir() {
  local dir="$1"
  if [[ -z "${dir}" || ! -d "${dir}" ]]; then
    return
  fi
  local existing
  for existing in "${INCLUDE_DIRS[@]}"; do
    if [[ "${existing}" == "${dir}" ]]; then
      return
    fi
  done
  INCLUDE_DIRS+=("${dir}")
}

for dir in \
  "${INCLUDE_DIR}" \
  "${ASCEND_ROOT}/pkg_inc/runtime" \
  "${ASCEND_ROOT}/aarch64-linux/pkg_inc/runtime" \
  "/usr/local/Ascend/cann/aarch64-linux/pkg_inc/runtime"; do
  if [[ -f "${dir}/runtime/rt_ffts_plus.h" || -f "${dir}/rt_external_ffts.h" ]]; then
    FFTS_INCLUDE_DIR="${dir}"
    break
  fi
done

if [[ -z "${FFTS_INCLUDE_DIR}" ]]; then
  FFTS_HEADER=$(find /usr/local/Ascend -maxdepth 8 -type f \
    \( -path "*/runtime/rt_ffts_plus.h" -o -path "*/rt_external_ffts.h" \) 2>/dev/null \
    | sort -V \
    | tail -n 1 || true)
  if [[ -n "${FFTS_HEADER}" ]]; then
    case "${FFTS_HEADER}" in
      */runtime/rt_ffts_plus.h)
        FFTS_INCLUDE_DIR=${FFTS_HEADER%/runtime/rt_ffts_plus.h}
        ;;
      */rt_external_ffts.h)
        FFTS_INCLUDE_DIR=$(dirname "${FFTS_HEADER}")
        ;;
    esac
  fi
fi

if [[ -n "${FFTS_INCLUDE_DIR}" ]]; then
  echo "FFTS_HEADER=official"
  echo "FFTS_INCLUDE_DIR=${FFTS_INCLUDE_DIR}"
  add_include_dir "${FFTS_INCLUDE_DIR}"
  if [[ "${FFTS_INCLUDE_DIR}" == */pkg_inc/runtime ]]; then
    add_include_dir "${FFTS_INCLUDE_DIR%/runtime}"
  fi
else
  echo "FFTS_HEADER=minimal fallback in src/ffts_plus_minimal_runtime.h"
  echo "Warning: CANN did not expose FFTS Plus headers under detected include paths."
  echo "         The benchmark will compile with the minimal local struct definitions."
fi

for dir in \
  "${ASCEND_ROOT}/pkg_inc" \
  "${ASCEND_ROOT}/pkg_inc/toolchain" \
  "${ASCEND_ROOT}/pkg_inc/profiling" \
  "${ASCEND_ROOT}/aarch64-linux/pkg_inc" \
  "${ASCEND_ROOT}/aarch64-linux/pkg_inc/toolchain" \
  "${ASCEND_ROOT}/aarch64-linux/pkg_inc/profiling" \
  "/usr/local/Ascend/cann/aarch64-linux/pkg_inc" \
  "/usr/local/Ascend/cann/aarch64-linux/pkg_inc/toolchain" \
  "/usr/local/Ascend/cann/aarch64-linux/pkg_inc/profiling"; do
  if [[ -f "${dir}/toolchain/prof_api.h" || -f "${dir}/profiling/prof_api.h" ]]; then
    add_include_dir "${dir}"
  elif [[ -f "${dir}/prof_api.h" || -f "${dir}/prof_common.h" ]]; then
    add_include_dir "${dir}"
  fi
done

for header in prof_api.h prof_common.h; do
  while IFS= read -r found_header; do
    if [[ -n "${found_header}" ]]; then
      add_include_dir "$(dirname "${found_header}")"
    fi
  done < <(find /usr/local/Ascend -maxdepth 8 -type f -name "${header}" 2>/dev/null | sort -V)
done

if [[ -n "${EXTRA_INCLUDE_DIRS:-}" ]]; then
  IFS=':' read -r -a EXTRA_DIR_ARRAY <<< "${EXTRA_INCLUDE_DIRS}"
  for dir in "${EXTRA_DIR_ARRAY[@]}"; do
    add_include_dir "${dir}"
  done
fi

LIB_DIRS=()
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
    LIB_DIRS+=("${dir}")
  fi
done

if [[ ${#LIB_DIRS[@]} -eq 0 ]]; then
  echo "Cannot find any CANN lib directory under ${ASCEND_ROOT}." >&2
  exit 1
fi

LIB_FLAGS=()
RPATH=""
for dir in "${LIB_DIRS[@]}"; do
  LIB_FLAGS+=("-L${dir}")
  if [[ -z "${RPATH}" ]]; then
    RPATH="${dir}"
  else
    RPATH="${RPATH}:${dir}"
  fi
done

LINK_LIBS=("-lascendcl")
for dir in "${LIB_DIRS[@]}"; do
  if [[ -f "${dir}/libruntime.so" ]]; then
    LINK_LIBS+=("-lruntime")
    break
  fi
done

CXX=${CXX:-g++}
INCLUDE_FLAGS=()
for dir in "${INCLUDE_DIRS[@]}"; do
  INCLUDE_FLAGS+=("-I${dir}")
done

echo "ASCEND_ROOT=${ASCEND_ROOT}"
echo "INCLUDE_DIRS=${INCLUDE_DIRS[*]}"
echo "LIB_DIRS=${LIB_DIRS[*]}"
echo "CXX=${CXX}"

"${CXX}" -std=c++17 -O2 -Wall -Wextra \
  "${INCLUDE_FLAGS[@]}" \
  ffts_vs_acl_d2d_benchmark.cpp \
  "${LIB_FLAGS[@]}" "${LINK_LIBS[@]}" \
  -Wl,-rpath,"${RPATH}" \
  -o build/ffts_vs_acl_d2d_benchmark

echo "Built build/ffts_vs_acl_d2d_benchmark"

