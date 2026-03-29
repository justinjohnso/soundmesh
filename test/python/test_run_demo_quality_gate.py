from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import shutil
import sys
from types import SimpleNamespace
import unittest
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = REPO_ROOT / "tools" / "quality" / "run_demo_quality_gate.py"

spec = importlib.util.spec_from_file_location("run_demo_quality_gate", MODULE_PATH)
assert spec and spec.loader
gate = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = gate
spec.loader.exec_module(gate)  # type: ignore[arg-type]


class RunDemoQualityGateTests(unittest.TestCase):
    def _gate_args(self, output_dir: str, *, transport_node_metrics: str | None = None) -> SimpleNamespace:
        return SimpleNamespace(
            src_port=None,
            out_port=None,
            src_log="src.log",
            out_log="out.log",
            capture_wav="captured.wav",
            reference_wav="reference.wav",
            duration=300,
            sample_rate=48000,
            thresholds="thresholds.json",
            output_dir=output_dir,
            soak_summary="soak.json",
            markdown_output=None,
            transport_node_metrics=transport_node_metrics,
        )

    def _remove_tree(self, path: Path) -> None:
        if path.exists():
            shutil.rmtree(path)

    def test_parse_args_requires_ports_or_soak_summary(self) -> None:
        with self.assertRaises(SystemExit):
            gate.parse_args(
                [
                    "--src-log",
                    "src.log",
                    "--out-log",
                    "out.log",
                    "--capture-wav",
                    "captured.wav",
                    "--reference-wav",
                    "reference.wav",
                ]
            )

    def test_parse_args_accepts_soak_summary_override(self) -> None:
        args = gate.parse_args(
            [
                "--src-log",
                "src.log",
                "--out-log",
                "out.log",
                "--capture-wav",
                "captured.wav",
                "--reference-wav",
                "reference.wav",
                "--soak-summary",
                "soak.json",
            ]
        )
        self.assertEqual(args.duration, 300)
        self.assertEqual(args.soak_summary, "soak.json")
        self.assertIsNone(args.transport_node_metrics)

    def test_evaluate_transport_slos_pass(self) -> None:
        metrics = {
            "join_time_s": 15.0,
            "rejoin_time_s": 12.0,
            "stream_continuity_pct": 99.5,
            "underruns_per_min": 0.2,
            "decode_failures_per_min": 0.1,
            "loss_pct": 0.5,
        }
        result = gate.evaluate_transport_slos(metrics)
        self.assertTrue(result["passed"])
        self.assertEqual(result["failed_metrics"], [])

    def test_evaluate_transport_slos_fail_on_missing_metric(self) -> None:
        metrics = {
            "join_time_s": 15.0,
            "stream_continuity_pct": 99.5,
            "underruns_per_min": 0.2,
            "decode_failures_per_min": 0.1,
            "loss_pct": 0.5,
        }
        result = gate.evaluate_transport_slos(metrics)
        self.assertFalse(result["passed"])
        self.assertIn("rejoin_time_s", result["failed_metrics"])

    def test_evaluate_transport_slos_rejects_boolean_metric_values(self) -> None:
        metrics = {
            "join_time_s": True,
            "rejoin_time_s": 12.0,
            "stream_continuity_pct": 99.5,
            "underruns_per_min": 0.2,
            "decode_failures_per_min": 0.1,
            "loss_pct": 0.5,
        }
        result = gate.evaluate_transport_slos(metrics)
        self.assertFalse(result["passed"])
        self.assertIn("join_time_s", result["failed_metrics"])

    def test_evaluate_quality_gate_requires_pass_band(self) -> None:
        quality_metrics = {
            "threshold_evaluation": {
                "overall_band": "warn",
                "failed_metrics": [],
                "warned_metrics": ["quality_score"],
            }
        }
        result = gate.evaluate_quality_gate(quality_metrics)
        self.assertFalse(result["passed"])
        self.assertEqual(result["overall_band"], "warn")

    def test_evaluate_soak_gate_detects_issues(self) -> None:
        soak_payload = {
            "result": "FAIL",
            "nodes": {
                "SRC": {
                    "open_error": None,
                    "panic_hits": 1,
                    "late_reset_hits": 0,
                    "ok_hits": 12,
                },
                "OUT": {
                    "open_error": None,
                    "panic_hits": 0,
                    "late_reset_hits": 0,
                    "ok_hits": 0,
                },
            },
        }
        result = gate.evaluate_soak_gate(soak_payload, command_exit_code=2)
        self.assertFalse(result["passed"])
        self.assertIn("hil_soak_check.py exit code 2", result["issues"])
        self.assertIn("SRC panic_hits=1", result["issues"])
        self.assertIn("OUT ok_hits=0", result["issues"])

    def test_run_command_wraps_oserror_with_context(self) -> None:
        with mock.patch.object(gate.subprocess, "run", side_effect=OSError("permission denied")):
            with self.assertRaises(RuntimeError) as ctx:
                gate.run_command(["python", "missing.py"], label="sample")
        self.assertIn("sample failed to start", str(ctx.exception))
        self.assertIn("permission denied", str(ctx.exception))

    def test_read_json_file_surfaces_parse_failures_with_label(self) -> None:
        fake_path = Path("docs/quality/reports/not-json.txt")
        with mock.patch.object(Path, "read_text", return_value="{oops"):
            with self.assertRaises(RuntimeError) as ctx:
                gate.read_json_file(fake_path, "transport metrics")
        message = str(ctx.exception)
        self.assertIn("transport metrics", message)
        self.assertIn(str(fake_path), message)

    def test_validate_transport_sanity_fields_detects_mismatches(self) -> None:
        payload = {
            "raw": {
                "line_count": 7,
                "src_line_count": 2,
                "out_line_count": 4,
                "join_time_source": "mystery",
                "src_join_events": 1,
                "out_join_events": 1,
                "rejoin_events": 0,
                "underruns": 1,
                "decode_failures": 0,
                "reason201_count": 0,
                "buf0_events": 0,
                "event_counts": {
                    "join_total": 5,
                    "rejoin_events": 0,
                    "underruns": 2,
                    "decode_failures": 0,
                    "reason201_count": 0,
                    "buf0_events": 0,
                },
            }
        }
        issues = gate.validate_transport_sanity_fields(payload)
        self.assertGreaterEqual(len(issues), 3)
        self.assertTrue(any("line_count mismatch" in issue for issue in issues))
        self.assertTrue(any("join_time_source is invalid" in issue for issue in issues))
        self.assertTrue(any("event_counts.join_total mismatch" in issue for issue in issues))

    def test_validate_transport_metrics_payload_requires_slo_fields(self) -> None:
        payload = {"join_time_s": 1.0, "raw": {}}
        issues = gate.validate_transport_metrics_payload(payload)
        self.assertIn("transport metric missing: rejoin_time_s", issues)
        self.assertIn("transport metric missing: loss_pct", issues)

    def test_validate_output_quality_payload_detects_missing_threshold_evaluation(self) -> None:
        issues = gate.validate_output_quality_payload({"quality_score": 88.0})
        self.assertIn("output quality missing threshold_evaluation object", issues)

    def test_validate_soak_summary_payload_requires_required_fields(self) -> None:
        issues = gate.validate_soak_summary_payload({"result": "PASS", "nodes": {"SRC": {"ok_hits": 1}}})
        self.assertIn("soak summary SRC.panic_hits must be a non-negative integer", issues)
        self.assertIn("soak summary missing node payload for OUT", issues)

    def test_extract_transport_node_metrics_supports_dict_and_metrics_field(self) -> None:
        payload = {
            "nodes": {
                "OUT_A": {"metrics": {"join_time_s": 1.0}},
                "OUT_B": {"join_time_s": 3.0},
            }
        }
        node_metrics = gate.extract_transport_node_metrics(payload)
        self.assertEqual(node_metrics["OUT_A"]["join_time_s"], 1.0)
        self.assertEqual(node_metrics["OUT_B"]["join_time_s"], 3.0)

    def test_select_worst_transport_candidate_picks_failing_node(self) -> None:
        healthy_metrics = {
            "join_time_s": 5.0,
            "rejoin_time_s": 0.0,
            "stream_continuity_pct": 100.0,
            "underruns_per_min": 0.1,
            "decode_failures_per_min": 0.0,
            "loss_pct": 0.1,
            "raw": {"line_count": 10, "src_line_count": 5, "out_line_count": 5},
        }
        failing_metrics = {
            "join_time_s": 5.0,
            "rejoin_time_s": 0.0,
            "stream_continuity_pct": 100.0,
            "underruns_per_min": 5.0,
            "decode_failures_per_min": 0.0,
            "loss_pct": 0.1,
            "raw": {"line_count": 10, "src_line_count": 5, "out_line_count": 5},
        }
        aggregate = gate.evaluate_transport_candidate("aggregate", healthy_metrics)
        node = gate.evaluate_transport_candidate("node:OUT_2", failing_metrics)
        selected = gate.select_worst_transport_candidate([aggregate, node])
        self.assertEqual(selected["candidate"], "node:OUT_2")
        self.assertIn("underruns_per_min", selected["failed_metrics"])

    def test_read_required_json_artifact_rejects_empty_file(self) -> None:
        path = Path("docs/quality/reports/empty-artifact.json")
        mocked_stat = SimpleNamespace(st_size=0)
        with (
            mock.patch.object(Path, "exists", return_value=True),
            mock.patch.object(Path, "is_file", return_value=True),
            mock.patch.object(Path, "stat", return_value=mocked_stat),
        ):
            payload, error = gate.read_required_json_artifact(path, "transport metrics")
        self.assertEqual(payload, {})
        self.assertIn("output is empty", error or "")

    def test_main_fails_when_transport_artifact_is_missing_with_zero_exit(self) -> None:
        output_dir = Path("test/python/.gate-main-missing-transport")
        self._remove_tree(output_dir)

        args = self._gate_args(str(output_dir))
        soak_payload = {
            "result": "PASS",
            "nodes": {
                "SRC": {"ok_hits": 1, "panic_hits": 0, "late_reset_hits": 0},
                "OUT": {"ok_hits": 1, "panic_hits": 0, "late_reset_hits": 0},
            },
        }
        quality_payload = {
            "quality_score": 90.0,
            "threshold_evaluation": {
                "overall_band": "pass",
                "failed_metrics": [],
                "warned_metrics": [],
            },
        }

        with (
            mock.patch.object(gate, "parse_args", return_value=args),
            mock.patch.object(gate, "require_existing_file", return_value=None),
            mock.patch.object(gate, "run_command", side_effect=[0, 0]),
            mock.patch.object(gate, "resolve_path", side_effect=lambda raw_path: Path(raw_path)),
            mock.patch.object(
                gate,
                "read_required_json_artifact",
                side_effect=[
                    (soak_payload, None),
                    ({}, "transport metrics output missing: fake"),
                    (quality_payload, None),
                ],
            ),
        ):
            exit_code = gate.main([])

        self.assertEqual(exit_code, 2)
        summary = json.loads((output_dir / "demo-quality-gate-summary.json").read_text(encoding="utf-8"))
        self.assertFalse(summary["gates"]["transport_slo"]["passed"])
        self.assertIn("transport metrics output missing: fake", summary["gates"]["transport_slo"]["issues"])
        self.assertEqual(summary["final_verdict"]["result"], "FAIL")
        self._remove_tree(output_dir)

    def test_main_uses_worst_candidate_when_node_metrics_provided(self) -> None:
        output_dir = Path("test/python/.gate-main-worst-node")
        self._remove_tree(output_dir)

        args = self._gate_args(str(output_dir), transport_node_metrics="node-metrics.json")
        soak_payload = {
            "result": "PASS",
            "nodes": {
                "SRC": {"ok_hits": 1, "panic_hits": 0, "late_reset_hits": 0},
                "OUT": {"ok_hits": 1, "panic_hits": 0, "late_reset_hits": 0},
            },
        }
        aggregate_metrics = {
            "join_time_s": 10.0,
            "rejoin_time_s": 0.0,
            "stream_continuity_pct": 100.0,
            "underruns_per_min": 0.1,
            "decode_failures_per_min": 0.0,
            "loss_pct": 0.5,
            "raw": {"line_count": 10, "src_line_count": 5, "out_line_count": 5},
        }
        node_metrics = {
            "nodes": {
                "OUT_A": {
                    "metrics": {
                        "join_time_s": 12.0,
                        "rejoin_time_s": 0.0,
                        "stream_continuity_pct": 100.0,
                        "underruns_per_min": 0.2,
                        "decode_failures_per_min": 0.0,
                        "loss_pct": 0.4,
                        "raw": {"line_count": 10, "src_line_count": 5, "out_line_count": 5},
                    }
                },
                "OUT_B": {
                    "metrics": {
                        "join_time_s": 12.0,
                        "rejoin_time_s": 0.0,
                        "stream_continuity_pct": 100.0,
                        "underruns_per_min": 0.2,
                        "decode_failures_per_min": 0.0,
                        "loss_pct": 5.0,
                        "raw": {"line_count": 10, "src_line_count": 5, "out_line_count": 5},
                    }
                },
            }
        }
        quality_payload = {
            "quality_score": 90.0,
            "threshold_evaluation": {
                "overall_band": "pass",
                "failed_metrics": [],
                "warned_metrics": [],
            },
        }

        with (
            mock.patch.object(gate, "parse_args", return_value=args),
            mock.patch.object(gate, "require_existing_file", return_value=None),
            mock.patch.object(gate, "run_command", side_effect=[0, 0]),
            mock.patch.object(gate, "resolve_path", side_effect=lambda raw_path: Path(raw_path)),
            mock.patch.object(
                gate,
                "read_required_json_artifact",
                side_effect=[
                    (soak_payload, None),
                    (aggregate_metrics, None),
                    (node_metrics, None),
                    (quality_payload, None),
                ],
            ),
        ):
            exit_code = gate.main([])

        self.assertEqual(exit_code, 2)
        summary = json.loads((output_dir / "demo-quality-gate-summary.json").read_text(encoding="utf-8"))
        transport = summary["gates"]["transport_slo"]
        self.assertEqual(transport["selected_candidate"], "node:OUT_B")
        self.assertIn("loss_pct", transport["failed_metrics"])
        self.assertEqual(summary["final_verdict"]["result"], "FAIL")
        self._remove_tree(output_dir)


if __name__ == "__main__":
    unittest.main()
