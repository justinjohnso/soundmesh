#!/usr/bin/env python3
"""Score captured output quality against a reference WAV."""

from __future__ import annotations

import argparse
import json
import math
import wave
from dataclasses import dataclass
from pathlib import Path
from typing import Any

try:
    import numpy as np
except Exception as exc:  # pragma: no cover
    raise SystemExit(
        "numpy is required for score_output_quality.py. "
        "Install it with: python -m pip install numpy"
    ) from exc


@dataclass
class ScoringConfig:
    reference_wav: Path
    captured_wav: Path
    analysis_sample_rate: int
    output_json: Path | None
    estimate_alignment: bool
    thresholds_path: Path | None


DEFAULT_THRESHOLDS_PATH = Path(__file__).resolve().with_name("output_quality_thresholds.json")


def _pcm_to_float(samples: bytes, sample_width: int) -> np.ndarray:
    if sample_width == 1:
        arr = np.frombuffer(samples, dtype=np.uint8).astype(np.float32)
        return (arr - 128.0) / 128.0
    if sample_width == 2:
        arr = np.frombuffer(samples, dtype="<i2").astype(np.float32)
        return arr / 32768.0
    if sample_width == 3:
        raw = np.frombuffer(samples, dtype=np.uint8).reshape(-1, 3)
        vals = (
            raw[:, 0].astype(np.int32)
            | (raw[:, 1].astype(np.int32) << 8)
            | (raw[:, 2].astype(np.int32) << 16)
        )
        sign = vals & 0x800000
        vals = vals - (sign << 1)
        return vals.astype(np.float32) / 8388608.0
    if sample_width == 4:
        arr = np.frombuffer(samples, dtype="<i4").astype(np.float32)
        return arr / 2147483648.0
    raise ValueError(f"Unsupported PCM sample width: {sample_width}")


def load_wav_mono(path: Path) -> tuple[np.ndarray, int]:
    with wave.open(str(path), "rb") as wav:
        channels = wav.getnchannels()
        sample_width = wav.getsampwidth()
        sample_rate = wav.getframerate()
        if wav.getcomptype() != "NONE":
            raise ValueError(f"{path} must be uncompressed PCM WAV")
        frames = wav.readframes(wav.getnframes())

    data = _pcm_to_float(frames, sample_width)
    if channels > 1:
        data = data.reshape(-1, channels).mean(axis=1)
    return data.astype(np.float32), sample_rate


def resample_linear(signal: np.ndarray, src_rate: int, dst_rate: int) -> np.ndarray:
    if src_rate == dst_rate:
        return signal
    if signal.size == 0:
        return signal
    duration = signal.size / src_rate
    target_len = max(1, int(round(duration * dst_rate)))
    src_x = np.linspace(0.0, duration, num=signal.size, endpoint=False)
    dst_x = np.linspace(0.0, duration, num=target_len, endpoint=False)
    return np.interp(dst_x, src_x, signal).astype(np.float32)


def sliding_rms(signal: np.ndarray, window_samples: int, hop_samples: int) -> np.ndarray:
    if window_samples <= 0 or hop_samples <= 0:
        raise ValueError("window_samples and hop_samples must be > 0")
    if signal.size < window_samples:
        return np.array([], dtype=np.float32)
    out = []
    for start in range(0, signal.size - window_samples + 1, hop_samples):
        window = signal[start : start + window_samples]
        out.append(float(np.sqrt(np.mean(window * window) + 1e-12)))
    return np.array(out, dtype=np.float32)


def estimate_alignment_offset_samples(
    reference: np.ndarray, captured: np.ndarray, max_lag_samples: int
) -> int:
    if reference.size == 0 or captured.size == 0:
        return 0
    n = min(reference.size, captured.size)
    ref = reference[:n] - np.mean(reference[:n])
    cap = captured[:n] - np.mean(captured[:n])
    corr = np.correlate(cap, ref, mode="full")
    lags = np.arange(-n + 1, n)
    mask = (lags >= -max_lag_samples) & (lags <= max_lag_samples)
    if not np.any(mask):
        return 0
    valid_corr = corr[mask]
    valid_lags = lags[mask]
    return int(valid_lags[int(np.argmax(valid_corr))])


def apply_offset(signal: np.ndarray, offset_samples: int) -> np.ndarray:
    if offset_samples == 0:
        return signal
    if offset_samples > 0:
        return signal[offset_samples:]
    pad = np.zeros((-offset_samples,), dtype=signal.dtype)
    return np.concatenate([pad, signal])


def compute_dropout_ratio_pct(
    reference: np.ndarray,
    captured: np.ndarray,
    sample_rate: int,
    window_ms: float = 20.0,
) -> float:
    window_samples = max(1, int(sample_rate * window_ms / 1000.0))
    hop_samples = window_samples
    n = min(reference.size, captured.size)
    ref_rms = sliding_rms(reference[:n], window_samples, hop_samples)
    cap_rms = sliding_rms(captured[:n], window_samples, hop_samples)
    if ref_rms.size == 0:
        return 100.0

    ref_active_threshold = max(1e-4, float(np.max(ref_rms)) * 0.1)
    cap_silence_threshold = 2e-3
    active = ref_rms >= ref_active_threshold
    if not np.any(active):
        return 0.0
    drops = active & (cap_rms < cap_silence_threshold)
    return float(np.sum(drops) / np.sum(active) * 100.0)


def compute_lag_stats_ms(
    reference: np.ndarray,
    captured: np.ndarray,
    sample_rate: int,
    frame_ms: float = 100.0,
    hop_ms: float = 50.0,
    max_lag_ms: float = 120.0,
) -> tuple[float, float]:
    frame = max(1, int(sample_rate * frame_ms / 1000.0))
    hop = max(1, int(sample_rate * hop_ms / 1000.0))
    max_lag = max(1, int(sample_rate * max_lag_ms / 1000.0))
    n = min(reference.size, captured.size)

    lags_ms: list[float] = []
    for start in range(0, n - frame + 1, hop):
        ref = reference[start : start + frame]
        cap = captured[start : start + frame]
        ref_energy = float(np.sqrt(np.mean(ref * ref) + 1e-12))
        if ref_energy < 1e-4:
            continue

        ref_z = ref - np.mean(ref)
        cap_z = cap - np.mean(cap)
        denom = (np.linalg.norm(ref_z) * np.linalg.norm(cap_z)) + 1e-12
        corr = np.correlate(cap_z, ref_z, mode="full") / denom
        corr_lags = np.arange(-frame + 1, frame)
        mask = (corr_lags >= -max_lag) & (corr_lags <= max_lag)
        if not np.any(mask):
            continue
        best_lag = int(corr_lags[mask][int(np.argmax(corr[mask]))])
        lags_ms.append(best_lag * 1000.0 / sample_rate)

    if not lags_ms:
        return 0.0, 0.0
    lags = np.array(lags_ms, dtype=np.float32)
    return float(np.mean(lags)), float(np.std(lags))


def stft_magnitude(signal: np.ndarray, n_fft: int, hop: int) -> np.ndarray:
    if signal.size < n_fft:
        return np.empty((0, n_fft // 2 + 1), dtype=np.float32)
    window = np.hanning(n_fft).astype(np.float32)
    frames = []
    for start in range(0, signal.size - n_fft + 1, hop):
        frame = signal[start : start + n_fft] * window
        spec = np.fft.rfft(frame)
        frames.append(np.abs(spec).astype(np.float32))
    if not frames:
        return np.empty((0, n_fft // 2 + 1), dtype=np.float32)
    return np.stack(frames)


def compute_spectral_deviation_db(reference: np.ndarray, captured: np.ndarray) -> float:
    n = min(reference.size, captured.size)
    ref_mag = stft_magnitude(reference[:n], n_fft=1024, hop=256)
    cap_mag = stft_magnitude(captured[:n], n_fft=1024, hop=256)
    frames = min(ref_mag.shape[0], cap_mag.shape[0])
    if frames == 0:
        return float("inf")

    eps = 1e-8
    ref_db = 20.0 * np.log10(ref_mag[:frames] + eps)
    cap_db = 20.0 * np.log10(cap_mag[:frames] + eps)
    return float(np.mean(np.abs(cap_db - ref_db)))


def compute_quality_score(dropout_ratio_pct: float, lag_ms_mean: float, lag_ms_std: float, spectral_deviation_db: float) -> float:
    dropout_component = max(0.0, 100.0 - (dropout_ratio_pct * 2.5))
    lag_penalty_ms = abs(lag_ms_mean) + lag_ms_std
    lag_component = max(0.0, 100.0 - (lag_penalty_ms * 2.0))
    spectral_component = max(0.0, 100.0 - (spectral_deviation_db * 5.0))
    score = (0.45 * dropout_component) + (0.35 * lag_component) + (0.20 * spectral_component)
    return round(float(np.clip(score, 0.0, 100.0)), 2)


def _metric_band(metric_name: str, value: float | None, threshold_spec: dict[str, Any]) -> str:
    if value is None:
        return "fail"
    direction = threshold_spec.get("direction")
    if direction == "lower_is_better":
        pass_max = threshold_spec.get("pass_max")
        warn_max = threshold_spec.get("warn_max")
        if not isinstance(pass_max, (int, float)) or not isinstance(warn_max, (int, float)):
            raise ValueError(f"Invalid lower_is_better threshold config for {metric_name}")
        if value <= float(pass_max):
            return "pass"
        if value <= float(warn_max):
            return "warn"
        return "fail"
    if direction == "higher_is_better":
        pass_min = threshold_spec.get("pass_min")
        warn_min = threshold_spec.get("warn_min")
        if not isinstance(pass_min, (int, float)) or not isinstance(warn_min, (int, float)):
            raise ValueError(f"Invalid higher_is_better threshold config for {metric_name}")
        if value >= float(pass_min):
            return "pass"
        if value >= float(warn_min):
            return "warn"
        return "fail"
    raise ValueError(f"Unknown threshold direction for {metric_name}: {direction}")


def evaluate_quality_bands(result: dict[str, Any], thresholds: dict[str, Any]) -> dict[str, Any]:
    metric_specs = thresholds.get("metrics")
    if not isinstance(metric_specs, dict):
        raise ValueError("Threshold config missing 'metrics' object")

    metric_bands: dict[str, str] = {}
    for metric_name in ("dropout_ratio_pct", "lag_ms_std", "spectral_deviation_db", "quality_score"):
        if metric_name not in metric_specs:
            raise ValueError(f"Threshold config missing metric: {metric_name}")
        metric_value = result.get(metric_name)
        metric_bands[metric_name] = _metric_band(metric_name, metric_value, metric_specs[metric_name])

    rank = {"fail": 0, "warn": 1, "pass": 2}
    overall_band = min(metric_bands.values(), key=lambda band: rank[band])
    failed_metrics = [name for name, band in metric_bands.items() if band == "fail"]
    warned_metrics = [name for name, band in metric_bands.items() if band == "warn"]

    return {
        "overall_band": overall_band,
        "metric_bands": metric_bands,
        "failed_metrics": failed_metrics,
        "warned_metrics": warned_metrics,
    }


def load_thresholds(path: Path | None) -> dict[str, Any] | None:
    candidate = path if path is not None else DEFAULT_THRESHOLDS_PATH
    if not candidate.exists():
        return None
    return json.loads(candidate.read_text())


def score_quality(config: ScoringConfig) -> dict:
    reference, ref_sr = load_wav_mono(config.reference_wav)
    captured, cap_sr = load_wav_mono(config.captured_wav)

    if config.analysis_sample_rate <= 0:
        raise ValueError("analysis_sample_rate must be > 0")

    reference = resample_linear(reference, ref_sr, config.analysis_sample_rate)
    captured = resample_linear(captured, cap_sr, config.analysis_sample_rate)

    estimated_offset_samples = 0
    if config.estimate_alignment:
        estimated_offset_samples = estimate_alignment_offset_samples(
            reference, captured, max_lag_samples=int(config.analysis_sample_rate * 0.5)
        )
        captured = apply_offset(captured, estimated_offset_samples)

    n = min(reference.size, captured.size)
    reference = reference[:n]
    captured = captured[:n]

    dropout_ratio_pct = compute_dropout_ratio_pct(reference, captured, config.analysis_sample_rate)
    lag_ms_mean, lag_ms_std = compute_lag_stats_ms(reference, captured, config.analysis_sample_rate)
    spectral_deviation_db = compute_spectral_deviation_db(reference, captured)
    quality_score = compute_quality_score(dropout_ratio_pct, lag_ms_mean, lag_ms_std, spectral_deviation_db)

    result = {
        "reference_wav": str(config.reference_wav),
        "captured_wav": str(config.captured_wav),
        "analysis_sample_rate_hz": config.analysis_sample_rate,
        "alignment_offset_samples": estimated_offset_samples,
        "alignment_offset_ms": round(estimated_offset_samples * 1000.0 / config.analysis_sample_rate, 3),
        "dropout_ratio_pct": round(dropout_ratio_pct, 3),
        "lag_ms_mean": round(lag_ms_mean, 3),
        "lag_ms_std": round(lag_ms_std, 3),
        "spectral_deviation_db": round(spectral_deviation_db, 3) if math.isfinite(spectral_deviation_db) else None,
        "quality_score": quality_score,
        "score_components": {
            "dropout_weight": 0.45,
            "lag_weight": 0.35,
            "spectral_weight": 0.20,
        },
    }
    thresholds = load_thresholds(config.thresholds_path)
    if thresholds is not None:
        result["thresholds_path"] = str(config.thresholds_path or DEFAULT_THRESHOLDS_PATH)
        result["threshold_evaluation"] = evaluate_quality_bands(result, thresholds)
    return result


def parse_args() -> ScoringConfig:
    parser = argparse.ArgumentParser(description="Score captured audio against a reference WAV")
    parser.add_argument("--reference", required=True, help="Reference WAV path")
    parser.add_argument("--captured", required=True, help="Captured WAV path")
    parser.add_argument("--sample-rate", type=int, default=48000, help="Analysis sample rate")
    parser.add_argument("--output", default=None, help="Optional output JSON path")
    parser.add_argument(
        "--thresholds",
        default=None,
        help=(
            "Optional thresholds JSON path for pass/warn/fail band evaluation. "
            "If omitted, defaults to tools/quality/output_quality_thresholds.json when present."
        ),
    )
    parser.add_argument(
        "--no-alignment",
        action="store_true",
        help="Disable alignment offset estimation before metric computation",
    )
    args = parser.parse_args()

    return ScoringConfig(
        reference_wav=Path(args.reference),
        captured_wav=Path(args.captured),
        analysis_sample_rate=int(args.sample_rate),
        output_json=Path(args.output) if args.output else None,
        estimate_alignment=not args.no_alignment,
        thresholds_path=Path(args.thresholds) if args.thresholds else None,
    )


def main() -> int:
    cfg = parse_args()
    if not cfg.reference_wav.exists():
        print(f"score_output_quality.py error: missing reference WAV: {cfg.reference_wav}")
        return 2
    if not cfg.captured_wav.exists():
        print(f"score_output_quality.py error: missing captured WAV: {cfg.captured_wav}")
        return 2

    try:
        result = score_quality(cfg)
    except Exception as exc:
        print(f"score_output_quality.py error: {exc}")
        return 2

    payload = json.dumps(result, indent=2)
    if cfg.output_json:
        cfg.output_json.parent.mkdir(parents=True, exist_ok=True)
        cfg.output_json.write_text(payload + "\n")
    print(payload)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
