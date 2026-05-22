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


CASE_SPECS = [
    ("h2d_ffts", "ascend_h2d_ffts_split", "H2D+FFTS"),
    ("ce", "host_to_device_ce", "CE"),
    ("ce_ms", "host_to_device_ce_multi_stream", "CE multi-stream"),
]

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
    "experiment",
    "x_value",
    "io_size",
    "io_size_bytes",
    "buffer_num",
    "iterations",
    "device_count",
    "repeat",
    "case_id",
    "copy_case",
    "case_label",
    "size_kb_reported",
    "count_reported",
    "submit_min_us",
    "submit_max_us",
    "submit_avg_us",
    "submit_p50_us",
    "submit_p90_us",
    "copy_min_us",
    "copy_max_us",
    "copy_avg_us",
    "copy_p50_us",
    "copy_p90_us",
    "bw_gbs",
    "log_file",
    "command",
]

AGG_FIELDS = [
    "experiment",
    "x_value",
    "io_size",
    "io_size_bytes",
    "buffer_num",
    "iterations",
    "device_count",
    "case_id",
    "copy_case",
    "case_label",
    "repeats",
    "submit_avg_us",
    "copy_avg_us",
    "bw_gbs",
    "speedup_vs_ce",
    "speedup_vs_ce_ms",
]


def repo_root() -> Path:
    return Path(__file__).resolve().parents[3]


def split_list(text: str) -> list[str]:
    return [item.strip() for item in re.split(r"[;,]", text) if item.strip()]


def parse_size_to_bytes(text: str) -> int:
    match = re.fullmatch(r"\s*(\d+)\s*([kKmMgG])\s*", text)
    if not match:
        raise ValueError(f"invalid size: {text!r}; use values like 2K, 37K, 1M")
    value = int(match.group(1))
    unit = match.group(2).upper()
    scale = {"K": 1024, "M": 1024**2, "G": 1024**3}[unit]
    return value * scale


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
        raise RuntimeError(
            "parsed more than one result row; run with --device-count 1 for this comparison"
        )
    return matches[0]


def run_one(args, out_dir: Path, experiment: str, x_value: str, io_size: str, buffer_num: int,
            repeat: int, case_id: str, copy_case: str, case_label: str) -> dict[str, str]:
    log_dir = out_dir / "logs"
    log_dir.mkdir(parents=True, exist_ok=True)
    log_file = log_dir / (
        f"{safe_name(experiment)}_{safe_name(x_value)}_{case_id}_r{repeat}.log"
    )

    cmd = [
        str(args.copy_bin),
        "-t",
        copy_case,
        "-s",
        io_size,
        "-n",
        str(buffer_num),
        "-i",
        str(args.iterations),
        "-d",
        str(args.device_count),
    ]
    print(f"[run] {experiment} x={x_value} case={case_label} repeat={repeat}: "
          f"{shlex.join(cmd)}")
    completed = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    log_file.write_text(completed.stdout, encoding="utf-8")
    if completed.returncode != 0:
        print(completed.stdout)
        raise RuntimeError(f"command failed with exit code {completed.returncode}: {log_file}")

    parsed = parse_copy_output(completed.stdout)
    return {
        "experiment": experiment,
        "x_value": x_value,
        "io_size": io_size,
        "io_size_bytes": str(parse_size_to_bytes(io_size)),
        "buffer_num": str(buffer_num),
        "iterations": str(args.iterations),
        "device_count": str(args.device_count),
        "repeat": str(repeat),
        "case_id": case_id,
        "copy_case": copy_case,
        "case_label": case_label,
        "size_kb_reported": parsed["size_kb"],
        "count_reported": parsed["count"],
        "submit_min_us": parsed["submit_min"],
        "submit_max_us": parsed["submit_max"],
        "submit_avg_us": parsed["submit_avg"],
        "submit_p50_us": parsed["submit_p50"],
        "submit_p90_us": parsed["submit_p90"],
        "copy_min_us": parsed["copy_min"],
        "copy_max_us": parsed["copy_max"],
        "copy_avg_us": parsed["copy_avg"],
        "copy_p50_us": parsed["copy_p50"],
        "copy_p90_us": parsed["copy_p90"],
        "bw_gbs": parsed["bw_gbs"],
        "log_file": str(log_file.relative_to(out_dir)),
        "command": shlex.join(cmd),
    }


def write_csv(path: Path, rows: list[dict[str, str]], fields: list[str]) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def mean(values: list[float]) -> float:
    return sum(values) / len(values) if values else math.nan


def aggregate_rows(rows: list[dict[str, str]]) -> list[dict[str, str]]:
    grouped = defaultdict(list)
    for row in rows:
        key = (
            row["experiment"],
            row["x_value"],
            row["io_size"],
            row["io_size_bytes"],
            row["buffer_num"],
            row["iterations"],
            row["device_count"],
            row["case_id"],
            row["copy_case"],
            row["case_label"],
        )
        grouped[key].append(row)

    aggregate = []
    for key, items in grouped.items():
        row = {
            "experiment": key[0],
            "x_value": key[1],
            "io_size": key[2],
            "io_size_bytes": key[3],
            "buffer_num": key[4],
            "iterations": key[5],
            "device_count": key[6],
            "case_id": key[7],
            "copy_case": key[8],
            "case_label": key[9],
            "repeats": str(len(items)),
            "submit_avg_us": f"{mean([float(item['submit_avg_us']) for item in items]):.3f}",
            "copy_avg_us": f"{mean([float(item['copy_avg_us']) for item in items]):.3f}",
            "bw_gbs": f"{mean([float(item['bw_gbs']) for item in items]):.6f}",
            "speedup_vs_ce": "",
            "speedup_vs_ce_ms": "",
        }
        aggregate.append(row)

    by_point = defaultdict(dict)
    for row in aggregate:
        point_key = (row["experiment"], row["x_value"])
        by_point[point_key][row["case_id"]] = row

    for cases in by_point.values():
        ce_bw = float(cases["ce"]["bw_gbs"]) if "ce" in cases else math.nan
        ce_ms_bw = float(cases["ce_ms"]["bw_gbs"]) if "ce_ms" in cases else math.nan
        for row in cases.values():
            bw = float(row["bw_gbs"])
            row["speedup_vs_ce"] = f"{bw / ce_bw:.6f}" if ce_bw > 0 else ""
            row["speedup_vs_ce_ms"] = f"{bw / ce_ms_bw:.6f}" if ce_ms_bw > 0 else ""

    def sort_key(row):
        if row["experiment"] == "size_sweep":
            x = int(row["io_size_bytes"])
        else:
            x = int(row["buffer_num"])
        case_order = {case_id: i for i, (case_id, _, _) in enumerate(CASE_SPECS)}
        return row["experiment"], x, case_order.get(row["case_id"], 99)

    return sorted(aggregate, key=sort_key)


def print_terminal_summary(aggregate: list[dict[str, str]]) -> None:
    print()
    print("[summary]")
    header = (
        f"{'experiment':<12} {'x':<8} {'case':<16} {'submit_us':>10} "
        f"{'copy_us':>10} {'bw_gbs':>10} {'vs_ce':>9} {'vs_ce_ms':>9}"
    )
    print(header)
    print("-" * len(header))
    for row in aggregate:
        print(
            f"{row['experiment']:<12} {row['x_value']:<8} {row['case_label']:<16} "
            f"{float(row['submit_avg_us']):>10.3f} {float(row['copy_avg_us']):>10.3f} "
            f"{float(row['bw_gbs']):>10.3f} {row['speedup_vs_ce']:>9} "
            f"{row['speedup_vs_ce_ms']:>9}"
        )


def plot_results(out_dir: Path, aggregate: list[dict[str, str]]) -> list[Path]:
    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("[warn] matplotlib is not installed; skip plots.")
        return []

    plot_dir = out_dir / "plots"
    plot_dir.mkdir(parents=True, exist_ok=True)
    plot_paths = []

    for experiment in ("size_sweep", "num_sweep"):
        rows = [row for row in aggregate if row["experiment"] == experiment]
        if not rows:
            continue
        by_case = defaultdict(list)
        for row in rows:
            by_case[row["case_id"]].append(row)

        for metric, ylabel, filename in (
            ("bw_gbs", "Bandwidth (GB/s)", f"{experiment}_bandwidth.png"),
            ("copy_avg_us", "Copy time avg (us)", f"{experiment}_copy_time.png"),
        ):
            plt.figure(figsize=(9, 5.2))
            for case_id, _, case_label in CASE_SPECS:
                case_rows = by_case.get(case_id, [])
                if not case_rows:
                    continue
                if experiment == "size_sweep":
                    case_rows = sorted(case_rows, key=lambda row: int(row["io_size_bytes"]))
                    x_values = [int(row["io_size_bytes"]) / 1024 for row in case_rows]
                    x_labels = [row["io_size"] for row in case_rows]
                    plt.xscale("log", base=2)
                    plt.xticks(x_values, x_labels)
                    xlabel = "IO size"
                else:
                    case_rows = sorted(case_rows, key=lambda row: int(row["buffer_num"]))
                    x_values = [int(row["buffer_num"]) for row in case_rows]
                    xlabel = "Buffer number"
                y_values = [float(row[metric]) for row in case_rows]
                plt.plot(x_values, y_values, marker="o", linewidth=2, label=case_label)

            plt.grid(True, linestyle="--", alpha=0.35)
            plt.xlabel(xlabel)
            plt.ylabel(ylabel)
            plt.title(f"H2D {experiment.replace('_', ' ')} {ylabel}")
            plt.legend()
            plt.tight_layout()
            path = plot_dir / filename
            plt.savefig(path, dpi=160)
            plt.close()
            plot_paths.append(path)

    return plot_paths


def write_analysis(out_dir: Path, aggregate: list[dict[str, str]], plot_paths: list[Path]) -> Path:
    by_point = defaultdict(list)
    for row in aggregate:
        by_point[(row["experiment"], row["x_value"])].append(row)

    lines = [
        "# H2D FFTS Experiment Analysis",
        "",
        "## Scope",
        "",
        "- Cases: H2D+FFTS, CE, CE multi-stream.",
        "- `copy_avg_us` and `bw_gbs` are taken from the `copy` benchmark output.",
        "- `speedup_vs_ce` and `speedup_vs_ce_ms` use bandwidth ratios.",
        "",
        "## Plots",
        "",
    ]
    if plot_paths:
        for path in plot_paths:
            lines.append(f"- `{path.relative_to(out_dir)}`")
    else:
        lines.append("- Plot generation was skipped because matplotlib was unavailable.")

    lines.extend(["", "## Point Summary", ""])
    for experiment in ("size_sweep", "num_sweep"):
        lines.append(f"### {experiment}")
        lines.append("")
        keys = sorted(
            [key for key in by_point if key[0] == experiment],
            key=lambda key: parse_size_to_bytes(key[1])
            if experiment == "size_sweep"
            else int(key[1]),
        )
        for key in keys:
            rows = sorted(by_point[key], key=lambda row: float(row["bw_gbs"]), reverse=True)
            best = rows[0]
            ce = next((row for row in rows if row["case_id"] == "ce"), None)
            ce_ms = next((row for row in rows if row["case_id"] == "ce_ms"), None)
            ffts = next((row for row in rows if row["case_id"] == "h2d_ffts"), None)
            lines.append(
                f"- `{key[1]}` best is `{best['case_label']}` at "
                f"{float(best['bw_gbs']):.3f} GB/s."
            )
            if ffts and ce:
                lines.append(
                    f"  `H2D+FFTS / CE = {float(ffts['speedup_vs_ce']):.3f}`."
                )
            if ffts and ce_ms:
                lines.append(
                    f"  `H2D+FFTS / CE multi-stream = "
                    f"{float(ffts['speedup_vs_ce_ms']):.3f}`."
                )
        lines.append("")

    lines.extend([
        "## Data Files",
        "",
        "- `raw_results.csv`",
        "- `summary.csv`",
        "- `logs/`",
    ])
    path = out_dir / "analysis.md"
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return path


def parse_args() -> argparse.Namespace:
    root = repo_root()
    parser = argparse.ArgumentParser(
        description="Run H2D FFTS vs CE vs CE multi-stream experiments and plot results."
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
        / "h2d_ffts_results"
        / dt.datetime.now().strftime("%Y%m%d-%H%M%S"),
        help="Directory for logs, CSV files, plots, and analysis.",
    )
    parser.add_argument("--iterations", type=int, default=128)
    parser.add_argument("--device-count", type=int, default=1)
    parser.add_argument("--repeats", type=int, default=1)
    parser.add_argument(
        "--size-sweep-sizes",
        default="2K,37K,64K,128K,256K,512K",
        help="Comma or semicolon separated IO sizes for experiment 1.",
    )
    parser.add_argument(
        "--size-sweep-buffer-num",
        type=int,
        default=1024,
        help="Buffer count for experiment 1.",
    )
    parser.add_argument("--num-sweep-size", default="37K")
    parser.add_argument(
        "--num-sweep-buffer-nums",
        default="10,50,100,300,500,1000",
        help="Comma or semicolon separated buffer counts for experiment 2.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    args.copy_bin = args.copy_bin.resolve()
    args.output_dir = args.output_dir.resolve()

    try:
        ensure_copy_binary(args.copy_bin)
    except (FileNotFoundError, PermissionError) as exc:
        print(f"[error] {exc}", file=sys.stderr)
        return 1

    args.output_dir.mkdir(parents=True, exist_ok=True)
    rows = []

    size_sweep_sizes = split_list(args.size_sweep_sizes)
    num_sweep_buffer_nums = [int(item) for item in split_list(args.num_sweep_buffer_nums)]

    print(f"[config] copy_bin={args.copy_bin}")
    print(f"[config] output_dir={args.output_dir}")
    print(
        f"[config] experiment1 sizes={size_sweep_sizes}, "
        f"buffer_num={args.size_sweep_buffer_num}, iterations={args.iterations}"
    )
    print(
        f"[config] experiment2 size={args.num_sweep_size}, "
        f"buffer_nums={num_sweep_buffer_nums}, iterations={args.iterations}"
    )

    for io_size in size_sweep_sizes:
        for repeat in range(1, args.repeats + 1):
            for case_id, copy_case, case_label in CASE_SPECS:
                rows.append(
                    run_one(
                        args,
                        args.output_dir,
                        "size_sweep",
                        io_size,
                        io_size,
                        args.size_sweep_buffer_num,
                        repeat,
                        case_id,
                        copy_case,
                        case_label,
                    )
                )

    for buffer_num in num_sweep_buffer_nums:
        for repeat in range(1, args.repeats + 1):
            for case_id, copy_case, case_label in CASE_SPECS:
                rows.append(
                    run_one(
                        args,
                        args.output_dir,
                        "num_sweep",
                        str(buffer_num),
                        args.num_sweep_size,
                        buffer_num,
                        repeat,
                        case_id,
                        copy_case,
                        case_label,
                    )
                )

    raw_csv = args.output_dir / "raw_results.csv"
    write_csv(raw_csv, rows, RAW_FIELDS)
    aggregate = aggregate_rows(rows)
    summary_csv = args.output_dir / "summary.csv"
    write_csv(summary_csv, aggregate, AGG_FIELDS)
    plot_paths = plot_results(args.output_dir, aggregate)
    analysis_path = write_analysis(args.output_dir, aggregate, plot_paths)
    print_terminal_summary(aggregate)

    print()
    print(f"[done] raw csv: {raw_csv}")
    print(f"[done] summary csv: {summary_csv}")
    print(f"[done] analysis: {analysis_path}")
    if plot_paths:
        for path in plot_paths:
            print(f"[done] plot: {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
