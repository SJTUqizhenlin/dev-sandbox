#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${BIN:-${SCRIPT_DIR}/h2d_d2h_async_memcpy}"

IO_SIZES="${IO_SIZES:-64K;37K;2K}"
BUFFER_COUNT="${BUFFER_COUNT:-10000}"
STREAMS="${STREAMS:-4}"
ITERS="${ITERS:-10}"
REPEATS="${REPEATS:-3}"
SINGLE_DEVICE="${SINGLE_DEVICE:-0}"
ALL_DEVICES="${ALL_DEVICES:-0,1,2,3,4,5,6,7}"
LOG_DIR="${LOG_DIR:-${SCRIPT_DIR}/logs/io-multistream-$(date +%Y%m%d-%H%M%S)}"

if [[ ! -x "${BIN}" ]]; then
    echo "[error] executable not found: ${BIN}" >&2
    echo "[error] build h2d_d2h_async_memcpy first, or set BIN=/path/to/binary." >&2
    exit 1
fi

mkdir -p "${LOG_DIR}"

ROWS=()

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

run_repeated() {
    local io_size="$1"
    local label="$2"
    local devices="$3"
    local ndevices="$4"
    local ref_bw="$5"

    local sum_submit="0"
    local sum_copy="0"
    local sum_bw="0"
    local count="0"
    local log_prefix="${LOG_DIR}/${io_size}_${label}"

    for repeat in $(seq 1 "${REPEATS}"); do
        local log_file="${log_prefix}_r${repeat}.log"
        local cmd=("${BIN}" -t all8_process -s "${io_size}" -n "${BUFFER_COUNT}" -i "${ITERS}" -m "${STREAMS}" -d "${devices}")

        echo
        echo "[run] io=${io_size} case=${label} repeat=${repeat}/${REPEATS}: ${cmd[*]}"
        "${cmd[@]}" 2>&1 | tee "${log_file}"

        local submit_us copy_us bw
        IFS=$'\t' read -r count submit_us copy_us bw < <(extract_metrics "${log_file}")
        sum_submit="$(awk -v a="${sum_submit}" -v b="${submit_us}" 'BEGIN { printf "%.6f", a + b }')"
        sum_copy="$(awk -v a="${sum_copy}" -v b="${copy_us}" 'BEGIN { printf "%.6f", a + b }')"
        sum_bw="$(awk -v a="${sum_bw}" -v b="${bw}" 'BEGIN { printf "%.6f", a + b }')"
    done

    local avg_submit avg_copy avg_bw scale efficiency
    avg_submit="$(awk -v sum="${sum_submit}" -v n="${REPEATS}" 'BEGIN { printf "%.3f", sum / n }')"
    avg_copy="$(awk -v sum="${sum_copy}" -v n="${REPEATS}" 'BEGIN { printf "%.3f", sum / n }')"
    avg_bw="$(awk -v sum="${sum_bw}" -v n="${REPEATS}" 'BEGIN { printf "%.2f", sum / n }')"
    scale="NA"
    efficiency="NA"
    if [[ "${ref_bw}" != "0" ]]; then
        scale="$(awk -v bw="${avg_bw}" -v ref="${ref_bw}" 'BEGIN { printf "%.3f", bw / ref }')"
        efficiency="$(awk -v bw="${avg_bw}" -v ref="${ref_bw}" -v n="${ndevices}" 'BEGIN { printf "%.3f", bw / (ref * n) }')"
    else
        scale="1.000"
        efficiency="1.000"
    fi

    ROWS+=("${io_size}|${label}|${devices}|${ndevices}|${count}|${REPEATS}|${avg_submit}|${avg_copy}|${avg_bw}|${scale}|${efficiency}|$(basename "${log_prefix}")_r*.log")
    LAST_AVG_BW="${avg_bw}"
}

print_table() {
    echo
    echo "[result] 1-device vs 8-device multi-stream H2D average"
    printf "%-6s %-12s %-18s %4s %8s %7s %12s %12s %12s %8s %10s %s\n" \
        "io" "case" "devices" "N" "count" "runs" "submit_us" "copy_us" "bw_MBps" "scale" "eff" "logs"
    printf "%-6s %-12s %-18s %4s %8s %7s %12s %12s %12s %8s %10s %s\n" \
        "------" "------------" "------------------" "----" "--------" "-------" "------------" "------------" "------------" "--------" "----------" "----"
    for row in "${ROWS[@]}"; do
        IFS='|' read -r io_size label devices ndevices count repeats submit_us copy_us bw scale efficiency logs <<< "${row}"
        printf "%-6s %-12s %-18s %4s %8s %7s %12s %12s %12s %8s %10s %s\n" \
            "${io_size}" "${label}" "${devices}" "${ndevices}" "${count}" "${repeats}" "${submit_us}" "${copy_us}" "${bw}" "${scale}" "${efficiency}" "${logs}"
    done
}

echo "[compare] bin=${BIN}"
echo "[compare] io_sizes=${IO_SIZES}, io_num=${BUFFER_COUNT}, streams=${STREAMS}, iterations=${ITERS}, repeats=${REPEATS}"
echo "[compare] single_device=${SINGLE_DEVICE}, all_devices=${ALL_DEVICES}"
echo "[compare] logs=${LOG_DIR}"

IFS=';' read -r -a io_sizes <<< "${IO_SIZES}"
for io_size in "${io_sizes[@]}"; do
    [[ -z "${io_size}" ]] && continue

    LAST_AVG_BW="0"
    run_repeated "${io_size}" "single" "${SINGLE_DEVICE}" "1" "0"
    single_bw="${LAST_AVG_BW}"

    all_count="$(device_count "${ALL_DEVICES}")"
    run_repeated "${io_size}" "all${all_count}" "${ALL_DEVICES}" "${all_count}" "${single_bw}"
done

print_table

echo
echo "[done] logs saved in ${LOG_DIR}"
echo "[hint] scale is all-device bandwidth divided by single-device bandwidth; eff is scale divided by device count."
