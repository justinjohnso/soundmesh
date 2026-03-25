#!/usr/bin/env python3
import argparse
import json
from pathlib import Path

SLOS = {
    "join_time_s": ("<=", 20.0),
    "rejoin_time_s": ("<=", 30.0),
    "stream_continuity_pct": (">=", 99.0),
    "underruns_per_min": ("<=", 1.0),
    "decode_failures_per_min": ("<=", 0.5),
    "loss_pct": ("<=", 2.0),
}


def passed(value, op, threshold):
    if value is None:
        return False
    if op == "<=":
        return value <= threshold
    if op == ">=":
        return value >= threshold
    raise ValueError(op)


def main() -> None:
    parser = argparse.ArgumentParser(description="Render benchmark markdown report")
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    data = json.loads(Path(args.input).read_text())
    lines = [
        "# Release Candidate Benchmark Report",
        "",
        f"Duration: {data.get('duration_seconds', 0)} seconds",
        "",
        "| Metric | Value | SLO | Result |",
        "| --- | ---: | --- | --- |",
    ]

    all_pass = True
    for metric, (op, threshold) in SLOS.items():
        value = data.get(metric)
        ok = passed(value, op, threshold)
        all_pass = all_pass and ok
        lines.append(f"| {metric} | {value} | {op} {threshold} | {'PASS' if ok else 'FAIL'} |")

    lines.append("")
    lines.append(f"Overall: {'PASS' if all_pass else 'FAIL'}")
    lines.append("")
    lines.append("## Raw counters")
    lines.append("")
    raw = data.get("raw", {})
    for key in sorted(raw.keys()):
        lines.append(f"- {key}: {raw[key]}")

    Path(args.output).write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
