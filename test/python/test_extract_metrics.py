from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = REPO_ROOT / "tools" / "benchmarks" / "extract_metrics.py"

spec = importlib.util.spec_from_file_location("extract_metrics", MODULE_PATH)
assert spec and spec.loader
metrics_module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = metrics_module
spec.loader.exec_module(metrics_module)  # type: ignore[arg-type]


class ExtractMetricsTests(unittest.TestCase):
    def test_join_time_handles_ansi_and_src_only_join(self) -> None:
        src_log = "\x1b[32mI (1234) network_mesh: Network ready - starting audio transmission\x1b[0m\n"
        out_log = "I (1500) adf_pipeline: Playback started\n"

        metrics = metrics_module.compute_metrics(src_log, out_log, duration_s=60)

        self.assertEqual(metrics["join_time_s"], 1.234)
        self.assertEqual(metrics["raw"]["join_time_source"], "src_only")
        self.assertEqual(metrics["raw"]["src_join_events"], 1)
        self.assertEqual(metrics["raw"]["out_join_events"], 0)

    def test_join_time_handles_out_only_join_variant(self) -> None:
        src_log = "I (1000) wifi: disconnected reason:201\n"
        out_log = "I (4500) network_mesh: Parent connected, layer: 2 (stream ready)\n"

        metrics = metrics_module.compute_metrics(src_log, out_log, duration_s=60)

        self.assertEqual(metrics["join_time_s"], 4.5)
        self.assertEqual(metrics["raw"]["join_time_source"], "out_only")
        self.assertEqual(metrics["raw"]["src_join_events"], 0)
        self.assertEqual(metrics["raw"]["out_join_events"], 1)

    def test_outputs_rate_and_loss_context_metrics(self) -> None:
        src_log = "\n".join(
            [
                "I (100) wifi: disconnected reason:201",
                "I (200) wifi: disconnected reason:201",
            ]
        )
        out_log = "\n".join(
            [
                "I (1000) dashboard: RX: 100 pkts, 1 drops (1.0%), buf=0%",
                "W (1001) adf_pipeline: underrun detected",
                "E (1002) adf_pipeline: Opus decode failed",
                "I (2000) dashboard: RX: 200 pkts, 4 drops (2.0%), buf=50%",
                "I (3000) dashboard: RX: 300 pkts, 9 drops (3.0%), buf=0%",
            ]
        )

        metrics = metrics_module.compute_metrics(src_log, out_log, duration_s=120)

        self.assertEqual(metrics["reason201_per_min"], 1.0)
        self.assertEqual(metrics["buf0_events_per_min"], 1.0)
        self.assertEqual(metrics["loss_pct"], 3.0)
        self.assertEqual(metrics["loss_avg_pct"], 2.0)
        self.assertEqual(metrics["loss_last_pct"], 3.0)
        self.assertEqual(metrics["loss_p95_pct"], 3.0)

    def test_raw_metadata_includes_line_count_and_event_counters(self) -> None:
        src_log = "I (2000) wifi: disconnected reason:201\nI (2010) network_mesh: Root ready:\n"
        out_log = "\n".join(
            [
                "I (3000) network_mesh: MESH_EVENT_PARENT_CONNECTED",
                "I (3010) dashboard: RX: 10 pkts, 0 drops (0.0%), buf=0%",
                "W (3020) adf_pipeline: underrun",
            ]
        )

        metrics = metrics_module.compute_metrics(src_log, out_log, duration_s=60)
        raw = metrics["raw"]

        self.assertEqual(raw["line_count"], 5)
        self.assertEqual(raw["src_line_count"], 2)
        self.assertEqual(raw["out_line_count"], 3)
        self.assertEqual(raw["reason201_count"], 1)
        self.assertEqual(raw["buf0_events"], 1)
        self.assertEqual(raw["event_counts"]["join_total"], 2)
        self.assertEqual(raw["event_counts"]["reason201_count"], 1)
        self.assertEqual(raw["event_counts"]["buf0_events"], 1)

    def test_counts_new_transport_observability_markers(self) -> None:
        src_log = "I (100) network_mesh: Network ready\n"
        out_log = "\n".join(
            [
                "I (200) dashboard: RX OBS: audio=300 fwd=290 dup=11 ttl0=5 inv={hdr:1 ver:2 pay:3} batch={pkts:4 frames:8} cb_miss=1 recv={err:2 empty:3} burst_loss=6 burst_max=9 jitter_us=1200 ctrl={hb:4 ctl:5 ping:6 pong:7 ann:8} churn={pc:9 pd:10 np:11 sc:12 rj:13/14/15}",
                "I (220) dashboard: TX OBS: audio_ok=16 fail=3 qfull=2 noroute=5 inv=7 bp=1",
            ]
        )

        metrics = metrics_module.compute_metrics(src_log, out_log, duration_s=60)
        raw = metrics["raw"]

        self.assertEqual(raw["tx_obs_send_failures"], 3)
        self.assertEqual(raw["tx_obs_queue_full"], 2)
        self.assertEqual(raw["tx_obs_backpressure_level_max"], 1)
        self.assertEqual(raw["tx_obs_audio_ok"], 16)
        self.assertEqual(raw["tx_obs_no_route"], 5)
        self.assertEqual(raw["tx_obs_invalid_state"], 7)
        self.assertEqual(raw["rx_net_duplicates"], 11)
        self.assertEqual(raw["rx_net_ttl_expired"], 5)
        self.assertEqual(raw["rx_net_mesh_recv_errors"], 2)
        self.assertEqual(raw["rx_net_mesh_recv_empty"], 3)
        self.assertEqual(raw["rx_obs_burst_loss_events"], 6)
        self.assertEqual(raw["rx_obs_burst_loss_max"], 9)
        self.assertEqual(raw["rx_obs_jitter_us"], 1200)

    def test_outputs_cadence_and_backpressure_counters(self) -> None:
        src_log = "\n".join(
            [
                "I (1000) wifi: disconnected reason:201",
                "I (4000) wifi: disconnected reason:201",
                "I (7000) wifi: disconnected reason:201",
            ]
        )
        out_log = "\n".join(
            [
                "I (1500) dashboard: RX: 100 pkts, 1 drops (1.0%), buf=0%",
                "W (2000) adf_pipeline: underrun detected",
                "I (4500) dashboard: TX OBS: audio_ok=16 fail=3 qfull=2 noroute=0 inv=0 bp=1",
                "I (5000) dashboard: RX: 200 pkts, 2 drops (2.0%), buf=0%",
                "W (6000) adf_pipeline: underrun detected",
                "I (7500) dashboard: TX OBS: audio_ok=32 fail=3 qfull=2 noroute=0 inv=0 bp=2",
                "I (8500) dashboard: RX: 300 pkts, 3 drops (3.0%), buf=0%",
                "W (10000) adf_pipeline: underrun detected",
            ]
        )

        metrics = metrics_module.compute_metrics(src_log, out_log, duration_s=120)

        self.assertEqual(metrics["reason201_cadence_s"], 3.0)
        self.assertEqual(metrics["buf0_cadence_s"], 3.5)
        self.assertEqual(metrics["underrun_cadence_s"], 4.0)
        self.assertEqual(metrics["tx_backpressure_cadence_s"], 3.0)
        self.assertEqual(metrics["tx_backpressure_nonzero_samples_per_min"], 1.0)
        self.assertEqual(metrics["raw"]["tx_obs_backpressure_nonzero_samples"], 2)


if __name__ == "__main__":
    unittest.main()
