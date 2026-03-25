#!/usr/bin/env python3
import argparse
import json
import re
from pathlib import Path

LOG_PREFIX_RE = re.compile(r"^[IWE]\s+\(\s*(\d+)\)\s+")
JOIN_RE = re.compile(r"Network ready")
UNDER_RE = re.compile(r"underrun", re.IGNORECASE)
DECODE_RE = re.compile(r"Opus decode failed|decode failed", re.IGNORECASE)
LOSS_RE = re.compile(r"RX:\s+\d+\s+pkts,\s+\d+\s+drops\s+\(([0-9]+(?:\.[0-9]+)?)%\)")
REJOIN_RE = re.compile(r"triggering mesh rejoin|rejoin", re.IGNORECASE)


def read_text(path: Path) -> str:
    return path.read_text(errors="ignore") if path.exists() else ""


def parse_log_ms(line: str) -> int | None:
    m = LOG_PREFIX_RE.match(line)
    if not m:
        return None
    try:
        return int(m.group(1))
    except ValueError:
        return None


def first_match_time_ms(text: str, pattern: re.Pattern[str]) -> int | None:
    for line in text.splitlines():
        if not pattern.search(line):
            continue
        ms = parse_log_ms(line)
        if ms is not None:
            return ms
    return None


def first_rejoin_recovery_time_s(out_text: str) -> float | None:
    lines = out_text.splitlines()
    trigger_idx = None
    trigger_ms = None

    for i, line in enumerate(lines):
        if REJOIN_RE.search(line):
            ms = parse_log_ms(line)
            if ms is not None:
                trigger_idx = i
                trigger_ms = ms
                break
    if trigger_idx is None or trigger_ms is None:
        return None

    for line in lines[trigger_idx + 1:]:
        if JOIN_RE.search(line):
            join_ms = parse_log_ms(line)
            if join_ms is not None and join_ms >= trigger_ms:
                return round((join_ms - trigger_ms) / 1000.0, 3)
    return None


def compute_metrics(src_text: str, out_text: str, duration_s: int) -> dict:
    src_lines = src_text.splitlines()
    out_lines = out_text.splitlines()
    src_join_events = sum(1 for line in src_lines if JOIN_RE.search(line))
    out_join_events = sum(1 for line in out_lines if JOIN_RE.search(line))
    join_time_s = None
    src_first_join_ms = first_match_time_ms(src_text, JOIN_RE)
    out_first_join_ms = first_match_time_ms(out_text, JOIN_RE)
    if src_first_join_ms is not None and out_first_join_ms is not None:
        join_time_s = round(max(src_first_join_ms, out_first_join_ms) / 1000.0, 3)

    underruns = len(UNDER_RE.findall(out_text))
    decode_failures = len(DECODE_RE.findall(out_text))
    rejoin_events = len(REJOIN_RE.findall(out_text))
    rejoin_time_s = first_rejoin_recovery_time_s(out_text)

    losses = [float(m.group(1)) for m in LOSS_RE.finditer(out_text)]
    loss_pct = max(losses) if losses else 0.0

    minutes = max(duration_s / 60.0, 1e-9)
    underruns_per_min = underruns / minutes
    decode_failures_per_min = decode_failures / minutes

    stream_continuity_pct = 100.0
    if rejoin_events > 0:
        stream_continuity_pct = max(0.0, 100.0 - (rejoin_events * 1.0))

    return {
        "duration_seconds": duration_s,
        "join_time_s": join_time_s,
        "rejoin_time_s": 0.0 if rejoin_events == 0 else rejoin_time_s,
        "stream_continuity_pct": round(stream_continuity_pct, 2),
        "underruns_per_min": round(underruns_per_min, 3),
        "decode_failures_per_min": round(decode_failures_per_min, 3),
        "loss_pct": round(loss_pct, 3),
        "raw": {
            "src_join_events": src_join_events,
            "out_join_events": out_join_events,
            "src_first_join_ms": src_first_join_ms,
            "out_first_join_ms": out_first_join_ms,
            "rejoin_events": rejoin_events,
            "underruns": underruns,
            "decode_failures": decode_failures,
            "loss_samples": losses,
        },
    }


def main() -> None:
    parser = argparse.ArgumentParser(description="Extract benchmark metrics from SRC/OUT logs")
    parser.add_argument("--src-log", required=True)
    parser.add_argument("--out-log", required=True)
    parser.add_argument("--duration-seconds", type=int, required=True)
    parser.add_argument("--output", required=False)
    args = parser.parse_args()

    src_text = read_text(Path(args.src_log))
    out_text = read_text(Path(args.out_log))
    metrics = compute_metrics(src_text, out_text, args.duration_seconds)

    payload = json.dumps(metrics, indent=2)
    if args.output:
        Path(args.output).write_text(payload + "\n")
    else:
        print(payload)


if __name__ == "__main__":
    main()
