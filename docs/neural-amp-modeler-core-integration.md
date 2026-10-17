# NeuralAmpModelerCore Integration Summary

This document summarizes how `NeuralAmpModelerCore` is currently defined and used in Soundshed Guitar.

## Assumptions

- `SoundshedGuitarCore` is the single source of truth for NAM dependency wiring.
- The active consumer project (JUCE) includes `core/` via `add_subdirectory(...)`.
- `GUITARFX_CORE_FETCH_DEPS` decides whether NAM is auto-fetched or must be supplied by the consumer.

## Build-Time Integration

### 1. Dependency declaration and fetch

In `core/CMakeLists.txt`:

- NAM integration is gated by:
  - `GUITARFX_CORE_FETCH_DEPS`
  - `NOT TARGET NeuralAmpModelerCore`
  - `NOT TARGET nam`
- Fetch source:
  - Repository: `https://github.com/DLC86/NeuralAmpModelerCore.git`, a fork that adds time-scaled
    temporal convolutions (`SetTimeScale`) for oversampled rendering
  - Pinned commit: `bea723b300e2dfe54f417c9e9f9d7f68b4a8f30d`
- The fork's top-level project always adds its command-line tools, so the source is only
  populated (`FetchContent_Populate`), never added as a project. `NAM_BUILD_TOOLS` and
  `NAM_BUILD_TESTS` are set `OFF` as well.

### 2. Compatibility handling

`core/CMakeLists.txt` applies a few integration safeguards:

- Eigen bootstrap: ensures NAM's expected `Dependencies/eigen` tree exists by copying fetched Eigen headers when missing.
- `NAM_ENABLE_A2_FAST=1` on whichever NAM target exists, so the fork's fixed-size A2 path is compiled in.
- No link-time optimisation for the NAM target: MSVC's `/GL` made it 1.3-2x slower.

### 3. Target creation and linking model

`SoundshedGuitarCore` supports two NAM target patterns:

- Preferred: `NeuralAmpModelerCore`
- Fallback: `nam`

If upstream does not define `NeuralAmpModelerCore`, `core/CMakeLists.txt` creates a local static `NeuralAmpModelerCore` target from NAM `*.cpp` sources and sets include/link requirements. It does so in `core/cmake/NeuralAmpModelerCore/`, a directory scope of its own, so that Debug builds can compile NAM optimised (`/O2` without `/RTC1` on MSVC, `-O2` elsewhere) while the rest of the core stays unoptimised. Unoptimised Eigen ran a model load, prewarm and 8 blocks in 2.6 s against 72 ms optimised, with identical output, and made every Debug test and Debug app session that touches NAM crawl. Configure with `-DGUITARFX_OPTIMISE_NAM_IN_DEBUG=OFF` to step through NAM itself.

`SoundshedGuitarCore` then links:

- `NeuralAmpModelerCore` when present
- otherwise `nam` when present

When `NeuralAmpModelerCore_SOURCE_DIR` is known, NAM include paths are also exported to consumers.

It also links `SoundshedGuitarFusedWaveNet`, the fused WaveNet pass (`src/dsp/nam/`, see below),
built in `core/cmake/FusedWaveNet/` for the same reason as the NAM target: a directory scope of its
own, so it is optimised in Debug too (`GUITARFX_OPTIMISE_NAM_IN_DEBUG` covers both), and never
link-time optimised. It compiles with `-fno-associative-math` under clang and gcc, so its sums keep
their source order and its output stays bit-identical across host block sizes.

### 4. Consumer projects

- JUCE project forces core dependency fetch:
  - `juce/CMakeLists.txt` sets `GUITARFX_CORE_FETCH_DEPS ON` before adding `core/`.
- JUCE is the active and supported integration path.

## Runtime Usage in DSP

### 1. Effect registration

NAM-backed effects are registered in `core/src/dsp/effects/BuiltinEffects.h`:

- `amp_nam`
- `amp_nam_optimized`
- `amp_nam_blend`

### 2. Model loading

Every NAM model the app runs is built by `nammodelcache::GetModel(path)`
(`core/src/dsp/NamModelCache.h`): `OptimizedNAMAmpEffect` (the NAM amp and NAM FX nodes) and
`MultiModelNAMAmpEffect` (NAM Blend), one model per channel. Nothing else calls `nam::get_dsp`.

The cache parses each file once into `nam::dspData` and keeps it, keyed by path, size and write
time. Then, for each model it builds:

- **The fused WaveNet pass** (`core/src/dsp/nam/FusedWaveNet.h`) where the shape allows: a
  WaveNet whose layers are non-gated Tanh or LeakyReLU with layer1x1 on and no head1x1, FiLM,
  grouping, slimmable channels, condition DSP or post-stack head, or a `SlimmableContainer` whose
  every submodel is one. That is every NAM model measured (2,460 of them, the standard, lite,
  feather, nano, xSTD and A2 shapes). The weights are packed once per file and shared by every
  instance and channel; a build plus prewarm takes well under a millisecond, and it runs 2.0-4.8x
  faster than the NAM core, within float rounding of its output (100 dB or more on every test
  asset) and bit-identical across host block sizes.
- **The NAM core** (`nam::get_dsp` on a copy of the cached data) for anything else: LSTM,
  ConvNet, Linear and WaveNet options the fused pass does not compute.

Both apply the file's metadata (loudness, input and output level) as `nam::create_dsp` does, and
both run fast tanh: `GetModel` switches the NAM core to it before the first model is built.

The fused pass needs a vector kernel (`core/src/dsp/nam/FusedWaveNetSimd.h`): AVX2 with FMA on the
avx2 tier, two SSE halves on other x86-64 builds, and two NEON halves with fused multiply-adds on
arm64 (macOS, Windows on ARM, Linux, Android). A plain C++ version measured 0.3x the NAM core, so
nothing else runs the fused pass.

**The NEON kernel is on by default.** Timed on a GitHub Apple silicon runner by
`.github/workflows/neon-benchmark.yml` (Apple clang 17, 48 kHz, model alone), it ran 1.7-2.5x the
NAM core on a standard and an A2 full model and 1.1-1.3x on A2 lite at host blocks of 32 to 512,
with output 117-120 dB from the NAM core's and FusedWaveNetTests passing on the hardware. The same
workflow also times it on GitHub's arm64 Linux runner (`ubuntu-24.04-arm`, GCC) and its Windows on
ARM runner (`windows-11-arm`, MSVC); neither has been run yet, and Android has not been timed at all
(see below). Configuring the core with `-DGUITARFX_FUSED_WAVENET_NEON=OFF` puts every arm64 model
back on the NAM core (docs/nam-dsp-performance-review-2026-10-16.md, recommendation 3). Its
register tile is 4 outputs x 3 vectors of 8 frames, as on AVX2: 24 accumulators, 6 input registers
and one holding the four outputs' weights, 31 of NEON's 32. The accumulators are named variables
rather than an array, because MSVC's ARM64 back end stores an array of them back to the stack on
every input, and the four weights come from one load, multiplied by lane, because GCC spilled three
accumulators when it held four broadcast weights. Each output still sums its inputs in source order,
so every tile gives the same bits.

Checked without ARM hardware (2026-10-16):

- It compiles without warnings in MSVC ARM64 (`/arch:armv8.2`), clang-cl for
  `aarch64-pc-windows-msvc`, and GCC 13 and clang 18 for `aarch64-linux-gnu`, and in all four the
  tile's loop keeps every accumulator in registers: per input, 6 input loads, one weight load and 24
  multiply-adds.
- `FusedWaveNetTests` passes on aarch64 Linux under QEMU user-mode emulation, built with GCC 13 and
  with clang 18 (`-O3 -ffast-math -fno-associative-math`, kernel on): all 29 models 110.0-140.5 dB
  from the NAM core, bit-identical across host block patterns, the one-pass prewarm equal to a real
  one, time scales 2 and 4 at 118.2 dB or more. First blocks come closer to the 100 dB floor than on
  x86-64 (103.8 there): 101.0 dB at worst under clang and 102.8 under GCC. Both sides of that
  comparison change on arm64, since the NAM core runs Eigen's NEON code there.
- The x86-64 kernels are untouched: MSVC compiles `FusedWaveNet.cpp` to the same instructions as
  before for AVX2 and SSE.

Emulation says nothing about speed. `.github/workflows/neon-benchmark.yml` times the kernel on
hardware. Dispatch it with `platforms` set to any of `macos`, `linux` and `windows` (all three by
default). Each platform builds the core in Release with the kernel on:

- macOS with Apple clang;
- arm64 Linux with GCC and `-fsigned-char`, which the core needs there as the JUCE build does,
  since WDL's headers under the NAM resampler refuse unsigned char;
- Windows with MSVC ARM64 through the Visual Studio generator. MSVC compiles the kernel differently
  from clang and GCC, so Windows needs its own run.

Each runs `FusedWaveNetTests` and `FusedWaveNetBenchmark --require-kernel NEON`, and writes the
same table to the run's summary under its own heading, with the CPU it ran on. GitHub's arm64
runners are cloud VMs on server-class Arm cores, so the Linux figures stand in for Android phones
only loosely.

By hand, on a machine the workflow cannot reach:

1. Configure the core with `-DGUITARFX_CORE_BUILD_TESTS=ON -DGUITARFX_FUSED_WAVENET_NEON=ON` (on
   arm64 Linux also `-DCMAKE_C_FLAGS=-fsigned-char -DCMAKE_CXX_FLAGS=-fsigned-char`). Build
   `FusedWaveNetTests`, `FusedWaveNetBenchmark` and `NAMPerformanceBenchmark` in Release.
2. `FusedWaveNetTests` must pass and print `Fused WaveNet kernel: NEON`.
3. `FusedWaveNetBenchmark --require-kernel NEON` prints the workflow's table (`--markdown FILE`
   writes it).
4. `NAMPerformanceBenchmark`, once with no argument (a standard WaveNet) and once with an A2 model
   (`core/tests/testdata/assets/amps/Guitar/A2/...`), must print `Fused kernel: NEON`. Compare the
   NAM core and fused rows, directly and through `amp_nam`, at 512 and at 48 kHz/64. AVX2 measured
   2.8x (standard) and 3.1x (A2) directly, 2.3-2.8x through the effect.
5. If a platform's fused rows lose, narrow the `GUITARFX_FUSED_WAVENET_NEON` default in
   `core/CMakeLists.txt` for that platform. The Android build does not build the core tests, so
   Android needs either the test targets built with the NDK and run over adb, or a measurement in
   the app.

**Switching it off.** Launching with the environment variable `GUITARFX_NAM_ENGINE=core` makes
every model the NAM core's, for A/B listening and support. Tests and tools call
`nammodelcache::SetFusedEngineEnabled(false)`. Models already built keep the engine they were
built with. The session log says which engine a run used, once, on the first model load
(`[NamModelCache] NAM engine: ...`), and `nammodelcache::GetStats()` counts `fusedBuilds` and
`coreBuilds`.

### 3. Factory registration guard

`core/src/dsp/NAMFactoryForce.cpp` forces reference to NAM factory entry points (`wavenet`, `lstm`, `convnet`, `linear`, `container`) so static registration is not dead-stripped by the linker. The fused pass does not register a parser: the fork's `ConfigParserRegistry` throws on a second `WaveNet` registration, so it sits in the model cache instead.

Each NAM effect registration path calls `nam::factory::ForceFactoryRegistration()` during startup registration.

### 4. Processing behavior

Current NAM effect flow includes:

- Stereo model instances (left/right); a mono connection builds only the left one
- Resampling when model expected sample rate differs from host sample rate, and optional
  oversampling with the model's dilations time-scaled to match (`NamOversamplingProcessor`)
- Optional metadata-driven auto-level behavior (input/output/loudness/normalization gain)
- Wet/dry mix and output gain staging

## Tests

Core tests in `core/tests/CMakeLists.txt` link NAM targets the same way:

- Link `NeuralAmpModelerCore` if present
- otherwise link `nam` if present

This keeps test binaries aligned with runtime linkage behavior.

`FusedWaveNetTests` holds the fused pass to the NAM core: every test asset and synthetic models of
the shapes the assets lack, at least 100 dB apart at time scales 1, 2 and 4; bit-identical output
across host block patterns; the one-pass prewarm equal to a real one; and the shape detector refusing
everything it does not compute. Run it in Release and in the clang-cl build too, since fast-math is
where such code breaks. On arm64 it skips itself unless the NEON kernel is switched on (see above).

## Current Integration Contract (Practical)

- Primary contract: consumer links `SoundshedGuitarCore`.
- Dependency ownership: currently centralized in `core/CMakeLists.txt` with optional fetch mode.
- Target compatibility: both `NeuralAmpModelerCore` and `nam` are accepted.
- Runtime dependency use: all NAM inference paths live in core effect processors.
