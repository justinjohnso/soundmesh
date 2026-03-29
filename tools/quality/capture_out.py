#!/usr/bin/env python3
"""Capture OUT audio on host using ffmpeg/sox frontends."""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path


@dataclass
class CaptureConfig:
    device: str
    duration: float
    sample_rate: int
    channels: int
    output_wav: Path
    metadata_json: Path | None
    ffmpeg_input_format: str | None


def utc_now_iso() -> str:
    return datetime.now(UTC).isoformat().replace("+00:00", "Z")


def build_ffmpeg_cmd(cfg: CaptureConfig) -> list[str]:
    platform = sys.platform
    if cfg.ffmpeg_input_format:
        input_format = cfg.ffmpeg_input_format
        input_spec = cfg.device
    elif platform == "darwin":
        input_format = "avfoundation"
        input_spec = f":{cfg.device}"
    elif platform.startswith("linux"):
        input_format = "alsa"
        input_spec = cfg.device
    else:
        raise RuntimeError(
            "ffmpeg capture backend requires --ffmpeg-input-format on this platform"
        )

    return [
        "ffmpeg",
        "-hide_banner",
        "-loglevel",
        "error",
        "-y",
        "-f",
        input_format,
        "-i",
        input_spec,
        "-t",
        str(cfg.duration),
        "-ar",
        str(cfg.sample_rate),
        "-ac",
        str(cfg.channels),
        str(cfg.output_wav),
    ]


def build_sox_cmd(cfg: CaptureConfig) -> list[str]:
    if sys.platform == "darwin":
        return [
            "sox",
            "-q",
            "-t",
            "coreaudio",
            cfg.device,
            "-r",
            str(cfg.sample_rate),
            "-c",
            str(cfg.channels),
            str(cfg.output_wav),
            "trim",
            "0",
            str(cfg.duration),
        ]
    if sys.platform.startswith("linux"):
        return [
            "sox",
            "-q",
            "-t",
            "alsa",
            cfg.device,
            "-r",
            str(cfg.sample_rate),
            "-c",
            str(cfg.channels),
            str(cfg.output_wav),
            "trim",
            "0",
            str(cfg.duration),
        ]
    raise RuntimeError("sox capture backend is not configured for this platform")


def run_capture(cfg: CaptureConfig) -> tuple[str, list[str]]:
    ffmpeg_path = shutil.which("ffmpeg")
    sox_path = shutil.which("sox")

    if ffmpeg_path:
        cmd = build_ffmpeg_cmd(cfg)
        backend = "ffmpeg"
    elif sox_path:
        cmd = build_sox_cmd(cfg)
        backend = "sox"
    else:
        raise RuntimeError(
            "Neither ffmpeg nor sox is available on PATH. "
            "Install one of them, then rerun capture_out.py."
        )

    cfg.output_wav.parent.mkdir(parents=True, exist_ok=True)
    completed = subprocess.run(cmd, check=False)
    if completed.returncode != 0:
        raise RuntimeError(
            f"{backend} capture failed with exit code {completed.returncode}. "
            "Verify the device id/name and host audio permissions."
        )
    return backend, cmd


def parse_args() -> CaptureConfig:
    parser = argparse.ArgumentParser(description="Capture OUT audio to WAV")
    parser.add_argument("--device", required=True, help="Host audio input device name/id")
    parser.add_argument("--duration", type=float, required=True, help="Capture duration in seconds")
    parser.add_argument("--sample-rate", type=int, default=48000)
    parser.add_argument("--channels", type=int, default=1)
    parser.add_argument("--output", required=True, help="Output WAV path")
    parser.add_argument(
        "--metadata-json",
        default=None,
        help="Optional JSON metadata output path",
    )
    parser.add_argument(
        "--write-metadata",
        action="store_true",
        help="Write metadata JSON next to WAV if --metadata-json is not provided",
    )
    parser.add_argument(
        "--ffmpeg-input-format",
        default=None,
        help="Optional ffmpeg -f input format override",
    )
    args = parser.parse_args()

    if args.duration <= 0:
        raise SystemExit("--duration must be > 0")
    if args.sample_rate < 8000:
        raise SystemExit("--sample-rate must be >= 8000")
    if args.channels <= 0:
        raise SystemExit("--channels must be >= 1")

    output_wav = Path(args.output)
    metadata_path = Path(args.metadata_json) if args.metadata_json else None
    if args.write_metadata and metadata_path is None:
        metadata_path = output_wav.with_suffix(".metadata.json")
    return CaptureConfig(
        device=str(args.device),
        duration=float(args.duration),
        sample_rate=int(args.sample_rate),
        channels=int(args.channels),
        output_wav=output_wav,
        metadata_json=metadata_path,
        ffmpeg_input_format=args.ffmpeg_input_format,
    )


def main() -> int:
    cfg = parse_args()
    started_at = utc_now_iso()
    try:
        backend, cmd = run_capture(cfg)
    except Exception as exc:
        print(f"capture_out.py error: {exc}", file=sys.stderr)
        return 2

    finished_at = utc_now_iso()
    metadata = {
        "tool": "capture_out.py",
        "timestamp_start_utc": started_at,
        "timestamp_end_utc": finished_at,
        "backend": backend,
        "device": cfg.device,
        "duration_seconds": cfg.duration,
        "sample_rate_hz": cfg.sample_rate,
        "channels": cfg.channels,
        "output_wav": str(cfg.output_wav),
        "command": cmd,
    }
    metadata_path = cfg.metadata_json
    if metadata_path is not None:
        metadata_path.parent.mkdir(parents=True, exist_ok=True)
        metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")

    print(json.dumps(metadata, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
