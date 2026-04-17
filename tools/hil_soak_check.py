#!/usr/bin/env python3
"""Hardware-in-the-loop soak checker for SRC+OUT stability.

This tool validates that both nodes stay alive during a timed soak and fail-fast
on panic/reboot-loop signatures. It intentionally ignores the initial USB reset
line emitted when serial is opened.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

try:
    import serial  # type: ignore
except Exception as exc:  # pragma: no cover
    raise SystemExit(f"pyserial is required for HIL soak checks: {exc}")


PANIC_PATTERN = re.compile(
    r"Guru Meditation|abort\(|stack overflow|Stack smashing|CORRUPT HEAP|"
    r"ESP_ERR_NO_MEM|panic|Backtrace:",
    re.IGNORECASE,
)
RESET_PATTERN = re.compile(r"^rst:", re.IGNORECASE)
SRC_OK_PATTERN = re.compile(r"src_main|combo_main|tx_main|TX:|Mesh TX GROUP|In:AUX", re.IGNORECASE)
OUT_OK_PATTERN = re.compile(r"out_main|rx_main|RX:|Playback|Audio frame RX|Audio:RX|OUT packet", re.IGNORECASE)
HEAP_PATTERN = re.compile(r"heap_kb:(\d+)", re.IGNORECASE)


@dataclass
class PortResult:
    open_error: str | None = None
    lines_read: int = 0
    ok_hits: int = 0
    panic_hits: list[tuple[float, str]] = field(default_factory=list)
    late_reset_hits: list[tuple[float, str]] = field(default_factory=list)
    heap_kb_min: int | None = None
    planned_faults: int = 0
    fault_fired: list[str] = field(default_factory=list)
    tail: list[str] = field(default_factory=list)


def load_fault_schedule(path: str | None) -> list[dict]:
    if not path:
        return []
    payload = json.loads(Path(path).read_text())
    if not isinstance(payload, list):
        raise SystemExit("fault schedule must be a JSON array")

    normalized = []
    for idx, raw in enumerate(payload):
        if not isinstance(raw, dict):
            raise SystemExit(f"fault schedule entry #{idx} must be an object")
        at_seconds = raw.get("at_seconds")
        action = raw.get("action")
        target = raw.get("target")
        if not isinstance(at_seconds, (int, float)) or at_seconds < 0:
            raise SystemExit(f"fault schedule entry #{idx} invalid at_seconds")
        if action not in ("serial_reset", "log_marker"):
            raise SystemExit(f"fault schedule entry #{idx} invalid action '{action}'")
        if target not in ("SRC", "OUT", "BOTH"):
            raise SystemExit(f"fault schedule entry #{idx} invalid target '{target}'")
        label = raw.get("label", "")
        if not isinstance(label, str):
            raise SystemExit(f"fault schedule entry #{idx} label must be string")
        normalized.append(
            {
                "at_seconds": float(at_seconds),
                "action": action,
                "target": target,
                "label": label,
                "fired": False,
            }
        )
    normalized.sort(key=lambda item: item["at_seconds"])
    return normalized


def node_fault_schedule(base_schedule: list[dict], node_name: str) -> list[dict]:
    per_node: list[dict] = []
    for step in base_schedule:
        if step.get("target") not in (node_name, "BOTH"):
            continue
        per_node.append(
            {
                "at_seconds": float(step["at_seconds"]),
                "action": step["action"],
                "target": step["target"],
                "label": step.get("label", ""),
                "fired": False,
            }
        )
    return per_node


def maybe_reset_port(ser: serial.Serial) -> None:
    try:
        ser.setDTR(False)
        ser.setRTS(True)
        time.sleep(0.12)
        ser.setRTS(False)
        ser.setDTR(True)
    except Exception:
        # Not all USB serial bridges expose the same control semantics.
        pass


def read_port(
    node_name: str,
    port: str,
    baud_rate: int,
    duration_s: int,
    ignore_reset_window_s: float,
    tail_lines: int,
    result: PortResult,
    fault_schedule: list[dict] | None = None,
    skip_reset_toggle: bool = False,
) -> None:
    try:
        ser = serial.Serial(port, baudrate=baud_rate, timeout=0.25)
    except Exception as exc:
        result.open_error = str(exc)
        return

    if not skip_reset_toggle:
        maybe_reset_port(ser)
    started_at = time.time()

    ok_pattern = SRC_OK_PATTERN if node_name == "SRC" else OUT_OK_PATTERN

    schedule = fault_schedule or []
    result.planned_faults = len(schedule)

    while time.time() - started_at < duration_s:
        elapsed = time.time() - started_at
        for step in schedule:
            if step.get("fired"):
                continue
            if elapsed < float(step["at_seconds"]):
                continue
            step["fired"] = True
            action = step["action"]
            label = step["label"] or action
            result.fault_fired.append(f"{action}:{label}@{elapsed:0.2f}s")
            if action == "serial_reset":
                if skip_reset_toggle:
                    result.tail.append(
                        f"[fault] skipped serial_reset '{label}' at {elapsed:0.2f}s ({node_name})"
                    )
                else:
                    maybe_reset_port(ser)
                    result.tail.append(f"[fault] fired {label} at {elapsed:0.2f}s ({node_name})")
            elif action == "log_marker":
                result.tail.append(f"[fault] marker {label} at {elapsed:0.2f}s ({node_name})")
            if len(result.tail) > tail_lines:
                result.tail = result.tail[-tail_lines:]

        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", errors="replace").strip()
        if not line:
            continue

        elapsed = time.time() - started_at
        result.lines_read += 1

        if ok_pattern.search(line):
            result.ok_hits += 1
        if PANIC_PATTERN.search(line):
            result.panic_hits.append((elapsed, line))
        if RESET_PATTERN.search(line) and elapsed > ignore_reset_window_s:
            result.late_reset_hits.append((elapsed, line))

        heap_match = HEAP_PATTERN.search(line)
        if heap_match:
            heap_kb = int(heap_match.group(1))
            if result.heap_kb_min is None or heap_kb < result.heap_kb_min:
                result.heap_kb_min = heap_kb

        result.tail.append(line)
        if len(result.tail) > tail_lines:
            result.tail = result.tail[-tail_lines:]

    ser.close()


def print_hits(label: str, hits: list[tuple[float, str]]) -> None:
    if not hits:
        return
    for elapsed, line in hits:
        print(f"{label} t={elapsed:0.2f}s {line}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Run SRC+OUT HIL soak crash check.")
    parser.add_argument("--src-port", required=True, help="SRC serial monitor device path (monitor port)")
    parser.add_argument("--out-port", required=True, help="OUT serial device path")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate")
    parser.add_argument("--duration", type=int, default=300, help="Soak duration in seconds")
    parser.add_argument(
        "--ignore-reset-window",
        type=float,
        default=8.0,
        help="Seconds after serial-open reset where rst: lines are ignored",
    )
    parser.add_argument(
        "--tail-lines",
        type=int,
        default=80,
        help="How many trailing lines to print per node",
    )
    parser.add_argument(
        "--fault-schedule",
        default=None,
        help="Optional JSON file with timed fault actions for SRC/OUT (serial_reset, log_marker)",
    )
    parser.add_argument(
        "--summary-json",
        default=None,
        help="Optional path to write machine-readable soak summary JSON",
    )
    parser.add_argument(
        "--skip-reset-toggle",
        action="store_true",
        help="Do not toggle DTR/RTS when opening ports (avoids entering bootloader on some USB setups)",
    )
    parser.add_argument(
        "--allow-zero-ok-hits",
        action="store_true",
        help="Do not fail solely because OK-pattern hits are zero (useful for quieter runtime log profiles)",
    )
    args = parser.parse_args()

    results = {"SRC": PortResult(), "OUT": PortResult()}
    fault_schedule = load_fault_schedule(args.fault_schedule)
    src_schedule = node_fault_schedule(fault_schedule, "SRC")
    out_schedule = node_fault_schedule(fault_schedule, "OUT")

    threads = [
        threading.Thread(
            target=read_port,
            args=(
                "SRC",
                args.src_port,
                args.baud,
                args.duration,
                args.ignore_reset_window,
                args.tail_lines,
                results["SRC"],
                src_schedule,
                args.skip_reset_toggle,
            ),
            daemon=True,
        ),
        threading.Thread(
            target=read_port,
            args=(
                "OUT",
                args.out_port,
                args.baud,
                args.duration,
                args.ignore_reset_window,
                args.tail_lines,
                results["OUT"],
                out_schedule,
                args.skip_reset_toggle,
            ),
            daemon=True,
        ),
    ]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()

    print("=== SOAK SUMMARY ===")
    failed = False
    for node in ("SRC", "OUT"):
        data = results[node]
        print(
            f"{node}: open_err={data.open_error}, lines={data.lines_read}, "
            f"ok_hits={data.ok_hits}, panic_hits={len(data.panic_hits)}, "
            f"late_rsts={len(data.late_reset_hits)}, heap_kb_min={data.heap_kb_min}, "
            f"faults={len(data.fault_fired)}/{data.planned_faults}"
        )
        if data.open_error or data.panic_hits or data.late_reset_hits:
            failed = True
        if data.ok_hits == 0 and not args.allow_zero_ok_hits:
            failed = True
        if data.planned_faults > 0 and len(data.fault_fired) == 0:
            print(f"{node}: scheduled faults were configured but none were fired")
            failed = True

    for node in ("SRC", "OUT"):
        print(f"\n=== {node} panic/reset details ===")
        print_hits("PANIC", results[node].panic_hits)
        print_hits("RESET", results[node].late_reset_hits)

    for node in ("SRC", "OUT"):
        if results[node].fault_fired:
            print(f"\n=== {node} fired faults ===")
            for item in results[node].fault_fired:
                print(item)

    for node in ("SRC", "OUT"):
        print(f"\n=== {node} tail (last {args.tail_lines}) ===")
        for line in results[node].tail:
            print(line)

    print("\nSOAK_RESULT=" + ("FAIL" if failed else "PASS"))

    summary = {
        "result": "FAIL" if failed else "PASS",
        "duration_seconds": args.duration,
        "ignore_reset_window_seconds": args.ignore_reset_window,
        "fault_schedule_path": args.fault_schedule,
        "nodes": {
            "SRC": {
                "open_error": results["SRC"].open_error,
                "lines_read": results["SRC"].lines_read,
                "ok_hits": results["SRC"].ok_hits,
                "panic_hits": len(results["SRC"].panic_hits),
                "late_reset_hits": len(results["SRC"].late_reset_hits),
                "heap_kb_min": results["SRC"].heap_kb_min,
                "planned_faults": results["SRC"].planned_faults,
                "fired_faults": len(results["SRC"].fault_fired),
            },
            "OUT": {
                "open_error": results["OUT"].open_error,
                "lines_read": results["OUT"].lines_read,
                "ok_hits": results["OUT"].ok_hits,
                "panic_hits": len(results["OUT"].panic_hits),
                "late_reset_hits": len(results["OUT"].late_reset_hits),
                "heap_kb_min": results["OUT"].heap_kb_min,
                "planned_faults": results["OUT"].planned_faults,
                "fired_faults": len(results["OUT"].fault_fired),
            },
        },
    }
    if args.summary_json:
        Path(args.summary_json).write_text(json.dumps(summary, indent=2) + "\n")
        print(f"SOAK_SUMMARY_JSON={args.summary_json}")

    if not failed:
        src_heap = results["SRC"].heap_kb_min
        out_heap = results["OUT"].heap_kb_min
        if src_heap is not None:
            print(f"SUGGESTED_SRC_FREE_HEAP_MIN_BYTES={src_heap * 1024}")
        if out_heap is not None:
            print(f"SUGGESTED_OUT_FREE_HEAP_MIN_BYTES={out_heap * 1024}")
    return 2 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
