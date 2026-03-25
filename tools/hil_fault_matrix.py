#!/usr/bin/env python3
"""Run repeatable HIL soak + fault scenarios for SRC/OUT."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path


def run_case(
    script_path: Path,
    src_port: str,
    out_port: str,
    baud: int,
    duration: int,
    ignore_reset_window: float,
    schedule_path: Path | None,
    output_dir: Path,
    case_name: str,
) -> tuple[int, Path, bool]:
    output_dir.mkdir(parents=True, exist_ok=True)
    summary_path = output_dir / f"{case_name}-summary.json"

    cmd = [
        sys.executable,
        str(script_path),
        "--src-port",
        src_port,
        "--out-port",
        out_port,
        "--baud",
        str(baud),
        "--duration",
        str(duration),
        "--ignore-reset-window",
        str(ignore_reset_window),
        "--summary-json",
        str(summary_path),
    ]
    if schedule_path:
        cmd += ["--fault-schedule", str(schedule_path)]

    print(f"[fault-matrix] Running case '{case_name}'")
    print(f"[fault-matrix] Command: {' '.join(cmd)}")
    ret = subprocess.run(cmd, check=False)
    expected_faults = schedule_path is not None
    return ret.returncode, summary_path, expected_faults


def main() -> int:
    parser = argparse.ArgumentParser(description="Run HIL fault-injection matrix")
    parser.add_argument("--src-port", required=True)
    parser.add_argument("--out-port", required=True)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--duration", type=int, default=180)
    parser.add_argument("--ignore-reset-window", type=float, default=8.0)
    parser.add_argument("--schedule", default=None, help="Optional fault schedule JSON")
    parser.add_argument(
        "--output-dir",
        default="docs/operations/runtime-evidence/fault-matrix",
        help="Directory for per-case JSON summaries",
    )
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parents[1]
    soak_script = repo_root / "tools" / "hil_soak_check.py"
    output_dir = repo_root / args.output_dir

    cases: list[tuple[str, Path | None]] = [("baseline", None)]
    if args.schedule:
        schedule_path = Path(args.schedule)
        if not schedule_path.is_absolute():
            schedule_path = repo_root / schedule_path
        if not schedule_path.exists():
            print(f"[fault-matrix] Missing schedule file: {schedule_path}")
            return 2
        cases.append((schedule_path.stem, schedule_path))

    matrix_summary: dict[str, dict] = {}
    overall_ok = True
    for case_name, schedule_path in cases:
        rc, summary_path, expected_faults = run_case(
            script_path=soak_script,
            src_port=args.src_port,
            out_port=args.out_port,
            baud=args.baud,
            duration=args.duration,
            ignore_reset_window=args.ignore_reset_window,
            schedule_path=schedule_path,
            output_dir=output_dir,
            case_name=case_name,
        )
        if not summary_path.exists():
            matrix_summary[case_name] = {
                "exit_code": rc,
                "result": "FAIL",
                "error": "missing summary output",
            }
            overall_ok = False
            continue

        summary = json.loads(summary_path.read_text())
        summary["exit_code"] = rc
        if expected_faults:
            src_faults = int(summary.get("nodes", {}).get("SRC", {}).get("fired_faults", 0))
            out_faults = int(summary.get("nodes", {}).get("OUT", {}).get("fired_faults", 0))
            if src_faults == 0 and out_faults == 0:
                summary["result"] = "FAIL"
                summary["error"] = "scheduled faults not observed"
                rc = 2
                summary["exit_code"] = rc
        matrix_summary[case_name] = summary
        if rc != 0 or summary.get("result") != "PASS":
            overall_ok = False

    matrix_path = output_dir / "fault-matrix-summary.json"
    matrix_path.write_text(json.dumps(matrix_summary, indent=2) + "\n")
    print(f"[fault-matrix] Summary: {matrix_path}")
    print(f"[fault-matrix] RESULT={'PASS' if overall_ok else 'FAIL'}")
    return 0 if overall_ok else 2


if __name__ == "__main__":
    raise SystemExit(main())
