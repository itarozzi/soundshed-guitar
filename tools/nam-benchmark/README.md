# NAM benchmark

Measures the NAM core the way the app runs it, against `FusedWaveNet.h`, a prototype fused forward pass
for the WaveNet shapes NAM models ship in. Written for
[docs/nam-dsp-performance-review-2026-10-16.md](../../docs/nam-dsp-performance-review-2026-10-16.md);
the numbers there came from these tools.

The prototype is not in the app. Each tool compiles the fetched NAM core with the core's Release flags
(`/Ox /fp:fast /arch:AVX2`, `NAM_ENABLE_A2_FAST=1`, fast tanh on), so a number here is the model alone,
with no chain or wrapper around it.

## Build

Windows only. Configure `core/build` or `juce/builds` once first, so the NAM core and nlohmann_json
sources are fetched.

```bash
node tools/nam-benchmark/build.mjs            # MSVC, AVX2: what Windows ships
node tools/nam-benchmark/build.mjs --clang    # clang-cl -O3 -ffast-math -mavx2 -mfma: stands in for macOS Intel and Linux
node tools/nam-benchmark/build.mjs --sse      # 128-bit vectors: the nearest Windows gets to NEON
node tools/nam-benchmark/build.mjs --define FWN_OT=6 --clean
```

The output lands in `tools/nam-benchmark/build/<compiler>-<isa>/`, which git ignores. The NAM library
is compiled once per output folder. Pass `--clean` after the NAM pin moves, or when `--define` changes
something the library sees.

## Tools

Every model argument is a `.nam` path. Add `:size` for an A2 container's slimmable size: 0.0 is nano,
and 1.0, the app's default, is full.

| Tool | What it answers |
|---|---|
| `bench.exe [--block N] [--rounds R] [--seconds S] [--slow-tanh] [fused:]model ...` | ns per sample and µs per block at 48 kHz. Models run interleaved, and the best round is kept. A `fused:` entry is compared with the plain entry for the same file: speed-up, SNR and largest error |
| `invariance.exe [--scale N] [--sweep] model ...` | Renders 2 s with host blocks of 64, 47, 1, mixed sizes and 512 (past the prepared size, so it chunks). The fused output must be bit-identical across all of them, or it exits 1. Also prints the generic model's own spread across block sizes, and a hash for comparing builds. `--scale` sets the oversampling time scale |
| `loadtime.exe model ...` | Parse and build time, a model-cache hit (`get_dsp` from parsed data, including its own prewarm), and `Reset(48000, 65)` for both paths |
| `coverage.exe list.txt` | For each path in a UTF-8 list: does the fused path take it, and how far is it from the generic model? Prints any model below 90 dB, with its level and largest error |
| `sensitivity.exe model ...` | Whether a model amplifies float rounding. It nudges the input by 1e-7 and 1e-6 and compares the generic output against itself. If that moves it as far as the fused path sits from it, the gap belongs to the model |
| `parts.exe` | Time each step of one generic WaveNet layer (conv, mixin, tanh, 1x1, copies) across channel counts |

Useful models: `resources/metal-presets/Judas Priest - Painkiller/nam/JCM800 Hi P6 B8 M4 T7 G10.nam`
(the standard 16/8 WaveNet) and `core/tests/testdata/assets/amps/Guitar/A2/Boosted-6505+-A2-Chugs-FullRig.nam`
(an A2 container).

## Measuring on this kind of machine

- Each tool pins itself to core 4 at High priority, with FTZ/DAZ set as on an audio thread. Other work
  on the machine still moves single runs by 10-20%. Compare two builds by running them interleaved and
  keeping the minimum, and read the "worst block" column only on a quiet machine.
- Paths over 260 characters fail to open in these tools: they have no long-path manifest, so `coverage.exe`
  reports them as unreadable.
- `invariance.exe` never hands the generic model more frames than it was prepared for. The NAM core
  writes past its buffers when that happens, because Release builds compile the assert out.
