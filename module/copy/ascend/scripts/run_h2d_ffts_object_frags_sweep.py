#!/usr/bin/env python3
import argparse
import csv
import datetime as dt
import math
import os
import re
import shlex
import subprocess
import sys
from collections import defaultdict
from pathlib import Path
from typing import Optional


SPLIT_CASE = ("split", "ascend_h2d_ffts_split", "H2D+FFTS")
PIPELINE_CASE = ("pipeline", "ascend_h2d_ffts_yuanrong_pipeline", "H2D+YPipe")

RESULT_RE = re.compile(
    r"^\s*(?P<prefix>.+?)\s+"
    r"(?P<size_kb>\d+(?:\.\d+)?)\s+"
    r"(?P<count>\d+)\s+"
    r"(?P<submit_min>\d+)\s*/\s*(?P<submit_max>\d+)\s*/\s*"
    r"(?P<submit_avg>\d+)\s*/\s*(?P<submit_p50>\d+)\s*/\s*(?P<submit_p90>\d+)\s+"
    r"(?P<copy_min>\d+)\s*/\s*(?P<copy_max>\d+)\s*/\s*"
    r"(?P<copy_avg>\d+)\s*/\s*(?P<copy_p50>\d+)\s*/\s*(?P<copy_p90>\d+)\s+"
    r"(?P<bw_gbs>\d+(?:\.\d+)?)\s*$"
)

RAW_FIELDS = [
    "case_id",
    "copy_case",
    "case_label",
    "object_frags",
    "io_size",
    "io_size_bytes",
    "buffer_num",
    "iterations",
    "device_count",
    "ffts_lanes",
    "repeat",
    "submit_avg_us",
    "copy_avg_us",
    "copy_p50_us",
    "copy_p90_us",
    "bw_gbs",
    "speedup_vs_split",
    "log_file",
    "command",
]

SUMMARY_FIELDS = [
    "case_id",
    "copy_case",
    "case_label",
    "object_frags",
    "io_size",
    "io_size_bytes",
    "buffer_num",
    "iterations",
    "device_count",
    "ffts_lanes",
    "repeats",
    "submit_avg_us",
    "copy_avg_us",
    "copy_p50_us",
    "copy_p90_us",
    "bw_gbs",
    "speedup_vs_split",
]


def repo_root() -> Path:
    return Path(__file__).resolve().parents[4]


def split_list(text: str) -> list[str]:
    return [item.strip() for item in re.split(r"[;,]", text) if item.strip()]


def parse_size_to_bytes(text: str) -> int:
    match = re.fullmatch(r"\s*(\d+)\s*([kKmMgG])\s*", text)
    if not match:
        raise ValueError(f"invalid size: {text!r}; use values like 2K, 37K, 1M")
    value = int(match.group(1))
    unit = match.group(2).upper()
    return value * {"K": 1024, "M": 1024**2, "G": 1024**3}[unit]


def parse_object_frags(text: str) -> list[int]:
    values = []
    for item in split_list(text):
        try:
            values.append(int(item))
        except ValueError as exc:
            raise ValueError(f"invalid object_frags value: {item!r}") from exc
    if not values:
        raise ValueError("--object-frags must contain at least one value")
    if any(value <= 0 for value in values):
        raise ValueError("--object-frags values must be positive")
    return values


def validate_positive_int(name: str, value: int) -> None:
    if value <= 0:
        raise ValueError(f"--{name} must be positive")


def safe_name(text: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", text).strip("_")


def ensure_copy_binary(path: Path) -> None:
    if not path.exists():
        raise FileNotFoundError(f"copy binary not found: {path}")
    if not os.access(path, os.X_OK):
        raise PermissionError(f"copy binary is not executable: {path}")


def parse_copy_output(output: str) -> dict[str, str]:
    matches = []
    for line in output.splitlines():
        match = RESULT_RE.match(line)
        if match:
            matches.append(match.groupdict())
    if not matches:
        raise RuntimeError("failed to parse copy result row from command output")
    if len(matches) > 1:
        raise RuntimeError("parsed more than one result row; use --device-count 1")
    return matches[0]


def mean(values: list[float]) -> float:
    return sum(values) / len(values) if values else math.nan


def run_one(args, out_dir: Path, case_spec: tuple[str, str, str], repeat: int,
            object_frags: Optional[int]) -> dict[str, str]:
    case_id, copy_case, case_label = case_spec
    log_dir = out_dir / "logs"
    log_dir.mkdir(parents=True, exist_ok=True)
    object_frags_name = str(object_frags) if object_frags is not None else "baseline"
    log_file = log_dir / (
        f"{safe_name(copy_case)}_obj{object_frags_name}_r{repeat}.log"
    )

    env = os.environ.copy()
    env["COPY_FFTS_VALIDATE"] = "0"
    extra_env = {"COPY_FFTS_VALIDATE": "0"}
    if object_frags is not None:
        env["COPY_FFTS_PIPELINE_OBJECT_FRAGS"] = str(object_frags)
        extra_env["COPY_FFTS_PIPELINE_OBJECT_FRAGS"] = str(object_frags)
    if args.ffts_lanes:
        env["FFTS_MAX_READY_LANES"] = str(args.ffts_lanes)
        extra_env["FFTS_MAX_READY_LANES"] = str(args.ffts_lanes)

    cmd = [
        str(args.copy_bin),
        "-t",
        copy_case,
        "-s",
        args.io_size,
        "-n",
        str(args.buffer_num),
        "-i",
        str(args.iterations),
        "-d",
        str(args.device_count),
    ]
    command_text = " ".join(f"{key}={value}" for key, value in extra_env.items())
    command_text = f"{command_text} {shlex.join(cmd)}"
    print(f"[run] case={case_label} object_frags={object_frags_name} repeat={repeat}: {command_text}")

    completed = subprocess.run(
        cmd,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    log_file.write_text(completed.stdout, encoding="utf-8")
    if completed.returncode != 0:
        print(completed.stdout)
        raise RuntimeError(f"command failed with exit code {completed.returncode}: {log_file}")

    parsed = parse_copy_output(completed.stdout)
    return {
        "case_id": case_id,
        "copy_case": copy_case,
        "case_label": case_label,
        "object_frags": object_frags_name,
        "io_size": args.io_size,
        "io_size_bytes": str(parse_size_to_bytes(args.io_size)),
        "buffer_num": str(args.buffer_num),
        "iterations": str(args.iterations),
        "device_count": str(args.device_count),
        "ffts_lanes": str(args.ffts_lanes) if args.ffts_lanes else "",
        "repeat": str(repeat),
        "submit_avg_us": parsed["submit_avg"],
        "copy_avg_us": parsed["copy_avg"],
        "copy_p50_us": parsed["copy_p50"],
        "copy_p90_us": parsed["copy_p90"],
        "bw_gbs": parsed["bw_gbs"],
        "speedup_vs_split": "",
        "log_file": str(log_file.relative_to(out_dir)),
        "command": command_text,
    }


def aggregate_rows(rows: list[dict[str, str]]) -> list[dict[str, str]]:
    grouped = defaultdict(list)
    for row in rows:
        key = (
            row["case_id"],
            row["copy_case"],
            row["case_label"],
            row["object_frags"],
            row["io_size"],
            row["io_size_bytes"],
            row["buffer_num"],
            row["iterations"],
            row["device_count"],
            row["ffts_lanes"],
        )
        grouped[key].append(row)

    summary = []
    for key, items in grouped.items():
        summary.append({
            "case_id": key[0],
            "copy_case": key[1],
            "case_label": key[2],
            "object_frags": key[3],
            "io_size": key[4],
            "io_size_bytes": key[5],
            "buffer_num": key[6],
            "iterations": key[7],
            "device_count": key[8],
            "ffts_lanes": key[9],
            "repeats": str(len(items)),
            "submit_avg_us": f"{mean([float(item['submit_avg_us']) for item in items]):.3f}",
            "copy_avg_us": f"{mean([float(item['copy_avg_us']) for item in items]):.3f}",
            "copy_p50_us": f"{mean([float(item['copy_p50_us']) for item in items]):.3f}",
            "copy_p90_us": f"{mean([float(item['copy_p90_us']) for item in items]):.3f}",
            "bw_gbs": f"{mean([float(item['bw_gbs']) for item in items]):.6f}",
            "speedup_vs_split": "",
        })

    split_rows = [row for row in summary if row["case_id"] == "split"]
    split_bw = float(split_rows[0]["bw_gbs"]) if split_rows else math.nan
    if split_bw > 0:
        for row in summary:
            row["speedup_vs_split"] = f"{float(row['bw_gbs']) / split_bw:.6f}"

    def sort_key(row: dict[str, str]) -> tuple[int, int]:
        if row["case_id"] == "split":
            return (0, 0)
        return (1, int(row["object_frags"]))

    return sorted(summary, key=sort_key)


def write_csv(path: Path, rows: list[dict[str, str]], fields: list[str]) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def fill_raw_speedups(rows: list[dict[str, str]], summary: list[dict[str, str]]) -> None:
    speedups = {
        (row["case_id"], row["object_frags"]): row["speedup_vs_split"]
        for row in summary
    }
    for row in rows:
        row["speedup_vs_split"] = speedups.get((row["case_id"], row["object_frags"]), "")


def print_summary(summary: list[dict[str, str]]) -> None:
    print()
    print("[summary]")
    header = (
        f"{'case':<10} {'obj':>8} {'submit_us':>10} {'copy_us':>10} "
        f"{'p50_us':>10} {'p90_us':>10} {'bw_gbs':>10} {'vs_split':>10}"
    )
    print(header)
    print("-" * len(header))
    for row in summary:
        print(
            f"{row['case_label']:<10} {row['object_frags']:>8} "
            f"{float(row['submit_avg_us']):>10.3f} {float(row['copy_avg_us']):>10.3f} "
            f"{float(row['copy_p50_us']):>10.3f} {float(row['copy_p90_us']):>10.3f} "
            f"{float(row['bw_gbs']):>10.3f} {row['speedup_vs_split']:>10}"
        )

    pipeline_rows = [row for row in summary if row["case_id"] == "pipeline"]
    if pipeline_rows:
        best = max(pipeline_rows, key=lambda row: float(row["bw_gbs"]))
        print()
        print(
            f"[best] object_frags={best['object_frags']} "
            f"bw={float(best['bw_gbs']):.3f} GB/s "
            f"copy_avg={float(best['copy_avg_us']):.3f} us "
            f"speedup_vs_split={best['speedup_vs_split']}"
        )


def write_analysis(path: Path, summary: list[dict[str, str]]) -> None:
    pipeline_rows = [row for row in summary if row["case_id"] == "pipeline"]
    split = next((row for row in summary if row["case_id"] == "split"), None)
    lines = [
        "# H2D FFTS Object Frags Sweep",
        "",
        "## Scope",
        "",
        "- Fixed IO size and buffer count.",
        "- Baseline is `ascend_h2d_ffts_split`.",
        "- Swept case is `ascend_h2d_ffts_yuanrong_pipeline`.",
        "",
        "## Result",
        "",
    ]
    if split:
        lines.append(
            f"- Baseline bandwidth: `{float(split['bw_gbs']):.3f} GB/s`, "
            f"copy avg: `{float(split['copy_avg_us']):.3f} us`."
        )
    if pipeline_rows:
        best = max(pipeline_rows, key=lambda row: float(row["bw_gbs"]))
        lines.append(
            f"- Best object fragments: `{best['object_frags']}`, "
            f"bandwidth: `{float(best['bw_gbs']):.3f} GB/s`, "
            f"speedup vs split: `{best['speedup_vs_split']}`."
        )
    lines.extend([
        "",
        "## Data Files",
        "",
        "- `raw_results.csv`",
        "- `summary.csv`",
        "- `logs/`",
        "",
    ])
    path.write_text("\n".join(lines), encoding="utf-8")


def parse_args() -> argparse.Namespace:
    root = repo_root()
    parser = argparse.ArgumentParser(
        description="Sweep COPY_FFTS_PIPELINE_OBJECT_FRAGS for one H2D FFTS benchmark point."
    )
    parser.add_argument(
        "--copy-bin",
        type=Path,
        default=root / "build" / "module" / "copy" / "copy",
        help="Path to the module/copy benchmark binary.",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=root
        / "module"
        / "copy"
        / "ascend"
        / "h2d_ffts_object_frags_sweep"
        / dt.datetime.now().strftime("%Y%m%d-%H%M%S"),
        help="Directory for logs and CSV files.",
    )
    parser.add_argument("--io-size", "--size", default="37K", help="IO size, such as 8K, 37K, 1M.")
    parser.add_argument("--buffer-num", "-n", type=int, default=1024)
    parser.add_argument("--iterations", "-i", type=int, default=128)
    parser.add_argument("--device-count", "-d", type=int, default=1)
    parser.add_argument("--repeats", "-r", type=int, default=1)
    parser.add_argument(
        "--object-frags",
        default="1,2,4,8,16,32,64,128",
        help="Comma or semicolon separated COPY_FFTS_PIPELINE_OBJECT_FRAGS values.",
    )
    parser.add_argument(
        "--ffts-lanes",
        default="",
        help="Optional fixed FFTS_MAX_READY_LANES value for all FFTS cases.",
    )
    parser.add_argument(
        "--skip-baseline",
        action="store_true",
        help="Only run the yuanrong-style pipeline sweep.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    args.copy_bin = args.copy_bin.resolve()
    args.output_dir = args.output_dir.resolve()

    try:
        io_size_bytes = parse_size_to_bytes(args.io_size)
        object_frags_values = parse_object_frags(args.object_frags)
        validate_positive_int("buffer-num", args.buffer_num)
        validate_positive_int("iterations", args.iterations)
        validate_positive_int("device-count", args.device_count)
        validate_positive_int("repeats", args.repeats)
    except ValueError as exc:
        print(f"[error] {exc}", file=sys.stderr)
        return 1

    try:
        ensure_copy_binary(args.copy_bin)
    except (FileNotFoundError, PermissionError) as exc:
        print(f"[error] {exc}", file=sys.stderr)
        return 1

    args.output_dir.mkdir(parents=True, exist_ok=True)
    print(f"[config] copy_bin={args.copy_bin}")
    print(f"[config] output_dir={args.output_dir}")
    print(f"[config] io_size={args.io_size}")
    print(f"[config] io_size_bytes={io_size_bytes}")
    print(f"[config] buffer_num={args.buffer_num}")
    print(f"[config] iterations={args.iterations}")
    print(f"[config] device_count={args.device_count}")
    print(f"[config] repeats={args.repeats}")
    print(f"[config] object_frags={object_frags_values}")
    print(f"[config] ffts_lanes={args.ffts_lanes or 'default'}")

    rows = []
    for repeat in range(1, args.repeats + 1):
        if not args.skip_baseline:
            rows.append(run_one(args, args.output_dir, SPLIT_CASE, repeat, None))
        for object_frags in object_frags_values:
            rows.append(run_one(args, args.output_dir, PIPELINE_CASE, repeat, object_frags))

    summary = aggregate_rows(rows)
    fill_raw_speedups(rows, summary)

    raw_csv = args.output_dir / "raw_results.csv"
    summary_csv = args.output_dir / "summary.csv"
    analysis_path = args.output_dir / "analysis.md"
    write_csv(raw_csv, rows, RAW_FIELDS)
    write_csv(summary_csv, summary, SUMMARY_FIELDS)
    write_analysis(analysis_path, summary)
    print_summary(summary)

    print()
    print(f"[done] raw csv: {raw_csv}")
    print(f"[done] summary csv: {summary_csv}")
    print(f"[done] analysis: {analysis_path}")
    print(f"[done] logs: {args.output_dir / 'logs'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
