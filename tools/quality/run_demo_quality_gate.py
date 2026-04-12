#!/usr/bin/env python3
"""Run consolidated demo-quality gates for SRC/OUT release validation."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from datetime import UTC, datetime
from pathlib import Path
from typing import Any


SLO_THRESHOLDS: dict[str, tuple[str, float]] = {
    "join_time_s": ("<=", 20.0),
    "rejoin_time_s": ("<=", 30.0),
    "stream_continuity_pct": (">=", 99.0),
    "underruns_per_min": ("<=", 1.0),
    "decode_failures_per_min": ("<=", 0.5),
    "loss_pct": ("<=", 2.0),
}
NON_REGRESSION_THRESHOLDS: dict[str, str] = {
    "underruns_per_min": "max",
    "decode_failures_per_min": "max",
    "loss_pct": "max",
    "reason201_per_min": "max",
    "buf0_events_per_min": "max",
    "tx_backpressure_nonzero_samples_per_min": "max",
    "reason201_cadence_s": "min",
    "buf0_cadence_s": "min",
    "underrun_cadence_s": "min",
    "tx_backpressure_cadence_s": "min",
    "raw.tx_obs_backpressure_level_max": "max",
}

SCRIPT_NAME = "run_demo_quality_gate.py"
DEFAULT_QUALITY_THRESHOLDS_PATH = Path(__file__).resolve().with_name("output_quality_thresholds.json")
VALID_JOIN_TIME_SOURCES = {"none", "src_out_max", "src_only", "out_only"}
TRANSPORT_NODE_ARTIFACT_KEYS = ("nodes", "node_metrics", "out_nodes")
SOAK_REQUIRED_NODES = ("SRC", "OUT")


def utc_now_iso() -> str:
    return datetime.now(UTC).isoformat().replace("+00:00", "Z")


def resolve_path(raw_path: str) -> Path:
    candidate = Path(raw_path).expanduser()
    if candidate.is_absolute():
        return candidate.resolve()
    return (Path.cwd() / candidate).resolve()


def require_existing_file(path: Path, label: str) -> None:
    if not path.exists() or not path.is_file():
        raise FileNotFoundError(f"Missing {label}: {path}")


def run_command(command: list[str], label: str) -> int:
    printable = " ".join(command)
    print(f"[demo-gate] Running {label}: {printable}")
    try:
        completed = subprocess.run(command, check=False)
    except OSError as exc:
        raise RuntimeError(f"{label} failed to start ({printable}): {exc}") from exc
    print(f"[demo-gate] {label} exit={completed.returncode}")
    return int(completed.returncode)


def _to_number(value: Any) -> float | None:
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)):
        return float(value)
    return None


def _to_int(value: Any, default: int = 0) -> int:
    if isinstance(value, bool):
        return default
    if isinstance(value, int):
        return value
    if isinstance(value, float):
        return int(value)
    try:
        return int(str(value))
    except (TypeError, ValueError):
        return default


def _passes_threshold(value: float | None, op: str, threshold: float) -> bool:
    if value is None:
        return False
    if op == "<=":
        return value <= threshold
    if op == ">=":
        return value >= threshold
    raise ValueError(f"Unsupported operator: {op}")


def _to_non_negative_int(value: Any) -> int | None:
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value if value >= 0 else None
    if isinstance(value, float):
        if not value.is_integer() or value < 0:
            return None
        return int(value)
    if isinstance(value, str):
        try:
            parsed = int(value)
        except ValueError:
            return None
        return parsed if parsed >= 0 else None
    return None


def _metric_value(metrics: dict[str, Any], dotted_path: str) -> float | None:
    node: Any = metrics
    for key in dotted_path.split("."):
        if not isinstance(node, dict):
            return None
        node = node.get(key)
    return _to_number(node)


def _check_optional_non_negative_int(raw_obj: dict[str, Any], key: str, issues: list[str]) -> None:
    if key not in raw_obj:
        return
    parsed = _to_non_negative_int(raw_obj.get(key))
    if parsed is None:
        issues.append(f"transport raw.{key} must be a non-negative integer")


def validate_soak_summary_payload(soak_summary: dict[str, Any]) -> list[str]:
    issues: list[str] = []
    result = soak_summary.get("result")
    if not isinstance(result, str) or result.upper() not in {"PASS", "FAIL"}:
        issues.append("soak summary result must be PASS or FAIL")

    nodes = soak_summary.get("nodes")
    if not isinstance(nodes, dict):
        issues.append("soak summary nodes must be an object")
        return issues

    for node_name in SOAK_REQUIRED_NODES:
        node_payload = nodes.get(node_name)
        if not isinstance(node_payload, dict):
            issues.append(f"soak summary missing node payload for {node_name}")
            continue
        for key in ("ok_hits", "panic_hits", "late_reset_hits"):
            if _to_non_negative_int(node_payload.get(key)) is None:
                issues.append(f"soak summary {node_name}.{key} must be a non-negative integer")
        if "lines_read" in node_payload and _to_non_negative_int(node_payload.get("lines_read")) is None:
            issues.append(f"soak summary {node_name}.lines_read must be a non-negative integer")

    return issues


def validate_transport_metrics_payload(metrics: dict[str, Any]) -> list[str]:
    issues: list[str] = []
    for metric_name in SLO_THRESHOLDS:
        if metric_name not in metrics:
            issues.append(f"transport metric missing: {metric_name}")
            continue
        raw_value = metrics.get(metric_name)
        if raw_value is not None and _to_number(raw_value) is None:
            issues.append(f"transport metric {metric_name} must be numeric or null")
    raw_obj = metrics.get("raw")
    if not isinstance(raw_obj, dict):
        issues.append("transport metrics missing raw object")
    return issues


def validate_transport_sanity_fields(metrics: dict[str, Any]) -> list[str]:
    issues: list[str] = []
    raw_obj = metrics.get("raw")
    if not isinstance(raw_obj, dict):
        return issues

    _check_optional_non_negative_int(raw_obj, "line_count", issues)
    _check_optional_non_negative_int(raw_obj, "src_line_count", issues)
    _check_optional_non_negative_int(raw_obj, "out_line_count", issues)

    line_count = _to_non_negative_int(raw_obj.get("line_count"))
    src_line_count = _to_non_negative_int(raw_obj.get("src_line_count"))
    out_line_count = _to_non_negative_int(raw_obj.get("out_line_count"))
    if line_count is not None and src_line_count is not None and out_line_count is not None:
        expected = src_line_count + out_line_count
        if line_count != expected:
            issues.append(
                f"transport raw.line_count mismatch: expected {expected} from src/out lines, got {line_count}"
            )
        if line_count == 0:
            issues.append("transport raw.line_count is 0 (empty log input)")

    join_time_source = raw_obj.get("join_time_source")
    if join_time_source is not None and str(join_time_source) not in VALID_JOIN_TIME_SOURCES:
        issues.append(f"transport raw.join_time_source is invalid: {join_time_source}")

    event_counts = raw_obj.get("event_counts")
    if event_counts is not None and not isinstance(event_counts, dict):
        issues.append("transport raw.event_counts must be an object")
        return issues

    if isinstance(event_counts, dict):
        for event_key in ("join_total", "rejoin_events", "underruns", "decode_failures", "reason201_count", "buf0_events"):
            if event_key in event_counts and _to_non_negative_int(event_counts.get(event_key)) is None:
                issues.append(f"transport raw.event_counts.{event_key} must be a non-negative integer")

        join_total = _to_non_negative_int(event_counts.get("join_total"))
        src_join_events = _to_non_negative_int(raw_obj.get("src_join_events"))
        out_join_events = _to_non_negative_int(raw_obj.get("out_join_events"))
        if join_total is not None and src_join_events is not None and out_join_events is not None:
            expected_join_total = src_join_events + out_join_events
            if join_total != expected_join_total:
                issues.append(
                    "transport raw.event_counts.join_total mismatch: "
                    f"expected {expected_join_total} from src/out joins, got {join_total}"
                )

        for raw_key, event_key in (
            ("rejoin_events", "rejoin_events"),
            ("underruns", "underruns"),
            ("decode_failures", "decode_failures"),
            ("reason201_count", "reason201_count"),
            ("buf0_events", "buf0_events"),
        ):
            raw_value = _to_non_negative_int(raw_obj.get(raw_key))
            event_value = _to_non_negative_int(event_counts.get(event_key))
            if raw_value is not None and event_value is not None and raw_value != event_value:
                issues.append(
                    f"transport raw.event_counts.{event_key} mismatch: expected {raw_value} from raw.{raw_key}, got {event_value}"
                )

    return issues


def validate_output_quality_payload(quality_metrics: dict[str, Any]) -> list[str]:
    issues: list[str] = []
    quality_score = _to_number(quality_metrics.get("quality_score"))
    if quality_score is None:
        issues.append("output quality missing numeric quality_score")

    threshold_eval = quality_metrics.get("threshold_evaluation")
    if not isinstance(threshold_eval, dict):
        issues.append("output quality missing threshold_evaluation object")
        return issues

    overall_band = threshold_eval.get("overall_band")
    if str(overall_band) not in {"pass", "warn", "fail"}:
        issues.append(f"output quality threshold_evaluation.overall_band invalid: {overall_band}")
    for list_key in ("failed_metrics", "warned_metrics"):
        if list_key not in threshold_eval or not isinstance(threshold_eval.get(list_key), list):
            issues.append(f"output quality threshold_evaluation.{list_key} must be a list")
    return issues


def _extract_node_name(item: dict[str, Any], fallback: str) -> str:
    for key in ("node", "node_name", "name", "id", "node_id"):
        value = item.get(key)
        if value is not None:
            return str(value)
    return fallback


def extract_transport_node_metrics(nodes_payload: dict[str, Any]) -> dict[str, dict[str, Any]]:
    container: Any = None
    for key in TRANSPORT_NODE_ARTIFACT_KEYS:
        if key in nodes_payload:
            container = nodes_payload.get(key)
            break
    if container is None:
        return {}

    node_metrics: dict[str, dict[str, Any]] = {}
    if isinstance(container, dict):
        for node_name, payload in container.items():
            if isinstance(payload, dict):
                if isinstance(payload.get("metrics"), dict):
                    node_metrics[str(node_name)] = dict(payload["metrics"])
                else:
                    node_metrics[str(node_name)] = dict(payload)
    elif isinstance(container, list):
        for index, entry in enumerate(container):
            if not isinstance(entry, dict):
                continue
            fallback = f"node_{index + 1}"
            node_name = _extract_node_name(entry, fallback)
            if isinstance(entry.get("metrics"), dict):
                node_metrics[node_name] = dict(entry["metrics"])
            else:
                node_metrics[node_name] = dict(entry)
    return node_metrics


def evaluate_transport_candidate(candidate_name: str, metrics: dict[str, Any]) -> dict[str, Any]:
    slo_eval = evaluate_transport_slos(metrics)
    integrity_issues = validate_transport_metrics_payload(metrics)
    sanity_issues = validate_transport_sanity_fields(metrics)
    passed = bool(slo_eval["passed"]) and not integrity_issues and not sanity_issues
    return {
        "candidate": candidate_name,
        "passed": passed,
        "checks": slo_eval.get("checks", {}),
        "failed_metrics": list(slo_eval.get("failed_metrics", [])),
        "integrity_issues": integrity_issues,
        "sanity_issues": sanity_issues,
    }


def _worst_metric_value(checks: dict[str, Any], metric: str, *, higher_is_worse: bool) -> float:
    value = checks.get(metric, {}).get("value")
    numeric = _to_number(value)
    if numeric is None:
        return float("inf") if higher_is_worse else float("-inf")
    return numeric if higher_is_worse else -numeric


def select_worst_transport_candidate(candidates: list[dict[str, Any]]) -> dict[str, Any]:
    if not candidates:
        raise ValueError("No transport candidates available")
    return max(
        candidates,
        key=lambda candidate: (
            0 if candidate.get("passed") else 1,
            len(candidate.get("failed_metrics", [])),
            len(candidate.get("integrity_issues", [])) + len(candidate.get("sanity_issues", [])),
            _worst_metric_value(candidate.get("checks", {}), "loss_pct", higher_is_worse=True),
            _worst_metric_value(candidate.get("checks", {}), "underruns_per_min", higher_is_worse=True),
            _worst_metric_value(candidate.get("checks", {}), "decode_failures_per_min", higher_is_worse=True),
            _worst_metric_value(candidate.get("checks", {}), "stream_continuity_pct", higher_is_worse=False),
        ),
    )


def evaluate_transport_slos(metrics: dict[str, Any]) -> dict[str, Any]:
    checks: dict[str, dict[str, Any]] = {}
    all_pass = True
    for metric, (op, threshold) in SLO_THRESHOLDS.items():
        value = _to_number(metrics.get(metric))
        passed = _passes_threshold(value, op, threshold)
        checks[metric] = {
            "value": value,
            "operator": op,
            "threshold": threshold,
            "passed": passed,
        }
        all_pass = all_pass and passed
    failed_metrics = [name for name, check in checks.items() if not bool(check["passed"])]
    return {
        "passed": all_pass,
        "checks": checks,
        "failed_metrics": failed_metrics,
    }


def evaluate_transport_non_regression(candidate: dict[str, Any], baseline: dict[str, Any]) -> dict[str, Any]:
    checks: dict[str, dict[str, Any]] = {}
    failed_metrics: list[str] = []
    skipped_metrics: list[str] = []

    for metric, policy in NON_REGRESSION_THRESHOLDS.items():
        candidate_value = _metric_value(candidate, metric)
        baseline_value = _metric_value(baseline, metric)
        if candidate_value is None or baseline_value is None:
            skipped_metrics.append(metric)
            checks[metric] = {
                "policy": policy,
                "baseline": baseline_value,
                "candidate": candidate_value,
                "passed": None,
                "skipped": True,
            }
            continue

        if policy == "max":
            passed = candidate_value <= baseline_value
        elif policy == "min":
            passed = candidate_value >= baseline_value
        else:
            raise ValueError(f"Unsupported non-regression policy: {policy}")
        checks[metric] = {
            "policy": policy,
            "baseline": baseline_value,
            "candidate": candidate_value,
            "passed": passed,
            "skipped": False,
        }
        if not passed:
            failed_metrics.append(metric)

    return {
        "passed": not failed_metrics,
        "failed_metrics": failed_metrics,
        "skipped_metrics": skipped_metrics,
        "checks": checks,
    }


def evaluate_quality_gate(quality_metrics: dict[str, Any]) -> dict[str, Any]:
    threshold_eval = quality_metrics.get("threshold_evaluation")
    if not isinstance(threshold_eval, dict):
        return {
            "passed": False,
            "overall_band": None,
            "failed_metrics": [],
            "warned_metrics": [],
            "error": "Missing threshold_evaluation in quality metrics output",
        }
    overall_band = threshold_eval.get("overall_band")
    failed_metrics = threshold_eval.get("failed_metrics", [])
    warned_metrics = threshold_eval.get("warned_metrics", [])
    return {
        "passed": overall_band == "pass",
        "overall_band": overall_band,
        "failed_metrics": failed_metrics if isinstance(failed_metrics, list) else [],
        "warned_metrics": warned_metrics if isinstance(warned_metrics, list) else [],
        "error": None,
    }


def evaluate_soak_gate(soak_summary: dict[str, Any], command_exit_code: int | None) -> dict[str, Any]:
    issues: list[str] = []
    soak_result = str(soak_summary.get("result", "")).upper()
    if soak_result != "PASS":
        issues.append(f"soak summary result is {soak_result or 'UNKNOWN'}")
    if command_exit_code is not None and command_exit_code != 0:
        issues.append(f"hil_soak_check.py exit code {command_exit_code}")

    nodes = soak_summary.get("nodes")
    if not isinstance(nodes, dict):
        issues.append("soak summary is missing 'nodes' object")
        nodes = {}

    for node_name in ("SRC", "OUT"):
        node_data = nodes.get(node_name, {})
        if not isinstance(node_data, dict):
            issues.append(f"{node_name} node summary is malformed")
            continue
        open_error = node_data.get("open_error")
        if open_error:
            issues.append(f"{node_name} open_error={open_error}")
        panic_hits = _to_int(node_data.get("panic_hits"))
        late_reset_hits = _to_int(node_data.get("late_reset_hits"))
        ok_hits = _to_int(node_data.get("ok_hits"))
        if panic_hits > 0:
            issues.append(f"{node_name} panic_hits={panic_hits}")
        if late_reset_hits > 0:
            issues.append(f"{node_name} late_reset_hits={late_reset_hits}")
        if ok_hits <= 0:
            issues.append(f"{node_name} ok_hits={ok_hits}")

    return {
        "passed": len(issues) == 0,
        "issues": issues,
        "result": soak_result or None,
        "nodes": nodes,
    }


def read_json_file(path: Path, label: str) -> dict[str, Any]:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:
        raise RuntimeError(f"Failed to parse {label} JSON at {path}: {exc}") from exc
    if not isinstance(payload, dict):
        raise RuntimeError(f"{label} JSON must be an object: {path}")
    return payload


def read_required_json_artifact(path: Path, label: str) -> tuple[dict[str, Any], str | None]:
    if not path.exists() or not path.is_file():
        return {}, f"{label} output missing: {path}"
    if path.stat().st_size <= 0:
        return {}, f"{label} output is empty: {path}"
    try:
        return read_json_file(path, label), None
    except RuntimeError as exc:
        return {}, str(exc)


def render_markdown_summary(summary: dict[str, Any]) -> str:
    gates = summary.get("gates", {})
    soak_gate = gates.get("hil_soak", {})
    transport_gate = gates.get("transport_slo", {})
    quality_gate = gates.get("output_quality", {})

    soak_notes = ", ".join(soak_gate.get("issues", [])) if soak_gate.get("issues") else "none"
    transport_notes = ", ".join(transport_gate.get("failed_metrics", [])) if transport_gate.get("failed_metrics") else "none"
    quality_notes = f"band={quality_gate.get('overall_band')}"
    if quality_gate.get("error"):
        quality_notes = f"{quality_notes}; error={quality_gate.get('error')}"

    lines = [
        "# Demo Quality Gate Summary",
        "",
        f"- Generated: {summary.get('generated_at_utc')}",
        f"- Final verdict: **{summary.get('final_verdict', {}).get('result', 'FAIL')}**",
        "",
        "| Gate | Result | Notes |",
        "| --- | --- | --- |",
        f"| HIL soak | {'PASS' if soak_gate.get('passed') else 'FAIL'} | {soak_notes} |",
        f"| Transport SLO | {'PASS' if transport_gate.get('passed') else 'FAIL'} | failed_metrics={transport_notes} |",
        f"| Output quality | {'PASS' if quality_gate.get('passed') else 'FAIL'} | {quality_notes} |",
        "",
        "## Artifacts",
        "",
    ]
    artifacts = summary.get("artifacts", {})
    if isinstance(artifacts, dict):
        for key in sorted(artifacts.keys()):
            lines.append(f"- `{key}`: `{artifacts[key]}`")
    return "\n".join(lines) + "\n"


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run consolidated demo-quality gating workflow")
    parser.add_argument("--src-port", default=None, help="SRC serial monitor port for HIL soak")
    parser.add_argument("--out-port", default=None, help="OUT serial monitor port for HIL soak")
    parser.add_argument("--src-log", required=True, help="SRC runtime log path")
    parser.add_argument("--out-log", required=True, help="OUT runtime log path")
    parser.add_argument("--capture-wav", required=True, help="Captured OUT WAV path")
    parser.add_argument("--reference-wav", required=True, help="Reference WAV path")
    parser.add_argument("--duration", type=int, default=300, help="Gate duration in seconds (default: 300)")
    parser.add_argument("--sample-rate", type=int, default=48000, help="Output-quality analysis sample rate")
    parser.add_argument(
        "--thresholds",
        default=str(DEFAULT_QUALITY_THRESHOLDS_PATH),
        help="Output-quality thresholds JSON path",
    )
    parser.add_argument(
        "--output-dir",
        default="docs/quality/reports/demo-quality-gate",
        help="Directory for gate artifacts and summary outputs",
    )
    parser.add_argument(
        "--soak-summary",
        default=None,
        help="Optional existing HIL soak summary JSON path (skip serial soak execution)",
    )
    parser.add_argument(
        "--markdown-output",
        default=None,
        help="Optional markdown summary output path",
    )
    parser.add_argument(
        "--transport-node-metrics",
        default=None,
        help=(
            "Optional per-node transport metrics JSON artifact. "
            "When provided, transport gate evaluates against the worst candidate node."
        ),
    )
    parser.add_argument(
        "--transport-baseline-metrics",
        default=None,
        help=(
            "Optional baseline transport metrics JSON used for cadence/backpressure non-regression policy."
        ),
    )
    args = parser.parse_args(argv)
    if args.duration <= 0:
        parser.error("--duration must be > 0")
    if args.sample_rate <= 0:
        parser.error("--sample-rate must be > 0")
    if args.soak_summary is None and (not args.src_port or not args.out_port):
        parser.error("Provide --src-port and --out-port, or provide --soak-summary")
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)

    repo_root = Path(__file__).resolve().parents[2]
    soak_script = repo_root / "tools" / "hil_soak_check.py"
    extract_script = repo_root / "tools" / "benchmarks" / "extract_metrics.py"
    score_script = repo_root / "tools" / "quality" / "score_output_quality.py"
    transport_threshold_doc = repo_root / "docs" / "quality" / "benchmarks-and-slos.md"

    summary: dict[str, Any] = {
        "generated_at_utc": utc_now_iso(),
        "inputs": {},
        "threshold_sources": {
            "transport_slos_doc": str(transport_threshold_doc.resolve()),
            "quality_thresholds_json": None,
            "transport_slo_thresholds": {
                key: {"operator": spec[0], "threshold": spec[1]} for key, spec in SLO_THRESHOLDS.items()
            },
        },
        "gates": {},
        "artifacts": {},
        "final_verdict": {"passed": False, "result": "FAIL"},
    }

    try:
        require_existing_file(soak_script, "HIL soak script")
        require_existing_file(extract_script, "transport metrics script")
        require_existing_file(score_script, "output quality script")
        require_existing_file(transport_threshold_doc, "transport SLO doc")

        src_log = resolve_path(args.src_log)
        out_log = resolve_path(args.out_log)
        capture_wav = resolve_path(args.capture_wav)
        reference_wav = resolve_path(args.reference_wav)
        thresholds_path = resolve_path(args.thresholds)
        output_dir = resolve_path(args.output_dir)

        require_existing_file(src_log, "SRC log")
        require_existing_file(out_log, "OUT log")
        require_existing_file(capture_wav, "captured WAV")
        require_existing_file(reference_wav, "reference WAV")
        require_existing_file(thresholds_path, "quality thresholds JSON")

        output_dir.mkdir(parents=True, exist_ok=True)
        summary["threshold_sources"]["quality_thresholds_json"] = str(thresholds_path)
        summary["inputs"] = {
            "src_port": args.src_port,
            "out_port": args.out_port,
            "src_log": str(src_log),
            "out_log": str(out_log),
            "capture_wav": str(capture_wav),
            "reference_wav": str(reference_wav),
            "duration_seconds": args.duration,
            "sample_rate_hz": args.sample_rate,
            "output_dir": str(output_dir),
            "soak_summary_override": str(resolve_path(args.soak_summary)) if args.soak_summary else None,
            "transport_node_metrics": str(resolve_path(args.transport_node_metrics)) if args.transport_node_metrics else None,
            "transport_baseline_metrics": (
                str(resolve_path(args.transport_baseline_metrics)) if args.transport_baseline_metrics else None
            ),
        }

        artifacts: dict[str, str] = {}

        if args.soak_summary:
            soak_summary_path = resolve_path(args.soak_summary)
            require_existing_file(soak_summary_path, "HIL soak summary override")
            soak_exit_code: int | None = None
            print(f"[demo-gate] Using existing soak summary: {soak_summary_path}")
        else:
            soak_summary_path = output_dir / "hil-soak-summary.json"
            soak_exit_code = run_command(
                [
                    sys.executable,
                    str(soak_script),
                    "--src-port",
                    str(args.src_port),
                    "--out-port",
                    str(args.out_port),
                    "--duration",
                    str(args.duration),
                    "--summary-json",
                    str(soak_summary_path),
                ],
                label="hil_soak_check.py",
            )

        artifacts["hil_soak_summary_json"] = str(soak_summary_path)
        soak_payload, soak_error = read_required_json_artifact(soak_summary_path, "HIL soak summary")
        soak_integrity_issues = [] if soak_error else validate_soak_summary_payload(soak_payload)
        soak_eval = evaluate_soak_gate(soak_payload, soak_exit_code) if not soak_error else {
            "passed": False,
            "issues": [soak_error],
            "result": None,
            "nodes": {},
        }
        soak_issues = list(soak_eval.get("issues", []))
        if soak_error and soak_error not in soak_issues:
            soak_issues.append(soak_error)
        if soak_exit_code is not None and soak_exit_code != 0:
            soak_issue = f"hil_soak_check.py exit code {soak_exit_code}"
            if soak_issue not in soak_issues:
                soak_issues.append(soak_issue)
        for integrity_issue in soak_integrity_issues:
            soak_issues.append(f"soak artifact integrity: {integrity_issue}")
        soak_passed = bool(soak_eval["passed"]) and not soak_integrity_issues and soak_error is None
        summary["gates"]["hil_soak"] = {
            "passed": soak_passed,
            "status": "pass" if soak_passed else "fail",
            "command_exit_code": soak_exit_code,
            "summary_json": str(soak_summary_path),
            "result": soak_eval.get("result"),
            "issues": soak_issues,
            "nodes": soak_eval.get("nodes", {}),
            "integrity_issues": soak_integrity_issues,
            "error": "; ".join(soak_issues) if soak_issues else None,
        }

        metrics_output = output_dir / "transport-metrics.json"
        metrics_exit_code = run_command(
            [
                sys.executable,
                str(extract_script),
                "--src-log",
                str(src_log),
                "--out-log",
                str(out_log),
                "--duration-seconds",
                str(args.duration),
                "--output",
                str(metrics_output),
            ],
            label="extract_metrics.py",
        )
        artifacts["transport_metrics_json"] = str(metrics_output)
        metrics_payload, metrics_error = read_required_json_artifact(metrics_output, "transport metrics")

        transport_candidates: list[dict[str, Any]] = []
        if metrics_error is None:
            transport_candidates.append(evaluate_transport_candidate("aggregate", metrics_payload))

        transport_node_metrics_error: str | None = None
        if args.transport_node_metrics:
            node_metrics_path = resolve_path(args.transport_node_metrics)
            artifacts["transport_node_metrics_json"] = str(node_metrics_path)
            node_metrics_payload, transport_node_metrics_error = read_required_json_artifact(
                node_metrics_path,
                "transport node metrics",
            )
            if transport_node_metrics_error is None:
                node_candidates = extract_transport_node_metrics(node_metrics_payload)
                if not node_candidates:
                    transport_node_metrics_error = (
                        "transport node metrics missing per-node data (expected one of: "
                        f"{', '.join(TRANSPORT_NODE_ARTIFACT_KEYS)})"
                    )
                for node_name in sorted(node_candidates.keys()):
                    transport_candidates.append(
                        evaluate_transport_candidate(f"node:{node_name}", node_candidates[node_name])
                    )

        selected_transport = select_worst_transport_candidate(transport_candidates) if transport_candidates else None
        baseline_non_regression: dict[str, Any] = {
            "enabled": False,
            "passed": True,
            "failed_metrics": [],
            "skipped_metrics": [],
            "checks": {},
            "issues": [],
        }
        if args.transport_baseline_metrics:
            baseline_metrics_path = resolve_path(args.transport_baseline_metrics)
            artifacts["transport_baseline_metrics_json"] = str(baseline_metrics_path)
            baseline_non_regression["enabled"] = True
            baseline_payload, baseline_error = read_required_json_artifact(
                baseline_metrics_path,
                "transport baseline metrics",
            )
            if baseline_error is not None:
                baseline_non_regression["passed"] = False
                baseline_non_regression["issues"] = [baseline_error]
            elif selected_transport is None:
                baseline_non_regression["passed"] = False
                baseline_non_regression["issues"] = ["transport evaluation produced no candidate metrics"]
            else:
                selected_name = str(selected_transport.get("candidate"))
                selected_metrics = metrics_payload
                if selected_name.startswith("node:") and args.transport_node_metrics:
                    node_payload, node_error = read_required_json_artifact(
                        resolve_path(args.transport_node_metrics),
                        "transport node metrics",
                    )
                    if node_error is not None:
                        baseline_non_regression["passed"] = False
                        baseline_non_regression["issues"] = [node_error]
                    else:
                        node_metrics_map = extract_transport_node_metrics(node_payload)
                        selected_metrics = node_metrics_map.get(selected_name.split(":", 1)[1], {})

                if baseline_non_regression["passed"]:
                    non_regression = evaluate_transport_non_regression(selected_metrics, baseline_payload)
                    baseline_non_regression["passed"] = bool(non_regression["passed"])
                    baseline_non_regression["failed_metrics"] = list(non_regression["failed_metrics"])
                    baseline_non_regression["skipped_metrics"] = list(non_regression["skipped_metrics"])
                    baseline_non_regression["checks"] = dict(non_regression["checks"])
                    if non_regression["failed_metrics"]:
                        baseline_non_regression["issues"] = [
                            "transport non-regression failures: "
                            + ", ".join(map(str, non_regression["failed_metrics"]))
                        ]

        transport_issues: list[str] = []
        if metrics_exit_code != 0:
            transport_issues.append(f"extract_metrics.py exit code {metrics_exit_code}")
        if metrics_error:
            transport_issues.append(metrics_error)
        if transport_node_metrics_error:
            transport_issues.append(transport_node_metrics_error)
        for non_regression_issue in baseline_non_regression.get("issues", []):
            transport_issues.append(str(non_regression_issue))
        if selected_transport is None:
            transport_issues.append("transport evaluation produced no candidate metrics")
        else:
            candidate_name = str(selected_transport.get("candidate", "unknown"))
            for integrity_issue in selected_transport.get("integrity_issues", []):
                transport_issues.append(f"{candidate_name} integrity: {integrity_issue}")
            for sanity_issue in selected_transport.get("sanity_issues", []):
                transport_issues.append(f"{candidate_name} sanity: {sanity_issue}")
        if selected_transport and selected_transport.get("failed_metrics"):
            transport_issues.append(
                "transport SLO failures"
                + (
                    f" ({selected_transport.get('candidate')})"
                    if selected_transport.get("candidate")
                    else ""
                )
                + ": "
                + ", ".join(map(str, selected_transport.get("failed_metrics", [])))
            )
        transport_passed = (
            metrics_exit_code == 0
            and metrics_error is None
            and transport_node_metrics_error is None
            and selected_transport is not None
            and bool(selected_transport.get("passed"))
            and bool(baseline_non_regression.get("passed", True))
            and not transport_issues
        )
        summary["gates"]["transport_slo"] = {
            "passed": transport_passed,
            "status": "pass" if transport_passed else "fail",
            "command_exit_code": metrics_exit_code,
            "metrics_json": str(metrics_output),
            "checked_candidates": [candidate.get("candidate") for candidate in transport_candidates],
            "selected_candidate": selected_transport.get("candidate") if selected_transport else None,
            "checks": selected_transport.get("checks", {}) if selected_transport else {},
            "failed_metrics": selected_transport.get("failed_metrics", []) if selected_transport else list(SLO_THRESHOLDS.keys()),
            "integrity_issues": selected_transport.get("integrity_issues", []) if selected_transport else [],
            "sanity_issues": selected_transport.get("sanity_issues", []) if selected_transport else [],
            "candidate_results": transport_candidates,
            "non_regression": baseline_non_regression,
            "issues": transport_issues,
            "error": "; ".join(transport_issues) if transport_issues else None,
        }

        quality_output = output_dir / "output-quality.json"
        quality_exit_code = run_command(
            [
                sys.executable,
                str(score_script),
                "--reference",
                str(reference_wav),
                "--captured",
                str(capture_wav),
                "--sample-rate",
                str(args.sample_rate),
                "--thresholds",
                str(thresholds_path),
                "--output",
                str(quality_output),
            ],
            label="score_output_quality.py",
        )
        artifacts["output_quality_json"] = str(quality_output)
        quality_payload, quality_error = read_required_json_artifact(quality_output, "output quality")
        quality_integrity_issues = [] if quality_error else validate_output_quality_payload(quality_payload)
        quality_eval = evaluate_quality_gate(quality_payload) if not quality_error else {
            "passed": False,
            "overall_band": None,
            "failed_metrics": [],
            "warned_metrics": [],
            "error": None,
        }
        quality_issues: list[str] = []
        if quality_exit_code != 0:
            quality_issues.append(f"score_output_quality.py exit code {quality_exit_code}")
        if quality_error:
            quality_issues.append(quality_error)
        for integrity_issue in quality_integrity_issues:
            quality_issues.append(f"output quality artifact integrity: {integrity_issue}")
        quality_eval_error = quality_eval.get("error")
        if quality_eval_error:
            quality_issues.append(str(quality_eval_error))
        if quality_eval.get("failed_metrics"):
            quality_issues.append(
                "output quality failures: " + ", ".join(map(str, quality_eval.get("failed_metrics", [])))
            )
        quality_band = quality_eval.get("overall_band")
        if quality_band and quality_band != "pass":
            quality_issues.append(f"output quality band={quality_band}")
        quality_passed = (
            bool(quality_eval["passed"])
            and quality_exit_code == 0
            and quality_error is None
            and not quality_integrity_issues
            and not quality_issues
        )
        summary["gates"]["output_quality"] = {
            "passed": quality_passed,
            "status": "pass" if quality_passed else "fail",
            "command_exit_code": quality_exit_code,
            "quality_json": str(quality_output),
            "overall_band": quality_eval.get("overall_band"),
            "failed_metrics": quality_eval.get("failed_metrics", []),
            "warned_metrics": quality_eval.get("warned_metrics", []),
            "quality_score": quality_payload.get("quality_score"),
            "integrity_issues": quality_integrity_issues,
            "issues": quality_issues,
            "error": "; ".join(quality_issues) if quality_issues else None,
        }

        summary["artifacts"] = artifacts

    except Exception as exc:
        summary["gates"]["runner"] = {
            "passed": False,
            "status": "fail",
            "error": f"{type(exc).__name__}: {exc}",
        }

    gates = summary.get("gates", {})
    final_pass = (
        bool(gates.get("hil_soak", {}).get("passed"))
        and bool(gates.get("transport_slo", {}).get("passed"))
        and bool(gates.get("output_quality", {}).get("passed"))
    )
    summary["final_verdict"] = {
        "passed": final_pass,
        "result": "PASS" if final_pass else "FAIL",
    }

    output_dir = resolve_path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    summary_json_path = output_dir / "demo-quality-gate-summary.json"
    summary_json_path.write_text(json.dumps(summary, indent=2) + "\n")
    print(f"[demo-gate] summary_json={summary_json_path}")

    if args.markdown_output:
        markdown_path = resolve_path(args.markdown_output)
        markdown_path.parent.mkdir(parents=True, exist_ok=True)
        markdown_path.write_text(render_markdown_summary(summary))
        print(f"[demo-gate] markdown_summary={markdown_path}")

    print(f"DEMO_QUALITY_GATE_RESULT={summary['final_verdict']['result']}")
    return 0 if final_pass else 2


if __name__ == "__main__":
    raise SystemExit(main())
