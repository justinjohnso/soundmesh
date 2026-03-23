#!/usr/bin/env python3
"""Hardware-in-the-loop soak checker for SRC+OUT stability.

This tool validates that both nodes stay alive during a timed soak and fail-fast
on panic/reboot-loop signatures. It intentionally ignores the initial USB reset
line emitted when serial is opened.
"""

from __future__ import annotations

import argparse
import re
import sys
import threading
import time
from dataclasses import dataclass, field

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
SRC_OK_PATTERN = re.compile(r"combo_main|tx_main|TX:|Mesh TX GROUP|In:AUX", re.IGNORECASE)
OUT_OK_PATTERN = re.compile(r"rx_main|RX:|Playback|Audio frame RX|Audio:RX", re.IGNORECASE)
HEAP_PATTERN = re.compile(r"heap_kb:(\d+)", re.IGNORECASE)


@dataclass
class PortResult:
    open_error: str | None = None
    lines_read: int = 0
    ok_hits: int = 0
    panic_hits: list[tuple[float, str]] = field(default_factory=list)
    late_reset_hits: list[tuple[float, str]] = field(default_factory=list)
    heap_kb_min: int | None = None
    tail: list[str] = field(default_factory=list)


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
) -> None:
    try:
        ser = serial.Serial(port, baudrate=baud_rate, timeout=0.25)
    except Exception as exc:
        result.open_error = str(exc)
        return

    maybe_reset_port(ser)
    started_at = time.time()

    ok_pattern = SRC_OK_PATTERN if node_name == "SRC" else OUT_OK_PATTERN

    while time.time() - started_at < duration_s:
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
    parser.add_argument("--src-port", required=True, help="SRC serial device path")
    parser.add_argument("--out-port", required=True, help="OUT serial device path")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate")
    parser.add_argument("--duration", type=int, default=120, help="Soak duration in seconds")
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
    args = parser.parse_args()

    results = {"SRC": PortResult(), "OUT": PortResult()}

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
            f"late_rsts={len(data.late_reset_hits)}, heap_kb_min={data.heap_kb_min}"
        )
        if data.open_error or data.ok_hits == 0 or data.panic_hits or data.late_reset_hits:
            failed = True

    for node in ("SRC", "OUT"):
        print(f"\n=== {node} panic/reset details ===")
        print_hits("PANIC", results[node].panic_hits)
        print_hits("RESET", results[node].late_reset_hits)

    for node in ("SRC", "OUT"):
        print(f"\n=== {node} tail (last {args.tail_lines}) ===")
        for line in results[node].tail:
            print(line)

    print("\nSOAK_RESULT=" + ("FAIL" if failed else "PASS"))
    if not failed:
        src_heap = results["SRC"].heap_kb_min
        out_heap = results["OUT"].heap_kb_min
        if src_heap is not None:
            print(f"SUGGESTED_COMBO_FREE_HEAP_MIN_BYTES={src_heap * 1024}")
            print(f"SUGGESTED_TX_FREE_HEAP_MIN_BYTES={src_heap * 1024}")
        if out_heap is not None:
            print(f"SUGGESTED_RX_FREE_HEAP_MIN_BYTES={out_heap * 1024}")
    return 2 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
