#!/usr/bin/env python3
"""Build external plugin benchmark passes from rendered WAV files.

This script lets you compare third-party transpose plugins (e.g. HyperTune,
Archetype) inside the existing transpose benchmark report.

Plugin metadata (id/label/path) is loaded from a shared plugin collection JSON
by default (tools/transpose-benchmark/external-plugins.json).

Input render-manifest schema:
{
    "pluginId": "hypertune_vst3",
    "entries": [
        {
            "sample": "DI_Guitar_L.mp3",
            "semitones": -12,
            "wav": "external/HyperTune_DI_m12.wav",
            "reportedLatencySamples": 0
        }
    ]
}

The script reads dry references from <snapshot_dir>/results.json references and
writes <snapshot_dir>/external-passes-<pluginId>.json.
"""

from __future__ import annotations

import argparse
import json
import math
import wave
from pathlib import Path

ENVELOPE_DECIMATION = 32
MAX_MEASURED_LATENCY_SECONDS = 0.5


def read_wav_stereo(path: Path) -> tuple[float, list[float], list[float]]:
    with wave.open(str(path), "rb") as wf:
        channels = wf.getnchannels()
        sample_width = wf.getsampwidth()
        sample_rate = float(wf.getframerate())
        frame_count = wf.getnframes()
        raw = wf.readframes(frame_count)

    if sample_width not in (2, 3, 4):
        raise ValueError(f"Unsupported sample width in {path}: {sample_width}")
    if channels < 1:
        raise ValueError(f"No channels in {path}")

    left: list[float] = []
    right: list[float] = []

    if sample_width == 2:
        import struct

        ints = struct.unpack("<" + "h" * (len(raw) // 2), raw)
        scale = 32768.0
        for i in range(0, len(ints), channels):
            l = ints[i] / scale
            r = ints[i + 1] / scale if channels > 1 else l
            left.append(float(l))
            right.append(float(r))
    elif sample_width == 3:
        step = channels * 3
        for i in range(0, len(raw), step):
            base = i
            l = int.from_bytes(raw[base : base + 3] + (b"\x00" if raw[base + 2] < 0x80 else b"\xff"), "little", signed=True)
            if channels > 1:
                base_r = i + 3
                r = int.from_bytes(raw[base_r : base_r + 3] + (b"\x00" if raw[base_r + 2] < 0x80 else b"\xff"), "little", signed=True)
            else:
                r = l
            left.append(float(l / 8388608.0))
            right.append(float(r / 8388608.0))
    else:
        import struct

        ints = struct.unpack("<" + "i" * (len(raw) // 4), raw)
        scale = 2147483648.0
        for i in range(0, len(ints), channels):
            l = ints[i] / scale
            r = ints[i + 1] / scale if channels > 1 else l
            left.append(float(l))
            right.append(float(r))

    return sample_rate, left, right


def compute_envelope(left: list[float], right: list[float], sample_rate: float) -> list[float]:
    smoothing_seconds = 0.005
    alpha = 1.0 - math.exp(-1.0 / (smoothing_seconds * sample_rate))
    state = 0.0
    env: list[float] = []
    for i, (l, r) in enumerate(zip(left, right)):
        mono = 0.5 * (abs(float(l)) + abs(float(r)))
        state += alpha * (mono - state)
        if i % ENVELOPE_DECIMATION == 0:
            env.append(state)
    return env


def measure_latency_frames(
    input_left: list[float],
    input_right: list[float],
    output_left: list[float],
    output_right: list[float],
    sample_rate: float,
) -> int | None:
    input_env = compute_envelope(input_left, input_right, sample_rate)
    output_env = compute_envelope(output_left, output_right, sample_rate)

    if len(input_env) < 16 or len(output_env) < 16:
        return None

    max_lag = int(MAX_MEASURED_LATENCY_SECONDS * sample_rate) // ENVELOPE_DECIMATION
    usable_lag = min(max_lag, len(output_env) - 8)
    if usable_lag <= 0:
        return None

    input_energy = sum(v * v for v in input_env)
    if input_energy <= 1.0e-12:
        return None

    best_lag = 0
    best_score = -1.0
    for lag in range(usable_lag + 1):
        count = min(len(input_env), len(output_env) - lag)
        dot = 0.0
        out_energy = 0.0
        for i in range(count):
            o = output_env[i + lag]
            dot += input_env[i] * o
            out_energy += o * o
        if out_energy <= 1.0e-12:
            continue
        score = dot / math.sqrt(input_energy * out_energy)
        if score > best_score:
            best_score = score
            best_lag = lag

    if best_score < 0.5:
        return None
    return best_lag * ENVELOPE_DECIMATION


def db(value: float) -> float:
    return 20.0 * math.log10(value) if value > 1.0e-9 else -180.0


def ms_from_samples(samples: int | None, sample_rate: float) -> float | None:
    if samples is None:
        return None
    return 1000.0 * float(samples) / sample_rate


def sanitize_id(value: str) -> str:
    cleaned = "".join(ch if (ch.isalnum() or ch in ("-", "_")) else "_" for ch in value.strip().lower())
    return cleaned or "external"


def load_plugin_collection(path: Path) -> dict[str, dict]:
    if not path.is_file():
        return {}
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}

    plugins = payload.get("plugins", []) if isinstance(payload, dict) else []
    if not isinstance(plugins, list):
        return {}

    by_id: dict[str, dict] = {}
    for row in plugins:
        if not isinstance(row, dict):
            continue
        plugin_id = row.get("pluginId")
        if not isinstance(plugin_id, str) or not plugin_id.strip():
            continue
        by_id[sanitize_id(plugin_id)] = row
    return by_id


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("snapshot_dir", type=Path, help="Snapshot directory containing results.json")
    parser.add_argument("manifest", type=Path, help="Render manifest JSON file")
    parser.add_argument(
        "--collection",
        type=Path,
        default=(Path(__file__).resolve().parent / "external-plugins.json"),
        help="Plugin collection JSON (default: tools/transpose-benchmark/external-plugins.json)",
    )
    parser.add_argument("--output", type=Path, default=None, help="Output JSON path (default: external-passes-<pluginId>.json in snapshot dir)")
    args = parser.parse_args()

    results_path = args.snapshot_dir / "results.json"
    if not results_path.is_file():
        print(f"ERROR: missing {results_path}")
        return 1
    if not args.manifest.is_file():
        print(f"ERROR: missing {args.manifest}")
        return 1

    results = json.loads(results_path.read_text(encoding="utf-8"))
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    if not isinstance(manifest, dict):
        print("ERROR: manifest root must be an object")
        return 1

    collection = load_plugin_collection(args.collection)

    plugin_id_raw = manifest.get("pluginId")
    if isinstance(plugin_id_raw, str) and plugin_id_raw.strip():
        plugin_id = sanitize_id(plugin_id_raw)
    else:
        plugin_id = sanitize_id(str(manifest.get("pluginLabel", "external")))

    plugin_info = collection.get(plugin_id, {})
    plugin_label = str(manifest.get("pluginLabel") or plugin_info.get("pluginLabel") or "External Plugin")
    plugin_path = manifest.get("pluginPath") or plugin_info.get("pluginPath")
    if not isinstance(plugin_path, str) or not plugin_path.strip():
        plugin_path = None
    entries = manifest.get("entries", [])
    if not isinstance(entries, list) or not entries:
        print("ERROR: manifest.entries must be a non-empty list")
        return 1

    refs = {}
    for ref in results.get("references", []):
        sample = ref.get("sample")
        wav = ref.get("wav")
        if isinstance(sample, str) and isinstance(wav, str):
            refs[sample] = args.snapshot_dir / wav

    out_passes: list[dict] = []
    for row in entries:
        if not isinstance(row, dict):
            continue

        sample = row.get("sample")
        semitones = row.get("semitones")
        wav_rel = row.get("wav")
        if not isinstance(sample, str) or not isinstance(semitones, int) or not isinstance(wav_rel, str):
            print(f"WARN: skipping malformed entry: {row}")
            continue

        ref_path = refs.get(sample)
        if ref_path is None or not ref_path.is_file():
            print(f"WARN: missing dry reference for sample '{sample}'")
            continue

        render_path = (args.snapshot_dir / wav_rel)
        if not render_path.is_file():
            print(f"WARN: missing render WAV: {render_path}")
            continue

        ref_sr, ref_l, ref_r = read_wav_stereo(ref_path)
        out_sr, out_l, out_r = read_wav_stereo(render_path)
        if abs(ref_sr - out_sr) > 1e-6:
            print(f"WARN: sample-rate mismatch for {wav_rel}: dry={ref_sr}, render={out_sr}")

        n = min(len(ref_l), len(out_l))
        ref_l = ref_l[:n]
        ref_r = ref_r[:n]
        out_l = out_l[:n]
        out_r = out_r[:n]

        measured = measure_latency_frames(ref_l, ref_r, out_l, out_r, out_sr)
        reported = row.get("reportedLatencySamples")
        if not isinstance(reported, int):
            reported = None

        peak = 0.0
        sum_sq = 0.0
        for l, r in zip(out_l, out_r):
            al = abs(l)
            ar = abs(r)
            peak = max(peak, al, ar)
            sum_sq += 0.5 * (al * al + ar * ar)
        rms = math.sqrt(sum_sq / float(n)) if n > 0 else 0.0

        pass_row: dict = {
            "effect": str(row.get("effect", plugin_id)),
            "effectLabel": str(row.get("effectLabel", plugin_label)),
            "sourcePluginPath": plugin_path,
            "sample": sample,
            "semitones": semitones,
            "sampleRate": out_sr,
            "wav": wav_rel.replace("\\", "/"),
            "reportedLatencySamples": reported,
            "reportedLatencyMs": ms_from_samples(reported, out_sr),
            "measuredLatencySamples": measured,
            "measuredLatencyMs": ms_from_samples(measured, out_sr),
            "latencyDeltaSamples": (measured - reported) if (measured is not None and reported is not None) else None,
            "processMs": row.get("processMs"),
            "audioMs": row.get("audioMs"),
            "realtimeFactor": row.get("realtimeFactor"),
            "avgBlockUs": row.get("avgBlockUs"),
            "maxBlockUs": row.get("maxBlockUs"),
            "peakDb": db(peak),
            "rmsDb": db(rms),
        }
        out_passes.append(pass_row)

    if not out_passes:
        print("ERROR: no valid external passes generated")
        return 1

    payload = {
        "pluginId": plugin_id,
        "pluginLabel": plugin_label,
        "passes": out_passes,
    }

    out_path = args.output or (args.snapshot_dir / f"external-passes-{plugin_id}.json")
    out_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    print(f"Wrote {out_path} ({len(out_passes)} pass(es))")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
