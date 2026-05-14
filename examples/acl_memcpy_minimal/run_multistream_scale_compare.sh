#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${BIN:-${SCRIPT_DIR}/h2d_d2h_async_memcpy}"

IO_SIZE="${IO_SIZE:-32K}"
BUFFER_COUNT="${BUFFER_COUNT:-10000}"
STREAMS="${STREAMS:-4}"
ITERS="${ITERS:-10}"
RUN_SINGLE_DIRECT="${RUN_SINGLE_DIRECT:-1}"
DEVICE_GROUPS="${DEVICE_GROUPS:-0;1;2;3;4;5;6;7;0,1;0,2;0,3;0,4;0,5;0,6;0,7;1,2;2,3;3,4;4,5;5,6;6,7;0,1,2,3;4,5,6,7;0,2,4,6;1,3,5,7;0,1,2,3,4,5,6,7}"
LOG_DIR="${LOG_DIR:-${SCRIPT_DIR}/logs/multistream-scale-$(date +%Y%m%d-%H%M%S)}"

if [[ ! -x "${BIN}" ]]; then
    echo "[error] executable not found: ${BIN}" >&2
    echo "[error] build h2d_d2h_async_memcpy first, or set BIN=/path/to/binary." >&2
    exit 1
fi

mkdir -p "${LOG_DIR}"

ROWS=()
LAST_BW="0"

trim() {
    local text="$1"
    text="${text#"${text%%[![:space:]]*}"}"
    text="${text%"${text##*[![:space:]]}"}"
    printf "%s" "${text}"
}

device_count() {
    local devices="${1// /}"
    awk -F',' '{ print NF }' <<< "${devices}"
}

safe_label() {
    local text="$1"
    text="${text// /}"
    text="${text//,/_}"
    printf "%s" "${text}"
}

extract_metrics() {
    local log_file="$1"
    awk '
        /^H2D/ {
            row=$1
            count=$4
            submit=$5
            copy=$7
            bw=$10
        }
        END {
            if (row == "") {
                exit 1
            }
            printf "%s\t%s\t%s\t%s\n", count, submit, copy, bw
        }
    ' "${log_file}"
}

record_result() {
    local label="$1"
    local mode="$2"
    local devices="$3"
    local ndevices="$4"
    local log_file="$5"
    local ref_bw="$6"

    local count submit_us copy_us bw
    IFS=$'\t' read -r count submit_us copy_us bw < <(extract_metrics "${log_file}")

    local scale="NA"
    local efficiency="NA"
    local effective_ref="${ref_bw}"
    if [[ "${effective_ref}" == "0" && "${mode}" == "all8_process" && "${devices}" == "0" ]]; then
        effective_ref="${bw}"
    fi
    if [[ "${effective_ref}" != "0" ]]; then
        scale="$(awk -v bw="${bw}" -v ref="${effective_ref}" 'BEGIN { printf "%.3f", bw / ref }')"
        efficiency="$(awk -v bw="${bw}" -v ref="${effective_ref}" -v n="${ndevices}" 'BEGIN { printf "%.3f", bw / (ref * n) }')"
    fi

    ROWS+=("${label}|${mode}|${devices}|${ndevices}|${count}|${submit_us}|${copy_us}|${bw}|${scale}|${efficiency}|$(basename "${log_file}")")
    LAST_BW="${bw}"
}

run_case() {
    local label="$1"
    local mode="$2"
    local devices="$3"
    local ndevices="$4"
    local ref_bw="$5"
    shift 5

    local log_file="${LOG_DIR}/${label}.log"
    local cmd=("$@")

    echo
    echo "[run] ${label}: ${cmd[*]}"
    "${cmd[@]}" 2>&1 | tee "${log_file}"
    record_result "${label}" "${mode}" "${devices}" "${ndevices}" "${log_file}" "${ref_bw}"
}

print_table() {
    echo
    echo "[result] multistream scaling summary"
    printf "%-22s %-14s %-18s %4s %8s %12s %12s %12s %8s %10s %s\n" \
        "case" "mode" "devices" "N" "count" "submit_us" "copy_us" "bw_MBps" "scale" "eff" "log"
    printf "%-22s %-14s %-18s %4s %8s %12s %12s %12s %8s %10s %s\n" \
        "----------------------" "--------------" "------------------" "----" "--------" "------------" "------------" "------------" "--------" "----------" "---"
    for row in "${ROWS[@]}"; do
        IFS='|' read -r label mode devices ndevices count submit_us copy_us bw scale efficiency log_name <<< "${row}"
        printf "%-22s %-14s %-18s %4s %8s %12s %12s %12s %8s %10s %s\n" \
            "${label}" "${mode}" "${devices}" "${ndevices}" "${count}" "${submit_us}" "${copy_us}" "${bw}" "${scale}" "${efficiency}" "${log_name}"
    done
}

echo "[compare] bin=${BIN}"
echo "[compare] io_size=${IO_SIZE}, io_num=${BUFFER_COUNT}, streams=${STREAMS}, iterations=${ITERS}"
echo "[compare] groups=${DEVICE_GROUPS}"
echo "[compare] logs=${LOG_DIR}"

REF_BW="0"

if [[ "${RUN_SINGLE_DIRECT}" == "1" ]]; then
    run_case "single_direct_ms${STREAMS}_d0" "multi_stream" "0" "1" "${REF_BW}" \
        "${BIN}" -t multi_stream -s "${IO_SIZE}" -n "${BUFFER_COUNT}" -i "${ITERS}" -m "${STREAMS}"
fi

run_case "proc_ms${STREAMS}_d0" "all8_process" "0" "1" "${REF_BW}" \
    "${BIN}" -t all8_process -s "${IO_SIZE}" -n "${BUFFER_COUNT}" -i "${ITERS}" -m "${STREAMS}" -d "0"
REF_BW="${LAST_BW}"

IFS=';' read -r -a groups <<< "${DEVICE_GROUPS}"
for raw_group in "${groups[@]}"; do
    group="$(trim "${raw_group}")"
    [[ -z "${group}" ]] && continue
    [[ "${group}" == "0" ]] && continue

    clean_group="${group// /}"
    ndevices="$(device_count "${clean_group}")"
    label="proc_ms${STREAMS}_d$(safe_label "${clean_group}")"
    run_case "${label}" "all8_process" "${clean_group}" "${ndevices}" "${REF_BW}" \
        "${BIN}" -t all8_process -s "${IO_SIZE}" -n "${BUFFER_COUNT}" -i "${ITERS}" -m "${STREAMS}" -d "${clean_group}"
done

print_table

echo
echo "[done] logs saved in ${LOG_DIR}"
echo "[hint] eff close to 1.0 means near-linear scaling; lower values show where scaling is lost."
