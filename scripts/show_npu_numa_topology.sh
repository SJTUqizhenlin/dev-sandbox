#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: show_npu_numa_topology.sh [--visible] [NPU_ID]

Show Ascend NPU IDs and map them to PCI Bus-Id, NUMA node, and local CPU list.

By default this script shows the actual driver/npu-smi view. This is the ID
space used by direct ACL code when aclrtGetDeviceCount still reports all cards.

With --visible, ASCEND_VISIBLE_DEVICES is interpreted as an intended visible
device list: visible ID 0 maps to the first physical ID in that list, visible ID
1 maps to the second, and so on.

When NPU_ID is provided, only that device is shown in the selected mode.
EOF
}

visible_mode=0
if [[ "${1:-}" == "--visible" ]]; then
    visible_mode=1
    shift
fi

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

target_npu="${1:-}"
if [[ -n "${target_npu}" && ! "${target_npu}" =~ ^[0-9]+$ ]]; then
    echo "error: NPU_ID must be an integer" >&2
    usage >&2
    exit 1
fi

if ! command -v npu-smi >/dev/null 2>&1; then
    echo "error: npu-smi not found in PATH" >&2
    exit 1
fi

declare -A bus_by_physical_npu=()
current_npu=""
while IFS= read -r line; do
    if [[ "${line}" =~ ^\|[[:space:]]*([0-9]+)[[:space:]]+[^[:space:]\|]+[[:space:]]*\| ]]; then
        current_npu="${BASH_REMATCH[1]}"
    fi
    if [[ "${line}" =~ ([0-9A-Fa-f]{4}:[0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}\.[0-9]) ]]; then
        if [[ -n "${current_npu}" ]]; then
            bus_by_physical_npu["${current_npu}"]="${BASH_REMATCH[1]}"
            current_npu=""
        fi
    fi
done < <(npu-smi info)

if [[ "${#bus_by_physical_npu[@]}" -eq 0 ]]; then
    echo "error: failed to parse NPU Bus-Id from npu-smi info" >&2
    exit 1
fi

id_physical_pairs=()
if [[ "${visible_mode}" -eq 1 ]]; then
    if [[ -z "${ASCEND_VISIBLE_DEVICES:-}" ]]; then
        echo "error: --visible requires ASCEND_VISIBLE_DEVICES to be set" >&2
        exit 1
    fi
    IFS=',' read -r -a visible_devices <<<"${ASCEND_VISIBLE_DEVICES}"
    for visible_id in "${!visible_devices[@]}"; do
        physical_id="${visible_devices[${visible_id}]}"
        physical_id="${physical_id//[[:space:]]/}"
        if [[ -n "${physical_id}" ]]; then
            id_physical_pairs+=("${visible_id} ${physical_id}")
        fi
    done
else
    mapfile -t sorted_physical_ids < <(printf '%s\n' "${!bus_by_physical_npu[@]}" | sort -n)
    for physical_id in "${sorted_physical_ids[@]}"; do
        id_physical_pairs+=("${physical_id} ${physical_id}")
    done
fi

dev_count="$(find /dev -maxdepth 1 -type c -name 'davinci[0-9]*' 2>/dev/null | wc -l)"
printf 'ASCEND_VISIBLE_DEVICES=%s\n' "${ASCEND_VISIBLE_DEVICES:-<unset>}"
printf '/dev/davinci device count=%s\n' "${dev_count}"
if [[ "${visible_mode}" -eq 1 ]]; then
    printf 'mode=ASCEND_VISIBLE_DEVICES intended view\n'
    printf '%-8s %-8s %-14s %-9s %-14s %s\n' "Visible" "Physical" "Bus-Id" "NUMA" "CPU-List" "numactl prefix"
    printf '%-8s %-8s %-14s %-9s %-14s %s\n' "-------" "--------" "------" "----" "--------" "--------------"
else
    printf 'mode=actual npu-smi/driver view\n'
    if [[ -n "${ASCEND_VISIBLE_DEVICES:-}" ]]; then
        printf 'note: ASCEND_VISIBLE_DEVICES is set; use --visible to print that intended mapping.\n'
    fi
    printf '%-8s %-8s %-14s %-9s %-14s %s\n' "CodeID" "Physical" "Bus-Id" "NUMA" "CPU-List" "numactl prefix"
    printf '%-8s %-8s %-14s %-9s %-14s %s\n' "------" "--------" "------" "----" "--------" "--------------"
fi

found=0
for pair in "${id_physical_pairs[@]}"; do
    read -r shown_id physical_id <<<"${pair}"
    if [[ -n "${target_npu}" && "${shown_id}" != "${target_npu}" ]]; then
        continue
    fi

    found=1
    bus_id="${bus_by_physical_npu[${physical_id}]:-}"
    if [[ -z "${bus_id}" ]]; then
        printf '%-8s %-8s %-14s %-9s %-14s %s\n' "${shown_id}" "${physical_id}" "?" "?" "?" "# physical NPU not found in npu-smi"
        continue
    fi

    pci_path="/sys/bus/pci/devices/${bus_id,,}"
    if [[ ! -d "${pci_path}" ]]; then
        printf '%-8s %-8s %-14s %-9s %-14s %s\n' "${shown_id}" "${physical_id}" "${bus_id}" "?" "?" "# PCI sysfs path not found"
        continue
    fi

    numa_node="$(<"${pci_path}/numa_node")"
    cpu_list="$(<"${pci_path}/local_cpulist")"
    if [[ "${numa_node}" == "-1" ]]; then
        prefix="# NUMA node unknown; try taskset -c ${cpu_list}"
    else
        prefix="numactl --cpunodebind=${numa_node} --membind=${numa_node}"
    fi

    printf '%-8s %-8s %-14s %-9s %-14s %s\n' "${shown_id}" "${physical_id}" "${bus_id}" "${numa_node}" "${cpu_list}" "${prefix}"
done

if [[ "${found}" -eq 0 ]]; then
    echo "error: NPU ${target_npu} was not found" >&2
    exit 1
fi