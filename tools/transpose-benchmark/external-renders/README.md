# External Render Manifests

Place one JSON manifest file here for each external plugin you want to include in the benchmark report.

`run_benchmark.ps1` automatically discovers every `*.json` file in this directory and passes each one to `build_external_passes.py`.

## Workflow

1. Render audio through the external plugin at each semitone setting you want to compare (manually or via your DAW). Use the same dry source files as the benchmark (`core/ui/demo/guitar-riff-01.wav`, `guitar-riff-02.wav`, `DI_Guitar_L.mp3`).

2. Copy the rendered WAVs into the snapshot directory that `run_benchmark.ps1` creates, typically under `transpose-benchmark-out/<snapshot>/external/`.

3. Write a manifest JSON and drop it here. See `../external-renders.example.json` for the schema, or the example below.

## Manifest schema

```json
{
  "pluginId": "hypertune_vst3",
  "entries": [
    {
      "sample": "guitar-riff-01.wav",
      "semitones": -12,
      "wav": "external/HyperTune_guitar-riff-01_m12.wav",
      "reportedLatencySamples": 0
    },
    {
      "sample": "guitar-riff-01.wav",
      "semitones": 12,
      "wav": "external/HyperTune_guitar-riff-01_p12.wav",
      "reportedLatencySamples": 0
    }
  ]
}
```

- `pluginId` — must match a `pluginId` entry in `../external-plugins.json` for metadata (label, path) to be picked up automatically.
- `wav` — path to the rendered WAV, **relative to the snapshot directory** (`transpose-benchmark-out/<snapshot>/`).
- `reportedLatencySamples` — plugin-reported latency in samples (set to 0 if unknown).
- `processMs` / `avgBlockUs` / `maxBlockUs` / `realtimeFactor` — optional timing fields; omit if unavailable.
