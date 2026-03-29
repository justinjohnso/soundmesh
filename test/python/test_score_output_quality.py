from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import unittest

try:
    import numpy as np
except Exception:  # pragma: no cover
    np = None


REPO_ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = REPO_ROOT / "tools" / "quality" / "score_output_quality.py"
score = None
if np is not None:
    spec = importlib.util.spec_from_file_location("score_output_quality", MODULE_PATH)
    assert spec and spec.loader
    score = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = score
    spec.loader.exec_module(score)  # type: ignore[arg-type]


@unittest.skipIf(np is None, "numpy is required for score_output_quality tests")
class ScoreOutputQualityTests(unittest.TestCase):
    def test_alignment_offset_estimation(self) -> None:
        sr = 48000
        t = np.arange(0, 0.4, 1 / sr)
        ref = np.sin(2 * np.pi * 440.0 * t).astype(np.float32)
        offset = int(0.02 * sr)
        cap = np.concatenate([np.zeros(offset, dtype=np.float32), ref])[: ref.size]

        estimated = score.estimate_alignment_offset_samples(ref, cap, max_lag_samples=int(0.1 * sr))
        self.assertAlmostEqual(estimated, offset, delta=5)

    def test_dropout_ratio_detects_drop(self) -> None:
        sr = 48000
        ref = np.ones(sr, dtype=np.float32) * 0.1
        cap = ref.copy()
        cap[sr // 4 : sr // 2] = 0.0

        ratio = score.compute_dropout_ratio_pct(ref, cap, sr)
        self.assertGreater(ratio, 20.0)

    def test_quality_score_bounds(self) -> None:
        good = score.compute_quality_score(0.0, 1.0, 1.0, 1.0)
        bad = score.compute_quality_score(80.0, 120.0, 80.0, 40.0)

        self.assertGreater(good, bad)
        self.assertGreaterEqual(good, 0.0)
        self.assertLessEqual(good, 100.0)
        self.assertGreaterEqual(bad, 0.0)

    def test_threshold_evaluation_bands(self) -> None:
        thresholds = {
            "metrics": {
                "dropout_ratio_pct": {
                    "direction": "lower_is_better",
                    "pass_max": 1.0,
                    "warn_max": 6.0,
                },
                "lag_ms_std": {
                    "direction": "lower_is_better",
                    "pass_max": 12.0,
                    "warn_max": 25.0,
                },
                "spectral_deviation_db": {
                    "direction": "lower_is_better",
                    "pass_max": 5.0,
                    "warn_max": 10.0,
                },
                "quality_score": {
                    "direction": "higher_is_better",
                    "pass_min": 85.0,
                    "warn_min": 70.0,
                },
            }
        }
        result = {
            "dropout_ratio_pct": 3.5,
            "lag_ms_std": 9.0,
            "spectral_deviation_db": 4.5,
            "quality_score": 77.0,
        }

        evaluation = score.evaluate_quality_bands(result, thresholds)
        self.assertEqual(evaluation["metric_bands"]["dropout_ratio_pct"], "warn")
        self.assertEqual(evaluation["metric_bands"]["lag_ms_std"], "pass")
        self.assertEqual(evaluation["metric_bands"]["spectral_deviation_db"], "pass")
        self.assertEqual(evaluation["metric_bands"]["quality_score"], "warn")
        self.assertEqual(evaluation["overall_band"], "warn")

    def test_threshold_evaluation_overall_fail(self) -> None:
        thresholds = {
            "metrics": {
                "dropout_ratio_pct": {
                    "direction": "lower_is_better",
                    "pass_max": 1.0,
                    "warn_max": 6.0,
                },
                "lag_ms_std": {
                    "direction": "lower_is_better",
                    "pass_max": 12.0,
                    "warn_max": 25.0,
                },
                "spectral_deviation_db": {
                    "direction": "lower_is_better",
                    "pass_max": 5.0,
                    "warn_max": 10.0,
                },
                "quality_score": {
                    "direction": "higher_is_better",
                    "pass_min": 85.0,
                    "warn_min": 70.0,
                },
            }
        }
        result = {
            "dropout_ratio_pct": 10.5,
            "lag_ms_std": 13.0,
            "spectral_deviation_db": 6.5,
            "quality_score": 73.0,
        }

        evaluation = score.evaluate_quality_bands(result, thresholds)
        self.assertEqual(evaluation["metric_bands"]["dropout_ratio_pct"], "fail")
        self.assertEqual(evaluation["overall_band"], "fail")
        self.assertIn("dropout_ratio_pct", evaluation["failed_metrics"])


if __name__ == "__main__":
    unittest.main()
