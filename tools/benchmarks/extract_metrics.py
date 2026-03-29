#!/usr/bin/env python3
import argparse
import json
import math
import re
from pathlib import Path

ANSI_RE = re.compile(r"\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~]|\][^\x1B\x07]*(?:\x07|\x1B\\))")
LOG_PREFIX_RE = re.compile(r"\b[IWE]\s+\(\s*(\d+)\)\s+")
JOIN_RE = re.compile(
    r"Network ready|Designated root ready|Root ready:|State:\s*Mesh Joining\s*->\s*Streaming|"
    r"Parent connected,\s*layer|stream ready|MESH_EVENT_PARENT_CONNECTED|MESH_EVENT_ROOT_FIXED",
    re.IGNORECASE,
)
UNDER_RE = re.compile(r"underrun", re.IGNORECASE)
DECODE_RE = re.compile(r"Opus decode failed|decode failed", re.IGNORECASE)
LOSS_RE = re.compile(r"RX:\s+\d+\s+pkts,\s+\d+\s+drops\s+\(([0-9]+(?:\.[0-9]+)?)%\)")
REJOIN_RE = re.compile(r"triggering mesh rejoin|rejoin", re.IGNORECASE)
REASON201_RE = re.compile(r"reason:201", re.IGNORECASE)
BUF0_RE = re.compile(r"\bbuf\s*=\s*0\s*%|\bBuf:\s*0\s*%", re.IGNORECASE)
TX_OBS_RE = re.compile(
    r"TX OBS:\s*sent=(\d+)\s+fail=(\d+)\s+qfull=(\d+)\s+bp=(\d+)",
    re.IGNORECASE,
)
RX_OBS_RE = re.compile(
    r"RX OBS:\s*gap=(\d+)/(\d+)\s+late=(\d+)\s+hard=(\d+)\s+fec=(\d+)\s+plc=(\d+)/(\d+)\s+ovf=(\d+)\s+dec=(\d+)\s+und=(\d+)\s+rebuf=(\d+)\s+miss_pk=(\d+)\s+prefill=(\d+)\s+wait_ms=(\d+)\s+buf_peak=(\d+)%",
    re.IGNORECASE,
)
RX_NET_RE = re.compile(
    r"RX NET:\s*rx=(\d+)\s+dup=(\d+)\s+ttl0=(\d+)\s+inv=(\d+)/(\d+)/(\d+)\s+batch=(\d+)/(\d+)\s+cb_miss=(\d+)\s+recv=(\d+)/(\d+)\s+ctrl=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)\s+churn=(\d+)/(\d+)/(\d+)/(\d+)\s+rj=(\d+)/(\d+)/(\d+)",
    re.IGNORECASE,
)


def read_text(path: Path) -> str:
    return path.read_text(errors="ignore") if path.exists() else ""


def strip_ansi(text: str) -> str:
    return ANSI_RE.sub("", text)


def percentile(values: list[float], q: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    rank = int(math.ceil(q * len(ordered))) - 1
    rank = max(0, min(rank, len(ordered) - 1))
    return ordered[rank]


def parse_log_ms(line: str) -> int | None:
    m = LOG_PREFIX_RE.search(strip_ansi(line))
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
    src_text_clean = strip_ansi(src_text)
    out_text_clean = strip_ansi(out_text)
    src_lines = src_text_clean.splitlines()
    out_lines = out_text_clean.splitlines()
    src_join_events = sum(1 for line in src_lines if JOIN_RE.search(line))
    out_join_events = sum(1 for line in out_lines if JOIN_RE.search(line))
    join_time_s = None
    src_first_join_ms = first_match_time_ms(src_text_clean, JOIN_RE)
    out_first_join_ms = first_match_time_ms(out_text_clean, JOIN_RE)
    if src_first_join_ms is not None and out_first_join_ms is not None:
        join_time_s = round(max(src_first_join_ms, out_first_join_ms) / 1000.0, 3)
    elif out_first_join_ms is not None:
        join_time_s = round(out_first_join_ms / 1000.0, 3)
    elif src_first_join_ms is not None:
        join_time_s = round(src_first_join_ms / 1000.0, 3)

    underruns = len(UNDER_RE.findall(out_text_clean))
    decode_failures = len(DECODE_RE.findall(out_text_clean))
    rejoin_events = len(REJOIN_RE.findall(out_text_clean))
    rejoin_time_s = first_rejoin_recovery_time_s(out_text_clean)
    reason201_count = len(REASON201_RE.findall(src_text_clean))
    buf0_events = len(BUF0_RE.findall(out_text_clean))

    tx_obs_samples = [TX_OBS_RE.search(line) for line in out_lines]
    tx_obs_samples = [sample for sample in tx_obs_samples if sample is not None]
    tx_obs_send_failures = max((int(sample.group(2)) for sample in tx_obs_samples), default=0)
    tx_obs_queue_full = max((int(sample.group(3)) for sample in tx_obs_samples), default=0)
    tx_obs_backpressure_level_max = max((int(sample.group(4)) for sample in tx_obs_samples), default=0)

    rx_obs_samples = [RX_OBS_RE.search(line) for line in out_lines]
    rx_obs_samples = [sample for sample in rx_obs_samples if sample is not None]
    rx_obs_gap_events = max((int(sample.group(1)) for sample in rx_obs_samples), default=0)
    rx_obs_gap_frames = max((int(sample.group(2)) for sample in rx_obs_samples), default=0)
    rx_obs_decode_errors = max((int(sample.group(9)) for sample in rx_obs_samples), default=0)
    rx_obs_underrun_rebuffers = max((int(sample.group(11)) for sample in rx_obs_samples), default=0)
    rx_obs_prefill_wait_ms = max((int(sample.group(14)) for sample in rx_obs_samples), default=0)
    rx_obs_buffer_peak_pct = max((int(sample.group(15)) for sample in rx_obs_samples), default=0)

    rx_net_samples = [RX_NET_RE.search(line) for line in out_lines]
    rx_net_samples = [sample for sample in rx_net_samples if sample is not None]
    rx_net_duplicates = max((int(sample.group(2)) for sample in rx_net_samples), default=0)
    rx_net_ttl_expired = max((int(sample.group(3)) for sample in rx_net_samples), default=0)
    rx_net_mesh_recv_errors = max((int(sample.group(10)) for sample in rx_net_samples), default=0)
    rx_net_mesh_recv_empty = max((int(sample.group(11)) for sample in rx_net_samples), default=0)

    losses = [float(m.group(1)) for m in LOSS_RE.finditer(out_text_clean)]
    loss_pct = max(losses) if losses else 0.0
    loss_avg_pct = sum(losses) / len(losses) if losses else 0.0
    loss_last_pct = losses[-1] if losses else 0.0
    loss_p95_pct = percentile(losses, 0.95) if losses else 0.0

    minutes = max(duration_s / 60.0, 1e-9)
    underruns_per_min = underruns / minutes
    decode_failures_per_min = decode_failures / minutes
    reason201_per_min = reason201_count / minutes
    buf0_events_per_min = buf0_events / minutes

    stream_continuity_pct = 100.0
    if rejoin_events > 0:
        stream_continuity_pct = max(0.0, 100.0 - (rejoin_events * 1.0))

    total_lines = len(src_lines) + len(out_lines)
    join_time_source = "none"
    if src_first_join_ms is not None and out_first_join_ms is not None:
        join_time_source = "src_out_max"
    elif out_first_join_ms is not None:
        join_time_source = "out_only"
    elif src_first_join_ms is not None:
        join_time_source = "src_only"

    return {
        "duration_seconds": duration_s,
        "join_time_s": join_time_s,
        "rejoin_time_s": 0.0 if rejoin_events == 0 else rejoin_time_s,
        "stream_continuity_pct": round(stream_continuity_pct, 2),
        "underruns_per_min": round(underruns_per_min, 3),
        "decode_failures_per_min": round(decode_failures_per_min, 3),
        "loss_pct": round(loss_pct, 3),
        "loss_avg_pct": round(loss_avg_pct, 3),
        "loss_last_pct": round(loss_last_pct, 3),
        "loss_p95_pct": round(loss_p95_pct, 3),
        "reason201_per_min": round(reason201_per_min, 3),
        "buf0_events_per_min": round(buf0_events_per_min, 3),
        "raw": {
            "src_join_events": src_join_events,
            "out_join_events": out_join_events,
            "src_first_join_ms": src_first_join_ms,
            "out_first_join_ms": out_first_join_ms,
            "join_time_source": join_time_source,
            "rejoin_events": rejoin_events,
            "underruns": underruns,
            "decode_failures": decode_failures,
            "reason201_count": reason201_count,
            "buf0_events": buf0_events,
            "line_count": total_lines,
            "src_line_count": len(src_lines),
            "out_line_count": len(out_lines),
            "loss_samples": losses,
            "event_counts": {
                "join_total": src_join_events + out_join_events,
                "rejoin_events": rejoin_events,
                "underruns": underruns,
                "decode_failures": decode_failures,
                "reason201_count": reason201_count,
                "buf0_events": buf0_events,
            },
            "tx_obs_send_failures": tx_obs_send_failures,
            "tx_obs_queue_full": tx_obs_queue_full,
            "tx_obs_backpressure_level_max": tx_obs_backpressure_level_max,
            "rx_obs_gap_events": rx_obs_gap_events,
            "rx_obs_gap_frames": rx_obs_gap_frames,
            "rx_obs_decode_errors": rx_obs_decode_errors,
            "rx_obs_underrun_rebuffers": rx_obs_underrun_rebuffers,
            "rx_obs_prefill_wait_ms": rx_obs_prefill_wait_ms,
            "rx_obs_buffer_peak_pct": rx_obs_buffer_peak_pct,
            "rx_net_duplicates": rx_net_duplicates,
            "rx_net_ttl_expired": rx_net_ttl_expired,
            "rx_net_mesh_recv_errors": rx_net_mesh_recv_errors,
            "rx_net_mesh_recv_empty": rx_net_mesh_recv_empty,
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
