#!/usr/bin/env python3
import argparse
import datetime as dt
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path
from typing import Optional


DEFAULT_CASES = [
    "ascend_h2d_ffts_split",
    "ascend_h2d_ffts_yuanrong_pipeline",
    "ascend_ffts_merge_d2h",
    "ascend_d2d_split_ffts",
    "ascend_d2d_merge_ffts",
]

PIPELINE_CASE = "ascend_h2d_ffts_yuanrong_pipeline"


def repo_root() -> Path:
    return Path(__file__).resolve().parents[4]


def split_list(text: str) -> list[str]:
    return [item.strip() for item in re.split(r"[;,]", text) if item.strip()]


def safe_name(text: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", text).strip("_")


def ensure_copy_binary(path: Path) -> None:
    if not path.exists():
        raise FileNotFoundError(f"copy binary not found: {path}")
    if not os.access(path, os.X_OK):
        raise PermissionError(f"copy binary is not executable: {path}")


def validation_env(lane: Optional[str], object_frags: Optional[str]) -> dict[str, str]:
    env = os.environ.copy()
    env["COPY_FFTS_VALIDATE"] = "1"
    if lane:
        env["FFTS_MAX_READY_LANES"] = lane
    if object_frags:
        env["COPY_FFTS_PIPELINE_OBJECT_FRAGS"] = object_frags
    return env


def command_text(cmd: list[str], lane: Optional[str], object_frags: Optional[str]) -> str:
    prefix = ["COPY_FFTS_VALIDATE=1"]
    if lane:
        prefix.append(f"FFTS_MAX_READY_LANES={lane}")
    if object_frags:
        prefix.append(f"COPY_FFTS_PIPELINE_OBJECT_FRAGS={object_frags}")
    return " ".join(prefix + [shlex.join(cmd)])


def run_one(args, case_name: str, io_size: str, buffer_num: int, lane: Optional[str],
            object_frags: Optional[str]) -> dict[str, str]:
    log_dir = args.output_dir / "logs"
    log_dir.mkdir(parents=True, exist_ok=True)
    lane_name = lane if lane else "default"
    object_frags_name = object_frags if object_frags else "default"
    log_file = log_dir / (
        f"{safe_name(case_name)}_{safe_name(io_size)}_n{buffer_num}_"
        f"lane{lane_name}_obj{object_frags_name}.log"
    )

    cmd = [
        str(args.copy_bin),
        "-t",
        case_name,
        "-s",
        io_size,
        "-n",
        str(buffer_num),
        "-i",
        str(args.iterations),
        "-d",
        str(args.device_count),
    ]
    text = command_text(cmd, lane, object_frags)
    print(f"[validate] {text}")
    completed = subprocess.run(
        cmd,
        env=validation_env(lane, object_frags),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    log_file.write_text(completed.stdout, encoding="utf-8")

    return {
        "case": case_name,
        "size": io_size,
        "buffers": str(buffer_num),
        "lane": lane_name,
        "object_frags": object_frags_name,
        "result": "PASS" if completed.returncode == 0 else "FAIL",
        "log": str(log_file.relative_to(args.output_dir)),
        "command": text,
    }


def print_summary(rows: list[dict[str, str]]) -> None:
    print()
    header = (
        f"{'case':<40} {'size':<8} {'buffers':>8} {'lane':>8} "
        f"{'obj':>8} {'result':>8} log"
    )
    print(header)
    print("-" * len(header))
    for row in rows:
        print(
            f"{row['case']:<40} {row['size']:<8} {row['buffers']:>8} "
            f"{row['lane']:>8} {row['object_frags']:>8} {row['result']:>8} {row['log']}"
        )


def parse_args() -> argparse.Namespace:
    root = repo_root()
    parser = argparse.ArgumentParser(
        description="Run correctness validation for Ascend FFTS copy cases."
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
        / "h2d_ffts_validation"
        / dt.datetime.now().strftime("%Y%m%d-%H%M%S"),
        help="Directory for validation logs.",
    )
    parser.add_argument(
        "--cases",
        default=",".join(DEFAULT_CASES),
        help="Comma or semicolon separated copy cases to validate.",
    )
    parser.add_argument(
        "--sizes",
        default="2K,8K,37K,64K",
        help="Comma or semicolon separated IO sizes.",
    )
    parser.add_argument(
        "--buffer-nums",
        default="1,10,64,1024",
        help="Comma or semicolon separated buffer counts.",
    )
    parser.add_argument(
        "--ffts-lanes",
        default="",
        help="Optional comma or semicolon separated FFTS lane values.",
    )
    parser.add_argument(
        "--pipeline-object-frags",
        default="8",
        help="Object fragment counts for the yuanrong-style H2D pipeline case.",
    )
    parser.add_argument("--iterations", type=int, default=1)
    parser.add_argument("--device-count", type=int, default=1)
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
    cases = split_list(args.cases)
    sizes = split_list(args.sizes)
    buffer_nums = [int(item) for item in split_list(args.buffer_nums)]
    lanes = split_list(args.ffts_lanes) or [None]
    pipeline_object_frags = split_list(args.pipeline_object_frags) or ["8"]

    print(f"[config] copy_bin={args.copy_bin}")
    print(f"[config] output_dir={args.output_dir}")
    print("[config] validation=enabled")
    print(f"[config] cases={cases}")
    print(f"[config] sizes={sizes}")
    print(f"[config] buffer_nums={buffer_nums}")
    print(f"[config] ffts_lanes={[lane if lane else 'default' for lane in lanes]}")
    print(f"[config] pipeline_object_frags={pipeline_object_frags}")

    rows = []
    for case_name in cases:
        object_frags_values = pipeline_object_frags if case_name == PIPELINE_CASE else [None]
        for io_size in sizes:
            for buffer_num in buffer_nums:
                for lane in lanes:
                    for object_frags in object_frags_values:
                        rows.append(run_one(args, case_name, io_size, buffer_num, lane,
                                            object_frags))

    print_summary(rows)
    failed = [row for row in rows if row["result"] != "PASS"]
    print()
    print(f"[done] logs: {args.output_dir / 'logs'}")
    if failed:
        print(f"[done] validation failed: {len(failed)} case(s)", file=sys.stderr)
        return 1
    print("[done] validation passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
