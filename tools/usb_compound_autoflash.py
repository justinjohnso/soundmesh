#!/usr/bin/env python3
"""Automate no-reset OTA flashing over USB portal (/api/ota)."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from typing import Any


EXIT_OK = 0
EXIT_RUNTIME_ERROR = 2
EXIT_TIMEOUT = 3


@dataclass
class OtaStatus:
    in_progress: bool
    phase: str
    last_ok: bool
    last_err: int


def positive_float(value: str) -> float:
    try:
        parsed = float(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"must be a number, got '{value}'") from exc
    if parsed <= 0:
        raise argparse.ArgumentTypeError(f"must be > 0, got {parsed}")
    return parsed


def validate_https_firmware_url(raw_url: str) -> str:
    parsed = urllib.parse.urlparse(raw_url)
    if parsed.scheme.lower() != "https":
        raise argparse.ArgumentTypeError("firmware URL must use https://")
    if not parsed.netloc:
        raise argparse.ArgumentTypeError("firmware URL must include a host")
    if not parsed.path or parsed.path == "/":
        raise argparse.ArgumentTypeError("firmware URL must include a firmware file path")
    if not parsed.path.lower().endswith(".bin"):
        raise argparse.ArgumentTypeError("firmware URL path must end with .bin")
    return raw_url


def normalize_base_url(raw_url: str) -> str:
    parsed = urllib.parse.urlparse(raw_url)
    if parsed.scheme.lower() not in ("http", "https"):
        raise argparse.ArgumentTypeError("portal base URL must use http:// or https://")
    if not parsed.netloc:
        raise argparse.ArgumentTypeError("portal base URL must include a host/IP")
    if parsed.path not in ("", "/"):
        raise argparse.ArgumentTypeError("portal base URL must not include a path")
    if parsed.query or parsed.fragment:
        raise argparse.ArgumentTypeError("portal base URL must not include query/fragment")
    return f"{parsed.scheme}://{parsed.netloc}"


def make_auth_headers(token: str | None, auth_mode: str) -> dict[str, str]:
    headers: dict[str, str] = {}
    if not token:
        return headers
    if auth_mode == "bearer":
        headers["Authorization"] = f"Bearer {token}"
    elif auth_mode == "x-token":
        headers["X-SoundMesh-Token"] = token
    else:  # pragma: no cover
        raise ValueError(f"unsupported auth mode '{auth_mode}'")
    return headers


def build_src_firmware(pio_cmd: str) -> None:
    print(f"[build] Running: {pio_cmd} run -e src")
    completed = subprocess.run([pio_cmd, "run", "-e", "src"], check=False)
    if completed.returncode != 0:
        raise RuntimeError(f"build failed with exit code {completed.returncode}")
    print("[build] OK")


def http_json_request(
    method: str,
    url: str,
    headers: dict[str, str],
    body_obj: dict[str, Any] | None,
    request_timeout_s: float,
) -> dict[str, Any]:
    body = None
    request_headers = dict(headers)
    if body_obj is not None:
        body = json.dumps(body_obj).encode("utf-8")
        request_headers["Content-Type"] = "application/json"
    request_headers["Accept"] = "application/json"
    req = urllib.request.Request(url=url, method=method, data=body, headers=request_headers)
    with urllib.request.urlopen(req, timeout=request_timeout_s) as resp:
        payload = resp.read().decode("utf-8", errors="replace").strip()
    if not payload:
        return {}
    return json.loads(payload)


def fetch_ota_status(base_url: str, headers: dict[str, str], request_timeout_s: float) -> OtaStatus:
    payload = http_json_request(
        method="GET",
        url=f"{base_url}/api/ota",
        headers=headers,
        body_obj=None,
        request_timeout_s=request_timeout_s,
    )
    return OtaStatus(
        in_progress=bool(payload.get("inProgress", False)),
        phase=str(payload.get("phase", "")),
        last_ok=bool(payload.get("lastOk", False)),
        last_err=int(payload.get("lastErr", 0)),
    )


def trigger_ota(
    base_url: str,
    firmware_url: str,
    headers: dict[str, str],
    request_timeout_s: float,
) -> None:
    url = f"{base_url}/api/ota"
    try:
        payload = http_json_request(
            method="POST",
            url=url,
            headers=headers,
            body_obj={"url": firmware_url},
            request_timeout_s=request_timeout_s,
        )
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", errors="replace").strip()
        raise RuntimeError(f"OTA trigger failed: HTTP {exc.code} at {url}: {body}") from exc
    except urllib.error.URLError as exc:
        raise RuntimeError(f"OTA trigger failed: cannot reach {url}: {exc}") from exc
    except json.JSONDecodeError:
        payload = {}
    if payload and not payload.get("ok", False):
        raise RuntimeError(f"OTA trigger did not acknowledge success: {payload}")
    print("[ota] Trigger accepted")


def poll_until_done(
    base_url: str,
    headers: dict[str, str],
    request_timeout_s: float,
    poll_interval_s: float,
    timeout_s: float,
) -> int:
    started = time.time()
    deadline = started + timeout_s
    last_phase: str | None = None
    saw_active = False
    saw_restarting = False
    saw_unreachable = False
    saw_unreachable_before_any_status = False
    saw_successful_status = False
    printed_unreachable = False

    print("[ota] Polling /api/ota for phase transitions...")
    while time.time() < deadline:
        now = time.time()
        elapsed = now - started
        try:
            status = fetch_ota_status(base_url, headers, request_timeout_s)
        except urllib.error.HTTPError as exc:
            if exc.code in (401, 403):
                print(
                    f"[ota] FAILED: auth rejected while polling /api/ota "
                    f"(HTTP {exc.code}); aborting."
                )
                return EXIT_RUNTIME_ERROR
            print(
                f"[ota] FAILED: unexpected HTTP error while polling /api/ota "
                f"(HTTP {exc.code}): {exc.reason}"
            )
            return EXIT_RUNTIME_ERROR
        except (urllib.error.URLError, TimeoutError):
            saw_unreachable = True
            if not saw_successful_status:
                saw_unreachable_before_any_status = True
            if not printed_unreachable:
                print("[ota] Device became unreachable (often expected during reboot)")
                printed_unreachable = True
            time.sleep(poll_interval_s)
            continue
        except json.JSONDecodeError as exc:
            raise RuntimeError(f"invalid JSON from {base_url}/api/ota: {exc}") from exc

        saw_successful_status = True
        if status.phase != last_phase:
            print(
                f"[ota] t={elapsed:0.1f}s phase={status.phase or '<empty>'} "
                f"inProgress={status.in_progress} lastErr={status.last_err}"
            )
            last_phase = status.phase

        if status.in_progress or status.phase in ("queued", "downloading", "restarting"):
            saw_active = True
        if status.phase == "restarting":
            saw_restarting = True
        if status.phase == "failed" or (not status.in_progress and status.last_err != 0):
            print("[ota] FAILED: firmware reported OTA failure; check serial logs and URL trust chain.")
            return EXIT_RUNTIME_ERROR

        if saw_active and not status.in_progress:
            if status.last_ok:
                print("[ota] SUCCESS: firmware reports lastOk=true.")
                return EXIT_OK
            if saw_restarting and status.phase == "idle":
                if saw_unreachable:
                    print("[ota] SUCCESS: observed restarting -> disconnect -> idle.")
                else:
                    print("[ota] SUCCESS: observed restarting -> idle.")
                return EXIT_OK
        if (
            not saw_active
            and saw_unreachable_before_any_status
            and not status.in_progress
            and status.phase == "idle"
            and status.last_ok
            and status.last_err == 0
        ):
            print("[ota] SUCCESS: observed disconnect before first status, then idle+lastOk=true.")
            return EXIT_OK

        time.sleep(poll_interval_s)

    print(
        f"[ota] TIMEOUT after {timeout_s:.1f}s. "
        "Action: verify HTTPS firmware URL, token, and reachability from SRC root."
    )
    return EXIT_TIMEOUT


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="No-manual-reset OTA trigger/poll helper for USB compound portal.",
        epilog=(
            "Example:\n"
            "  python3 tools/usb_compound_autoflash.py "
            "--base-url http://192.168.4.1 "
            "--firmware-url https://example.com/firmware.bin "
            "--token YOUR_TOKEN --auth-mode bearer --build"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--base-url",
        required=True,
        type=normalize_base_url,
        help="Portal root URL (e.g. http://192.168.4.1)",
    )
    parser.add_argument(
        "--firmware-url",
        required=True,
        type=validate_https_firmware_url,
        help="HTTPS URL to firmware .bin reachable from SRC root",
    )
    parser.add_argument("--build", action="store_true", help="Run 'pio run -e src' before OTA")
    parser.add_argument("--pio-cmd", default="pio", help="PlatformIO executable (default: pio)")
    parser.add_argument(
        "--token",
        required=True,
        help="Control-plane auth token (required for protected POST /api/ota)",
    )
    parser.add_argument(
        "--auth-mode",
        choices=("bearer", "x-token"),
        default="bearer",
        help="Token header style: bearer=Authorization, x-token=X-SoundMesh-Token",
    )
    parser.add_argument(
        "--request-timeout",
        type=positive_float,
        default=5.0,
        help="Per-request HTTP timeout in seconds (default: 5)",
    )
    parser.add_argument(
        "--poll-interval",
        type=positive_float,
        default=1.0,
        help="Poll interval for GET /api/ota in seconds (default: 1)",
    )
    parser.add_argument(
        "--timeout",
        type=positive_float,
        default=180.0,
        help="Overall OTA poll timeout in seconds (default: 180)",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print planned actions only; do not build or call API",
    )
    args = parser.parse_args(argv)
    if args.poll_interval > args.timeout:
        parser.error("--poll-interval must be <= --timeout")
    if args.build and not args.pio_cmd.strip():
        parser.error("--pio-cmd cannot be empty when --build is used")
    return args


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    headers = make_auth_headers(args.token, args.auth_mode)

    if args.dry_run:
        print("DRY RUN: no commands executed")
        print(f"- base URL:     {args.base_url}")
        print(f"- firmware URL: {args.firmware_url}")
        print(f"- build step:   {'yes' if args.build else 'no'}")
        print(f"- auth mode:    {args.auth_mode} (token provided)")
        print(f"- POST payload: {json.dumps({'url': args.firmware_url})}")
        print(f"- timeout:      {args.timeout}s (poll every {args.poll_interval}s)")
        return EXIT_OK

    try:
        if args.build:
            build_src_firmware(args.pio_cmd)
        trigger_ota(
            base_url=args.base_url,
            firmware_url=args.firmware_url,
            headers=headers,
            request_timeout_s=args.request_timeout,
        )
        return poll_until_done(
            base_url=args.base_url,
            headers=headers,
            request_timeout_s=args.request_timeout,
            poll_interval_s=args.poll_interval,
            timeout_s=args.timeout,
        )
    except RuntimeError as exc:
        print(f"[error] {exc}", file=sys.stderr)
        return EXIT_RUNTIME_ERROR


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
