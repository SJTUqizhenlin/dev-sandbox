#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build}"
COPY_BIN="${COPY_BIN:-${BUILD_DIR}/module/copy/copy}"

SIZE="${SIZE:-512M}"
COUNT="${COUNT:-8}"
ITERS="${ITERS:-128}"
DEVICES="${DEVICES:-8}"
BIND_NUMA="${BIND_NUMA:-0}"
HUGE_SHM_MODE="${HUGE_SHM_MODE:-parallel}"

# Actual driver/NPU ID mapping from scripts/show_npu_numa_topology.sh:
# device 0 -> NUMA 6, CPUs 144-167
# device 1 -> NUMA 6, CPUs 144-167
# device 2 -> NUMA 4, CPUs 96-119
# device 3 -> NUMA 4, CPUs 96-119
# device 4 -> NUMA 0, CPUs 0-23
# device 5 -> NUMA 0, CPUs 0-23
# device 6 -> NUMA 2, CPUs 48-71
# device 7 -> NUMA 2, CPUs 48-71
export COPY_HUGE_SHM_DEVICE_CPUS="${COPY_HUGE_SHM_DEVICE_CPUS:-144-167;144-167;96-119;96-119;0-23;0-23;48-71;48-71}"
COPY_HUGE_SHM_DEVICE_NUMA="${COPY_HUGE_SHM_DEVICE_NUMA:-6,6,4,4,0,0,2,2}"

parse_size_bytes() {
    local value="$1"
    local number="${value%[KkMm]}"
    local unit="${value:${#value}-1:1}"
    case "${unit}" in
        K|k) echo $((number * 1024)) ;;
        M|m) echo $((number * 1024 * 1024)) ;;
        *)
            echo "[error] SIZE must use K or M suffix, got: ${value}" >&2
            exit 1
            ;;
    esac
}

precheck_numa_hugepages() {
    local size_bytes pages_per_device required_pages node free_file free_pages
    size_bytes="$(parse_size_bytes "${SIZE}")"
    pages_per_device="$(((size_bytes * COUNT + 2 * 1024 * 1024 - 1) / (2 * 1024 * 1024)))"
    IFS=',' read -r -a numa_nodes <<<"${COPY_HUGE_SHM_DEVICE_NUMA}"

    declare -A required_by_node=()
    for ((device = 0; device < DEVICES; device++)); do
        node="${numa_nodes[device]:-}"
        if [[ -z "${node}" ]]; then
            echo "[error] missing NUMA node for device ${device}" >&2
            exit 1
        fi
        required_by_node["${node}"]="$(( ${required_by_node[${node}]:-0} + pages_per_device ))"
    done

    for node in "${!required_by_node[@]}"; do
        required_pages="${required_by_node[${node}]}"
        free_file="/sys/devices/system/node/node${node}/hugepages/hugepages-2048kB/free_hugepages"
        if [[ ! -r "${free_file}" ]]; then
            echo "[warn] cannot read ${free_file}; skip NUMA hugepage precheck" >&2
            continue
        fi
        free_pages="$(<"${free_file}")"
        if ((free_pages < required_pages)); then
            echo "[error] NUMA node ${node} has ${free_pages} free 2MB hugepages, but this run needs ${required_pages}." >&2
            echo "[error] Lower SIZE/COUNT/DEVICES, reserve more local hugepages, or run with BIND_NUMA=0." >&2
            exit 1
        fi
    done
}

if [[ "${BIND_NUMA}" == "1" ]]; then
    precheck_numa_hugepages
    export COPY_HUGE_SHM_DEVICE_NUMA
else
    unset COPY_HUGE_SHM_DEVICE_NUMA
fi

if [[ ! -x "${COPY_BIN}" ]]; then
    echo "[error] executable not found: ${COPY_BIN}" >&2
    echo "[error] run scripts/build.sh first, or set COPY_BIN." >&2
    exit 1
fi

echo "[run] copy bin: ${COPY_BIN}"
echo "[run] case: huge_shm_to_device_ce"
echo "[run] size: ${SIZE}"
echo "[run] count: ${COUNT}"
echo "[run] iterations: ${ITERS}"
echo "[run] devices: ${DEVICES}"
echo "[run] huge shm mode: ${HUGE_SHM_MODE}"
echo "[run] bind numa memory: ${BIND_NUMA}"
echo "[run] COPY_HUGE_SHM_DEVICE_CPUS=${COPY_HUGE_SHM_DEVICE_CPUS}"
if [[ "${BIND_NUMA}" == "1" ]]; then
    echo "[run] COPY_HUGE_SHM_DEVICE_NUMA=${COPY_HUGE_SHM_DEVICE_NUMA}"
else
    echo "[run] COPY_HUGE_SHM_DEVICE_NUMA=<unset>"
fi

exec "${COPY_BIN}" -t huge_shm_to_device_ce --huge-shm-mode "${HUGE_SHM_MODE}" \
    -s "${SIZE}" -n "${COUNT}" -i "${ITERS}" -d "${DEVICES}" "$@"
