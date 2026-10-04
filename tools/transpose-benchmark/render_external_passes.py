#!/usr/bin/env python3
"""Automatically render external plugin passes using pedalboard VST3 hosting.

Reads plugin definitions from external-plugins.json (or a supplied collection
file), loads each VST3 plugin headlessly, renders each demo audio file at every
configured semitone setting, and writes:

  <snapshot_dir>/external/<pluginId>/<pluginId>_<sample-stem>_<st>.wav
  <snapshot_dir>/auto-manifest-<pluginId>.json

The manifest files are in the same schema as external-renders.example.json so
build_external_passes.py can consume them unchanged.

Requires:
    pip install pedalboard

Usage:
    python render_external_passes.py <snapshot_dir> [--collection <path>]
                                     [--plugin <pluginId> ...]
                                     [--samples <name> ...]
                                     [--block-size <n>]
                                     [--dry-run]
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import traceback
import wave
from pathlib import Path

# Pedalboard is an optional runtime dependency; give a clear message if absent.
try:
    import numpy as np
    import pedalboard
    from pedalboard.io import AudioFile

    _PEDALBOARD_OK = True
except ImportError:
    _PEDALBOARD_OK = False

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

DEFAULT_BLOCK_SIZE = 512
# Maximum scan resolution when searching for a semitone by text representation.
_SCAN_STEPS = 400
# Fallback assumed parameter range when text-scan finds nothing.
_FALLBACK_SEMITONE_MIN = -24
_FALLBACK_SEMITONE_MAX = 24

# Default demo audio relative to the repo root (resolved via snapshot_dir ancestry).
_DEFAULT_SAMPLES = [
    "guitar-riff-01.wav",
    "guitar-riff-02.wav",
    "DI_Guitar_L.mp3",
]


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _find_repo_root(start: Path) -> Path | None:
    """Walk up from *start* looking for a .git directory."""
    p = start.resolve()
    for candidate in [p, *p.parents]:
        if (candidate / ".git").exists():
            return candidate
    return None


def _semitone_tag(semitones: int) -> str:
    """Return a compact tag like 'm12', 'p7', '0' for file names."""
    if semitones < 0:
        return f"m{abs(semitones)}"
    if semitones > 0:
        return f"p{semitones}"
    return "0"


def _extract_int(text: str) -> int | None:
    """Extract the first integer from *text*, or None."""
    m = re.search(r"-?\d+", text.strip())
    return int(m.group()) if m else None


def _normalize_param_key(name: str) -> str:
    """Normalize a parameter name for fuzzy matching.

    Pedalboard exposes VST3 params as snake_case identifiers (e.g. ``gate_active``)
    while JUCE / DAW UIs use display titles (e.g. ``Gate Active``). Collapse both
    to the same alphanumeric fingerprint.
    """
    out: list[str] = []
    for ch in name.strip().lower():
        if ch.isalnum():
            out.append(ch)
        else:
            # spaces, underscores, slashes, parentheses, etc. → separator
            if out and out[-1] != "_":
                out.append("_")
    # Trim trailing separators.
    while out and out[-1] == "_":
        out.pop()
    return "".join(out)


def _resolve_parameter_key(plugin, requested_name: str) -> str | None:
    """Resolve a plugin parameter name, case-insensitively / snake_case-aware."""
    params = plugin.parameters
    if requested_name in params:
        return requested_name

    requested_lower = requested_name.strip().lower()
    for key in params.keys():
        if str(key).strip().lower() == requested_lower:
            return str(key)

    # Fuzzy: "Gate Active" ↔ "gate_active", "Cab Type (Unlinked)" ↔ "cab_type_unlinked"
    requested_norm = _normalize_param_key(requested_name)
    if requested_norm:
        for key in params.keys():
            if _normalize_param_key(str(key)) == requested_norm:
                return str(key)
    return None


# ---------------------------------------------------------------------------
# Parameter value resolution
# ---------------------------------------------------------------------------


def _get_param_string(param) -> str | None:  # type: ignore[type-arg]
    """Return the display string for a pedalboard parameter, if available."""
    # pedalboard >= 0.9 exposes `.string_value`; earlier versions may not.
    if hasattr(param, "string_value"):
        try:
            return str(param.string_value)
        except Exception:
            pass
    # Fallback: use repr; strip the numeric raw_value part if present.
    try:
        return str(param)
    except Exception:
        return None


def _scan_text_mapping(plugin, param_name: str, target_semitones: int) -> float | None:
    """Scan parameter values to find the raw value whose display text matches
    *target_semitones*.

    Returns a normalized 0-1 float, or None if no match is found.
    """
    params = plugin.parameters
    resolved_name = _resolve_parameter_key(plugin, param_name)
    if resolved_name is None:
        print(f"    WARN: parameter '{param_name}' not found in plugin")
        _print_available_params(plugin)
        return None

    param = params[resolved_name]
    original_raw = param.raw_value

    found_raw: float | None = None
    seen: dict[int, float] = {}  # semitone -> first raw_value that produced it

    for step in range(_SCAN_STEPS + 1):
        raw = step / _SCAN_STEPS
        try:
            param.raw_value = raw
        except Exception:
            continue

        text = _get_param_string(param)
        if text is None:
            continue

        val = _extract_int(text)
        if val is not None and abs(val) <= 96:  # sanity-check for semitone range
            if val not in seen:
                seen[val] = raw
            if val == target_semitones:
                found_raw = raw
                break

    # Restore original value regardless.
    try:
        param.raw_value = original_raw
    except Exception:
        pass

    if found_raw is not None:
        return found_raw

    if seen:
        print(
            f"    WARN: could not find semitone {target_semitones} in text scan. "
            f"Discovered values: {sorted(seen.keys())}"
        )
    else:
        print(
            f"    WARN: text scan found no parseable semitone strings for '{param_name}'. "
            "Falling back to linear normalized mapping."
        )
        # Linear fallback: assume symmetric range centered at 0.
        span = _FALLBACK_SEMITONE_MAX - _FALLBACK_SEMITONE_MIN
        raw = (target_semitones - _FALLBACK_SEMITONE_MIN) / span
        raw = max(0.0, min(1.0, raw))
        print(f"    INFO: using fallback normalized value {raw:.4f} for {target_semitones} st")
        return raw

    return None


def _resolve_normalized_value(
    plugin, param_name: str, target_semitones: int, mapping: str
) -> float | None:
    """Return the normalized 0-1 parameter value for *target_semitones*."""
    if mapping == "text":
        return _scan_text_mapping(plugin, param_name, target_semitones)

    if mapping == "normalized":
        # Caller must have stored the expected range in transposeParameter; fall
        # back to the common +/-24 st convention.
        span = _FALLBACK_SEMITONE_MAX - _FALLBACK_SEMITONE_MIN
        raw = (target_semitones - _FALLBACK_SEMITONE_MIN) / span
        return max(0.0, min(1.0, raw))

    print(f"    WARN: unknown transposeParameter mapping '{mapping}'; trying text scan.")
    return _scan_text_mapping(plugin, param_name, target_semitones)


def _print_available_params(plugin) -> None:  # type: ignore[type-arg]
    """Print parameter names to help with debugging."""
    try:
        names = list(plugin.parameters.keys())
        if names:
            print(f"    INFO: available parameters: {names[:20]}" + (" ..." if len(names) > 20 else ""))
    except Exception:
        pass


def _apply_parameter_overrides(plugin, overrides: list) -> int:
    """Apply fixed parameter values from external-plugins.json.

    Each override is an object with:
      - name (str): parameter name (case-insensitive)
      - value (float): normalized 0..1 raw value to set
      - text (str, optional): if provided without value, scan for matching display text

    Returns the number of overrides successfully applied.
    """
    if not overrides:
        return 0

    applied = 0
    for item in overrides:
        if not isinstance(item, dict):
            continue
        name = str(item.get("name", "")).strip()
        if not name:
            print("    WARN: parameter override missing 'name'; skipping")
            continue

        resolved = _resolve_parameter_key(plugin, name)
        if resolved is None:
            print(f"    WARN: parameter override '{name}' not found in plugin")
            continue

        param = plugin.parameters[resolved]
        raw: float | None = None

        if "value" in item and item["value"] is not None:
            try:
                raw = float(item["value"])
            except (TypeError, ValueError):
                print(f"    WARN: parameter override '{name}' has non-numeric value; skipping")
                continue
        elif "text" in item and item["text"] is not None:
            target_text = str(item["text"]).strip().lower()
            original = param.raw_value
            for step in range(_SCAN_STEPS + 1):
                candidate = step / _SCAN_STEPS
                try:
                    param.raw_value = candidate
                except Exception:
                    continue
                display = (_get_param_string(param) or "").strip().lower()
                if display == target_text or target_text in display:
                    raw = candidate
                    break
            try:
                param.raw_value = original
            except Exception:
                pass
            if raw is None:
                print(f"    WARN: parameter override '{name}' text '{item['text']}' not found; skipping")
                continue
        else:
            print(f"    WARN: parameter override '{name}' needs 'value' or 'text'; skipping")
            continue

        raw = max(0.0, min(1.0, raw))
        try:
            param.raw_value = raw
        except Exception as exc:
            print(f"    WARN: could not set parameter '{resolved}' = {raw}: {exc}")
            continue

        display = _get_param_string(param) or ""
        print(f"    override: '{resolved}' = {raw:.4f} (\"{display}\")")
        applied += 1

    return applied


# ---------------------------------------------------------------------------
# Audio I/O helpers
# ---------------------------------------------------------------------------


def _read_wav_numpy(path: Path):  # type: ignore[return]
    """Return (audio_array [channels, samples], sample_rate) via pedalboard."""
    with AudioFile(str(path)) as f:
        audio = f.read(f.frames)
        sr = f.samplerate
    return audio, sr


def _write_wav_numpy(path: Path, audio, sample_rate: float) -> None:  # type: ignore[type-arg]
    """Write a numpy audio array [channels, samples] to a PCM-16 WAV file."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with AudioFile(str(path), "w", sample_rate, num_channels=audio.shape[0]) as f:
        f.write(audio)


# ---------------------------------------------------------------------------
# Core render logic
# ---------------------------------------------------------------------------


def render_plugin_passes(
    plugin_def: dict,
    sample_paths: list[Path],
    snapshot_dir: Path,
    block_size: int,
    dry_run: bool,
) -> dict | None:
    """Render all (sample, semitone) combinations for one plugin definition.

    Returns a manifest dict compatible with external-renders.example.json, or
    None if the plugin could not be loaded.
    """
    plugin_id_raw: str = plugin_def.get("pluginId", "")
    plugin_label: str = plugin_def.get("pluginLabel", plugin_id_raw)
    plugin_path_str: str = plugin_def.get("pluginPath", "")
    semitones: list[int] = plugin_def.get("semitones", [])
    transpose_param: dict = plugin_def.get("transposeParameter", {})
    param_name: str = transpose_param.get("name", "Transpose")
    param_mapping: str = transpose_param.get("mapping", "text")
    reported_latency: int = plugin_def.get("reportedLatencySamples", 0)

    if not plugin_path_str:
        print(f"  SKIP {plugin_id_raw}: no pluginPath defined")
        return None
    if not semitones:
        print(f"  SKIP {plugin_id_raw}: no semitones defined")
        return None

    plugin_path = Path(plugin_path_str)
    if not plugin_path.exists():
        print(f"  SKIP {plugin_id_raw}: plugin not found at {plugin_path}")
        return None

    print(f"\n  Plugin: {plugin_label}")
    print(f"  Path:   {plugin_path}")
    print(f"  Param:  '{param_name}' (mapping={param_mapping})")
    print(f"  Semitones: {semitones}")

    if dry_run:
        print("  [dry-run] Would load plugin and render passes - skipping.")
        return None

    # Load the plugin.
    try:
        plugin = pedalboard.load_plugin(str(plugin_path))
    except Exception as exc:
        print(f"  ERROR: could not load plugin: {exc}")
        return None

    # Apply fixed overrides first (e.g. disable Archetype amp/cab/FX blocks so
    # only transpose remains) so the comparison is fair vs native transpose.
    overrides = plugin_def.get("parameterOverrides") or []
    if isinstance(overrides, list) and overrides:
        print(f"  Applying {len(overrides)} parameter override(s)...")
        applied = _apply_parameter_overrides(plugin, overrides)
        print(f"  Applied {applied}/{len(overrides)} parameter override(s)")

    resolved_param_name = _resolve_parameter_key(plugin, param_name)
    if resolved_param_name is None:
        print(f"  WARN: parameter '{param_name}' not found in plugin")
        _print_available_params(plugin)
        return None
    effective_param_name = resolved_param_name
    if effective_param_name != param_name:
        print(f"  INFO: resolved parameter '{param_name}' -> '{effective_param_name}'")

    # Resolve plugin ID as a filesystem-safe slug.
    safe_id = re.sub(r"[^a-zA-Z0-9_-]", "_", plugin_id_raw.strip()) or "external"
    out_dir = snapshot_dir / "external" / safe_id
    out_dir.mkdir(parents=True, exist_ok=True)

    entries: list[dict] = []

    for sample_path in sample_paths:
        if not sample_path.is_file():
            print(f"  WARN: demo sample not found: {sample_path}")
            continue

        print(f"  Sample: {sample_path.name}")

        try:
            audio, sr = _read_wav_numpy(sample_path)
        except Exception as exc:
            print(f"  ERROR: could not read {sample_path.name}: {exc}")
            continue

        for st in semitones:
            print(f"    {st:+d} st ... ", end="", flush=True)

            # Resolve the normalized parameter value for this semitone.
            raw_val = _resolve_normalized_value(plugin, effective_param_name, st, param_mapping)
            if raw_val is None:
                print("skipped (no parameter value found)")
                continue

            # Set the transpose parameter.
            try:
                plugin.parameters[effective_param_name].raw_value = raw_val
            except Exception as exc:
                print(f"skipped (could not set parameter: {exc})")
                continue

            # Process audio.
            try:
                board = pedalboard.Pedalboard([plugin])
                processed = board.process(audio, sample_rate=sr, buffer_size=block_size, reset=True)
            except Exception as exc:
                print(f"skipped (processing error: {exc})")
                continue

            # Save rendered WAV.
            tag = _semitone_tag(st)
            stem = sample_path.stem
            wav_name = f"{safe_id}_{stem}_{tag}.wav"
            wav_rel = f"external/{safe_id}/{wav_name}"
            wav_abs = snapshot_dir / wav_rel

            try:
                _write_wav_numpy(wav_abs, processed, sr)
            except Exception as exc:
                print(f"skipped (write error: {exc})")
                continue

            print(f"ok -> {wav_rel}")
            entries.append(
                {
                    "sample": sample_path.name,
                    "semitones": st,
                    "wav": wav_rel,
                    "reportedLatencySamples": reported_latency,
                }
            )

    if not entries:
        print(f"  WARN: no passes rendered for {plugin_id_raw}")
        return None

    manifest = {
        "pluginId": plugin_id_raw,
        "pluginLabel": plugin_label,
        "entries": entries,
    }
    return manifest


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("snapshot_dir", type=Path, help="Snapshot directory containing results.json")
    parser.add_argument(
        "--collection",
        type=Path,
        default=None,
        help="Plugin collection JSON (default: tools/transpose-benchmark/external-plugins.json relative to repo root)",
    )
    parser.add_argument(
        "--plugin",
        metavar="PLUGIN_ID",
        action="append",
        dest="plugins",
        default=None,
        help="Limit rendering to specific pluginId(s). May be repeated. Default: all plugins.",
    )
    parser.add_argument(
        "--samples",
        metavar="NAME",
        action="append",
        dest="samples",
        default=None,
        help="Limit rendering to specific sample file names. May be repeated. Default: guitar-riff-01.wav only.",
    )
    parser.add_argument(
        "--all-demo-audio",
        action="store_true",
        help="Render all three demo audio files (same set as TransposeBenchmark --all-demo-audio).",
    )
    parser.add_argument(
        "--block-size",
        type=int,
        default=DEFAULT_BLOCK_SIZE,
        help=f"Audio processing block size in samples (default: {DEFAULT_BLOCK_SIZE}).",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Show what would be rendered without loading plugins or writing files.",
    )
    args = parser.parse_args()

    if not _PEDALBOARD_OK:
        print("ERROR: pedalboard and numpy are required. Install them with:")
        print("    pip install pedalboard numpy")
        return 1

    snapshot_dir: Path = args.snapshot_dir.resolve()
    if not snapshot_dir.is_dir():
        print(f"ERROR: snapshot_dir not found: {snapshot_dir}")
        return 1

    results_path = snapshot_dir / "results.json"
    if not results_path.is_file():
        print(f"ERROR: no results.json in snapshot directory: {snapshot_dir}")
        print("       Run TransposeBenchmark first to create the snapshot.")
        return 1

    # Locate repo root and demo audio.
    repo_root = _find_repo_root(snapshot_dir)
    if repo_root is None:
        # Fallback: assume snapshot_dir is inside the repo.
        repo_root = snapshot_dir
    demo_audio_dir = repo_root / "core" / "ui" / "demo"

    # Resolve sample list.
    if args.all_demo_audio:
        requested_samples = _DEFAULT_SAMPLES
    elif args.samples:
        requested_samples = args.samples
    else:
        requested_samples = [_DEFAULT_SAMPLES[0]]  # guitar-riff-01.wav only

    sample_paths = [demo_audio_dir / s for s in requested_samples]
    print(f"Demo audio dir: {demo_audio_dir}")
    for sp in sample_paths:
        status = "ok" if sp.is_file() else "MISSING"
        print(f"  [{status}] {sp.name}")

    # Load plugin collection.
    if args.collection:
        collection_path = args.collection.resolve()
    else:
        collection_path = Path(__file__).resolve().parent / "external-plugins.json"

    if not collection_path.is_file():
        print(f"ERROR: plugin collection not found: {collection_path}")
        return 1

    try:
        collection = json.loads(collection_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        print(f"ERROR: could not read collection: {exc}")
        return 1

    plugins: list[dict] = collection.get("plugins", [])
    if not plugins:
        print("ERROR: no plugins defined in collection")
        return 1

    # Apply plugin filter.
    if args.plugins:
        filter_ids = {p.lower() for p in args.plugins}
        plugins = [p for p in plugins if p.get("pluginId", "").lower() in filter_ids]
        if not plugins:
            print(f"ERROR: no plugins matched filter: {args.plugins}")
            return 1

    print(f"\nSnapshot: {snapshot_dir}")
    print(f"Plugins:  {len(plugins)}")

    manifest_paths: list[Path] = []

    for plugin_def in plugins:
        manifest = render_plugin_passes(
            plugin_def=plugin_def,
            sample_paths=sample_paths,
            snapshot_dir=snapshot_dir,
            block_size=args.block_size,
            dry_run=args.dry_run,
        )
        if manifest is None:
            continue

        plugin_id_raw = plugin_def.get("pluginId", "external")
        safe_id = re.sub(r"[^a-zA-Z0-9_-]", "_", plugin_id_raw.strip()) or "external"
        manifest_path = snapshot_dir / f"auto-manifest-{safe_id}.json"
        manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        print(f"\n  Manifest written: {manifest_path.name} ({len(manifest['entries'])} entries)")
        manifest_paths.append(manifest_path)

    if not manifest_paths:
        if not args.dry_run:
            print("\nWARN: no manifests were written - check plugin paths and semitone parameters above.")
        return 0

    print(f"\nDone. {len(manifest_paths)} manifest(s) written.")
    print("Run build_external_passes.py for each manifest to compute metrics:")
    for mp in manifest_paths:
        print(f"  python build_external_passes.py {snapshot_dir} {mp}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
