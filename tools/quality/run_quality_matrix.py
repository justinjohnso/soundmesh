#!/usr/bin/env python3
"""Run quality scoring across profile matrix with optional hook commands."""

from __future__ import annotations

import argparse
import csv
import json
import subprocess
import sys
from pathlib import Path
from typing import Any


DEFAULT_PROFILES = {
    "profiles": [
        {
            "name": "baseline",
            "reference_wav": "data/quality/reference.wav",
            "captured_wav": "data/quality/captured-baseline.wav",
            "hooks": {
                "pre": [],
                "capture": [],
                "post": [],
            },
        }
    ]
}


def run_hook_commands(commands: list[str], cwd: Path) -> None:
    for command in commands:
        completed = subprocess.run(command, shell=True, cwd=str(cwd), check=False)
        if completed.returncode != 0:
            raise RuntimeError(f"Hook command failed ({completed.returncode}): {command}")


def score_profile(
    repo_root: Path,
    score_script: Path,
    profile: dict[str, Any],
    analysis_rate: int,
    output_dir: Path,
    thresholds_path: Path | None,
) -> tuple[dict[str, Any] | None, str | None]:
    name = str(profile.get("name", "unnamed"))
    ref = profile.get("reference_wav")
    cap = profile.get("captured_wav")
    if not isinstance(ref, str) or not isinstance(cap, str):
        return None, "profile requires string fields: reference_wav, captured_wav"

    result_json = output_dir / f"{name}-quality.json"
    cmd = [
        sys.executable,
        str(score_script),
        "--reference",
        str((repo_root / ref).resolve() if not Path(ref).is_absolute() else Path(ref)),
        "--captured",
        str((repo_root / cap).resolve() if not Path(cap).is_absolute() else Path(cap)),
        "--sample-rate",
        str(analysis_rate),
        "--output",
        str(result_json),
    ]
    if thresholds_path is not None:
        cmd.extend(["--thresholds", str(thresholds_path)])
    completed = subprocess.run(cmd, check=False)
    if completed.returncode != 0:
        return None, f"scoring failed with exit code {completed.returncode}"
    payload = json.loads(result_json.read_text())
    return payload, None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run output-quality profile matrix")
    parser.add_argument("--profiles", default=None, help="Profile JSON path")
    parser.add_argument("--sample-rate", type=int, default=48000, help="Analysis sample rate")
    parser.add_argument(
        "--output-dir",
        default="docs/quality/reports",
        help="Output directory for per-profile and aggregate results",
    )
    parser.add_argument(
        "--score-script",
        default="tools/quality/score_output_quality.py",
        help="Path to score_output_quality.py",
    )
    parser.add_argument(
        "--skip-hooks",
        action="store_true",
        help="Skip pre/capture/post hook commands",
    )
    parser.add_argument(
        "--thresholds",
        default=None,
        help=(
            "Optional thresholds JSON path passed to score_output_quality.py. "
            "If omitted, scorer uses its default thresholds file when present."
        ),
    )
    parser.add_argument(
        "--fail-on-band",
        choices=["off", "fail", "warn"],
        default="fail",
        help=(
            "Quality gate policy for threshold bands: "
            "'fail' fails matrix on any fail profile (default), "
            "'warn' fails on warn or fail, 'off' disables threshold gating."
        ),
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.sample_rate <= 0:
        print("run_quality_matrix.py error: --sample-rate must be > 0")
        return 2

    repo_root = Path(__file__).resolve().parents[2]
    output_dir = repo_root / args.output_dir
    output_dir.mkdir(parents=True, exist_ok=True)
    score_script = repo_root / args.score_script
    thresholds_path: Path | None = None
    if args.thresholds:
        thresholds_path = Path(args.thresholds)
        if not thresholds_path.is_absolute():
            thresholds_path = repo_root / thresholds_path
        if not thresholds_path.exists():
            print(f"run_quality_matrix.py error: missing thresholds file {thresholds_path}")
            return 2

    if not score_script.exists():
        print(f"run_quality_matrix.py error: missing score script {score_script}")
        return 2

    if args.profiles:
        profile_path = Path(args.profiles)
        if not profile_path.is_absolute():
            profile_path = repo_root / profile_path
        if not profile_path.exists():
            print(f"run_quality_matrix.py error: missing profiles file {profile_path}")
            return 2
        profile_payload = json.loads(profile_path.read_text())
    else:
        profile_payload = DEFAULT_PROFILES

    profiles = profile_payload.get("profiles")
    if not isinstance(profiles, list) or not profiles:
        print("run_quality_matrix.py error: profiles JSON must contain non-empty 'profiles' list")
        return 2

    aggregate: list[dict[str, Any]] = []
    exit_code = 0
    for profile in profiles:
        name = str(profile.get("name", "unnamed"))
        hooks = profile.get("hooks", {}) if isinstance(profile.get("hooks", {}), dict) else {}
        try:
            if not args.skip_hooks:
                run_hook_commands([str(x) for x in hooks.get("pre", [])], repo_root)
                run_hook_commands([str(x) for x in hooks.get("capture", [])], repo_root)
                run_hook_commands([str(x) for x in hooks.get("post", [])], repo_root)
            payload, err = score_profile(
                repo_root,
                score_script,
                profile,
                args.sample_rate,
                output_dir,
                thresholds_path,
            )
            if err:
                aggregate.append(
                    {
                        "name": name,
                        "status": "failed",
                        "error": err,
                        "quality_score": None,
                    }
                )
                exit_code = 2
                continue
            assert payload is not None
            aggregate.append(
                {
                    "name": name,
                    "status": "ok",
                    "error": None,
                    "quality_score": payload.get("quality_score"),
                    "dropout_ratio_pct": payload.get("dropout_ratio_pct"),
                    "lag_ms_mean": payload.get("lag_ms_mean"),
                    "lag_ms_std": payload.get("lag_ms_std"),
                    "spectral_deviation_db": payload.get("spectral_deviation_db"),
                    "overall_band": (
                        payload.get("threshold_evaluation", {}) or {}
                    ).get("overall_band"),
                    "result_json": str(output_dir / f"{name}-quality.json"),
                }
            )
            band = ((payload.get("threshold_evaluation", {}) or {}).get("overall_band"))
            if args.fail_on_band != "off":
                if args.fail_on_band == "fail" and band == "fail":
                    exit_code = 2
                elif args.fail_on_band == "warn" and band in {"warn", "fail"}:
                    exit_code = 2
        except Exception as exc:
            aggregate.append(
                {
                    "name": name,
                    "status": "failed",
                    "error": str(exc),
                    "quality_score": None,
                }
            )
            exit_code = 2

    ranked = sorted(
        aggregate,
        key=lambda item: (item.get("quality_score") is None, -(item.get("quality_score") or -1.0)),
    )

    matrix_json = output_dir / "quality-matrix-results.json"
    matrix_csv = output_dir / "quality-matrix-results.csv"
    matrix_json.write_text(json.dumps({"profiles": ranked}, indent=2) + "\n")

    fieldnames = [
        "name",
        "status",
        "quality_score",
        "dropout_ratio_pct",
        "lag_ms_mean",
        "lag_ms_std",
        "spectral_deviation_db",
        "overall_band",
        "result_json",
        "error",
    ]
    with matrix_csv.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in ranked:
            writer.writerow({key: row.get(key) for key in fieldnames})

    print(json.dumps({"json": str(matrix_json), "csv": str(matrix_csv), "profiles": len(ranked)}, indent=2))
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
