# Signal Chain DSP Performance Review

**Date:** 14 October 2026
**Baseline:** `20dc9cc0` on `main`
**Scope:** NAM processing cost, the per-block cost of the signal chain around it, audio and helper thread priority, and compiler settings. The question asked: is Soundshed as cheap as, or cheaper than, a simple single-model NAM plugin?

## Status (14 October 2026)

Made after the review:

| # | Change | Where | Measured |
|---|---|---|---|
| 1a | A Stereo input whose sides match folds the rigs to mono | `dsp/IdenticalInputDetector.h`, `MultiPresetMixer::UpdateIdenticalInputFold`; `IdenticalInputFoldTests` | Standard model, stereo track, 48 kHz/64: 118.8 → 57.6 µs a block (with a cab IR, 131.2 → 70.8). The output is bit-identical while folded |
| 2 | The DAW parameter queue's lock is only tried on the audio thread | `PluginProcessorAdapter::applyPendingDAWParamChanges` | |
| 2 | The resampler's two per-block `std::cerr` writes are removed, and configure fails if one comes back | `GuitarfxAudioDSPTools.cmake` | |
| 2 | Riff capture raises notices on the audio thread; the message thread builds the JSON | `controller/RiffCaptureNotices.h`, `PublishRiffCaptureNotices`; `RiffCaptureNoticeTests` | |
| 3 | Lanczos A=32 → 12 | `NAMOversampling.h` (`kNamResamplerLanczosHalfWidth`) | 44.1 kHz host: 73.1 → 69.7 µs a block, latency 66 → 27 samples. audio-ab at 44.1 kHz: no band of the DI or the sweep more than 0.05 dB apart; only the impulse differs. Not yet checked by ear. |
| 5 | gcc/clang avx2 tier is `-march=x86-64-v3` (adds FMA) | `GuitarfxSimdLevel.cmake` | Not built here: no Linux or Intel Mac build was run |
| 6 | The NAM library is never link-time optimised | `cmake/NeuralAmpModelerCore/CMakeLists.txt` | |
| | Deferred rebuilds also run off the plugin's editor-closed timer | `PluginController::DrainControlSurfaceRequests` | Needed by 1a. It also fixes automation-triggered rebuilds (IR Normalize, Low Latency) waiting for the editor to open |

Not done: 4 (fork rebase), 7 (macOS workgroup, Linux priorities, time-bound spin), 8 (framework trims), 9 (committed parity benchmark), 10 (clang-cl experiment). The busy-DSP-lock silence in finding 6 is also still open: it needs the message thread to hold that lock less, not a change in the callback.

## Executive Summary

Per model, our NAM inference is faster than upstream NeuralAmpModelerCore built the usual way. With CMake's default Release flags (MSVC `/O2`, no `/arch`, no `/fp:fast`), upstream runs a standard WaveNet or an A2 full model 1.6x slower than our build. Without fast tanh it runs 3.3x slower. On a mono input the whole chain costs the bare model plus about 2-5 µs per 64-sample block. That puts us ahead of a simple plugin on a mono track.

We lose in three places:

1. **A stereo track costs 2.0x.** With a stereo DAW bus, the chain is stereo from the input, so every NAM node and cab runs both sides. That holds even when the two sides carry the same mono guitar. REAPER, Ableton Live, Bitwig and FL Studio tracks are stereo by default. A single-model plugin runs one model there.
   - A standard model on a stereo track costs 117 µs per block for us.
   - A plain upstream build costs about 84 µs per block for the same model.
   This is the gap users notice, and it was already listed as open ("mono NAM for identical L/R on stereo buses, undecided").
2. **A host rate different from the model's adds 26%.** At 44.1 kHz with a 48 kHz model, the resampler (Lanczos A=32 plus anti-alias stages, scalar double) costs about 120 ns per host sample. The model also runs 8.8% more samples.
3. **Small models.** An A2 nano costs about 19 µs per block, so the framework's 2 µs is 10% of it. At that size our core is only 5% faster than a plain upstream build. Upstream's newer core and clang codegen are both faster on nano.

There are also real-time-safety problems: a blocking mutex on every audio block, a `std::cerr` and JSON building on the audio thread, and a whole block silenced whenever the DSP lock is busy. These cause spikes and pops rather than average load.

## Method

- **Machine:** Ryzen 7 8845HS, Windows 11, MSVC 14.51 and clang-cl 22 from VS 18.
- **Load:** the machine was carrying about 40% load from other work. Every figure is the best of 3 to 5 interleaved rounds, and maximum block times were not interpreted.
- **Pinning:** each harness pins to one core at High priority, with FTZ/DAZ set as on an audio thread.
- **Models:**
  - *Standard:* JCM800 G10 (`resources/metal-presets/…`), a WaveNet 16x10 + 8x10.
  - *A2:* Boosted 6505+ (`core/tests/testdata/…/A2`), a SlimmableContainer. *A2 full* is size 1.0, the 8-channel model and our default. *A2 nano* is size 0.0, 3 channels.
- **Core harness:** `nam::get_dsp` → `Reset(48k, 64)` → `process()` in 64-sample blocks.
  - It was built six ways: our fork pin (`DLC86@bea723b`) or upstream (`sdatkinson@1f42f88`, June 2026), each with our flags or with plain `/O2`, plus clang-cl.
- **Chain harness:** the real `MultiPresetMixer` with the default global chains and an `input → amp_nam_optimized → output` preset (optionally with an `ir_cab`). It ran in a scratch Release build of the core, against the same model run bare in the same binary. Its per-block call matches what the existing `SteadyStateProfiler` makes (`SetTempo` + `Process`).
- **Resampler harness:** `dsp::ResamplingContainer` with an identity callback, from the build tree's patched overlay.

The harnesses are scratch code, not committed (see Recommendation 9).

## Measurements

### NAM core, ns per sample, 64-sample blocks, fast tanh on

| Model | Ours (fork, `/Ox /fp:fast /arch:AVX2`) | Fork + `/GL` (LTO) | Fork, `/arch:AVX` (no FMA) | Fork, clang-cl `-O3 -ffast-math -mavx2 -mfma` | Upstream, our flags | Upstream, plain `/O2` |
|---|---|---|---|---|---|---|
| Standard | 812 | 1069 | 919 | 721 | 768 | 1318 |
| A2 full | 752 | 1512 | 785 | 783 | 704 | 1210 |
| A2 nano | 300 | 288 | 310 | 192 | 278 | 316 |

Without fast tanh, plain `/O2` upstream runs the standard model at 2645 ns. MSVC only vectorises `std::tanh` under `/fp:fast` with AVX2. The A2 path uses LeakyReLU, so fast tanh does not change it.

### Whole chain, µs per block at 48 kHz

| Case | µs per block | vs bare model |
|---|---|---|
| Standard, bare model, 64 | 53.5 | |
| Standard, chain, mono input, 64 | 58.2 | +4.7 µs (+9%) |
| Same, editor open (diagnostics on) | 58.6 | +0.4 µs over closed |
| Standard, chain, **stereo** input, 64 | **116.9** | **2.0x** |
| Standard, chain, mono, **44.1 kHz** host, 64 | **73.1** | **+26%** over 48 kHz |
| Standard, chain, mono, 96 kHz host, 64 (667 µs deadline) | 38.1 | the model runs 32 samples per block at 48 kHz |
| Standard, bare model, 32 | 30.2 | |
| Standard, chain, mono, 32 | 30.9 | +0.7 µs |
| Standard + 108 KB cab IR, mono, 64 | 69.5 | the cab adds 11 µs |
| Standard + cab IR, stereo, 64 | 130.2 | |
| A2 full, bare model, 64 | 49.0 | |
| A2 full, chain, mono, 64 | 51.9 | +2.9 µs (+6%) |
| A2 full, chain, mono, 44.1 kHz, 64 | 64.4 | +24% over 48 kHz |

Against a 1333 µs deadline at 64 samples, one mono standard-model instance is 4.4% of a core and a stereo one is 8.8%.

### Framework alone (empty preset, default global chains)

| Block | Editor closed | Editor open |
|---|---|---|
| 32 | 1.0 µs | 1.6 µs |
| 64 | 1.8 µs | 3.0 µs |
| 128 | 3.5 µs | 5.7 µs |

That is about 28 ns per sample, and about 45 ns with diagnostics on. Mono and stereo inputs cost the same here. Removing the global-chain config changed nothing measurable, so the cost sits in the mixer and executor plumbing rather than in the global effects.

### Resampler alone, ns per host sample

| Host ↔ model | double A=32 (shipping) | double A=12 | float A=32 | float A=12 |
|---|---|---|---|---|
| 44.1k ↔ 48k | 121-125 | 82-83 | 136 | 83-112 |
| 96k ↔ 48k | 92-98 | 74 | | |

Float buys nothing, because the IIR anti-alias stages dominate and the Lanczos SIMD path is not taken.

## Findings

### 1. Stereo buses run every model twice for a mono guitar (P1, confirmed)

`MultiPresetMixer::ApplyInputLayout` (`core/src/dsp/MultiPresetMixer.cpp:136`) makes the rig stereo whenever the host bus has two inputs. `ResolveChannelLayout` then runs both sides of every node. For `amp_nam_optimized` that means two models (`OptimizedNAMAmpEffect.h:212-223`), and an IR cab gets two convolutions. At 64 samples the two sides run one after the other (`DualLaneExecutor` needs 96 or more), and in plugins multi-core is off by default. Measured cost: 116.9 µs against 58.2 µs for a mono input.

This follows the 2026-10-07 rule that the DAW bus decides the layout. That rule was right for correctness; the cost is the one thing it leaves open. Options, cheapest for users first:

- **a. Run one side while the input sides are identical.** Each block, compare the host's L and R input with `memcmp` (about 20 ns per block). After about 250 ms of bit-identical input, process the rig as Mono. On the first block that differs, return to Stereo through the machinery that already exists: a re-resolve in place, `StereoFadeIn` on the right side, and the NAM right-model fade-in.
  - It is exact while the sides match.
  - The cost is a short fade when a source really turns stereo, which is rare.
  - It reads audio to decide a layout, which the stereo-input design avoided on purpose. It only ever chooses between two correct layouts, though, so it cannot mis-route.
- **b. A per-instance input-mode choice in plugins**, as the standalone has: Mono L, Mono sum or Stereo. It is cheap and explicit, but most users will never find it.
- **c. Default plugin instances to a mono-sum input.** This matches simple plugins, but it changes the sound of stereo sources and of existing sessions.

Recommendation: (a), with (b) as the override.

### 2. Resampling at a mismatched host rate (P2, confirmed)

`NAMOversampling.h:148` instantiates `ResamplingContainer<NAM_SAMPLE, 1, 32>`. The overlay's default is A=12. A=32 came in with configurable oversampling (`73d3fc12`), and no measurement was recorded for it.

- At 44.1 kHz it costs 121 ns per host sample against 82 ns at A=12, which is 7.8 µs against 5.3 µs per 64-sample block.
- It also runs `std::cerr` on the audio thread when the downsampler underflows (generated `ResamplingContainer.h`, at the "Did not yield enough samples" message). That breaks the never-stdout/stderr rule: stderr on an undrained pipe blocks.

Try A=12 and check quality with `tools/audio-ab` at `--rate 44100`. Patch the `cerr` out in `GuitarfxAudioDSPTools.cmake` like the other overlay fixes.

### 3. Compiler settings (confirmed by measurement)

- **a. Keep what Windows has.** `/Ox /fp:fast /arch:AVX2` plus fast tanh is the reason we beat a plain upstream build by 1.6x, or 3.3x when the plain build has no fast tanh.
- **b. Never build NAM with LTO.** `/GL` on the NAM library made A2 full 2.0x slower and the standard model 1.3x slower.
  - Today only SharedCode (the JUCE target) has `/GL`. `NeuralAmpModelerCore` and `SoundshedGuitarCore` do not; `juce/builds` Release vcxprojs were checked.
  - Turning IPO on for every library, an obvious-looking tidy-up, would cost exactly this.
  - Add a comment or guard in `core/cmake/NeuralAmpModelerCore/CMakeLists.txt` so nobody turns it on globally.
- **c. Our NAM pin is 5-7% slower than current upstream.** Our pin is DLC86's fork, which branched at upstream `9c7b185` (2 June). Upstream has since landed `dd972d6` ("Optimize additional small convolution paths") and `1f42f88` (MSVC `__restrict` fixes).
  - Upstream with the same flags measured 768 / 704 / 278 ns against our 812 / 752 / 300.
  - Rebase the fork, or ask its maintainer to. Its time-scale and polyphase additions are the only reasons we are on it.
- **d. clang-cl is much faster on A2 nano.** It measured 192 ns against 300 (1.56x) on nano, 11% faster on the standard model and 4% slower on A2 full.
  - Building only the NAM static library with clang-cl links into an MSVC build, because the ABI is compatible. The work is in the CMake: an ExternalProject, or a toolset per target.
  - Worth an experiment if A2 nano becomes a default tier. It is not worth it for full-size models.
- **e. gcc/clang x86-64 builds have no FMA.** `GuitarfxSimdLevel.cmake:93-96` passes `-mavx2` without `-mfma`, and Eigen only fuses on `__FMA__`; MSVC's `/arch:AVX2` implies it. This affects Linux x64 and the Intel slice of macOS.
  - An `/arch:AVX` build, used as a proxy, measured 4-13% slower.
  - Fix: `-mavx2 -mfma`, or `-march=x86-64-v3`, for the avx2 tier.
- **f. The Windows x64 release requires AVX2,** with no installer check (`publish-release.yml` ships only the avx2 build). An old CPU crashes instead of being told why.

### 4. Fixed per-block overhead (P3, measured; attribution by reading)

The framework costs about 28 ns per sample, which is 0.14% of the deadline at 64 samples. It only matters relative to small models: about 10% of an A2 nano. Not profiled. By reading, the main pieces are:

- The global Transpose is always enabled (`GlobalChainEditor::LivePreChain`). At 0 semitones it still records two history rings per sample on both channels (`TransposeEffect.h:123-139`). It also has no mono path.
- About 70 buffer passes per block: per-node clears, edge copies (the gather copies both sides even for a mono node, `SignalGraphExecutorPlan.cpp:441-452`), executor in/out with gain, and the mixer's mix and copy-back.
- With the editor open: about 10 clock reads per block for node timing, and `MixerTelemetry::Record` three times per block, unthrottled. That is the +60%.

Cheap wins:
- Time nodes only on blocks that refresh the meters.
- Skip the post-chain when every effect in it is bypassed and its gains are unity.
- Gather only the left side for mono nodes.
- Use power-of-two rings with masks in the Transpose history.

The NAM wrapper itself adds about 5 ns per sample: a float↔double conversion, a 5 Hz DC blocker and output gain (`OptimizedNAMAmpEffect.h:194-340`). That is under 1% of a full-size model and not worth touching.

### 5. Audio and helper thread priority (confirmed by reading)

- **At 64 samples or fewer, a single-NAM chain runs entirely on the host's audio thread.** `DualLaneExecutor` needs 96 samples or more (`RealtimeParallel.h:313-317`). Level parallelism needs `score × samples ≥ 1800`, which two NAMs at 64 samples miss by 8 (`SignalGraphExecutorInternal.h:141`). Multi-core is off by default in plugins.
  - That is the right call at live block sizes, because a helper wake costs 5-50 µs.
  - Helper priority only matters at 128 samples and up, or with several rigs.
- **Helpers:**
  - Windows: MMCSS "Pro Audio", falling back to TIME_CRITICAL (`RealtimeParallel.cpp:89-106`).
  - macOS: time-constraint policy.
  - Linux: SCHED_FIFO at the minimum priority. That fails silently without an rtprio limit, and the audio thread then spin-waits on an ordinary thread.
  - FTZ/DAZ is set on every pool worker.
- **macOS helpers never join the audio workgroup.** `PluginProcessorAdapter` does not override `audioWorkgroupContextChanged`, and JUCE supplies one for the standalone and AU/AUv3. On Apple silicon, threads outside the workgroup do not count toward the IO deadline when the system picks cores and clocks.
- **The standalone's own audio thread:**
  - WASAPI: JUCE sets MMCSS "Pro Audio".
  - ASIO: the driver sets it.
  - Linux ALSA: `SCHED_OTHER`. Raise it once from the first callback.
- **Waiting on helpers.** The audio thread spins for 10,000 pauses, then yields without bound (`RealtimeParallel.cpp:60, 443-455`). The spin is a count, and `pause` latency varies more than 10x across CPUs, so the budget runs from about 30 µs to 0.5 ms. Bound it by time instead.
- **Fork polyphase workers.** Our build never starts them (oversample factor 1). They have a latent lost-wakeup: `notify_all` without the mutex, and an untimed wait (`polyphase.h:172-174, 300`). Fix that before ever enabling them.

### 6. Real-time safety: spikes and pops rather than average load (P1/P2, confirmed)

- **A blocking mutex on every audio block.** `PluginProcessorAdapter::applyPendingDAWParamChanges` takes `std::lock_guard` on `mPendingDAWParamMutex` (`PluginProcessorAdapter.cpp:316`). Host `setValue` holds the same mutex across an `emplace_back` that can allocate (`:197-198`). That is priority inversion against the host's parameter thread. Use `try_lock`, or a fixed SPSC queue.
- **A busy DSP lock silences the whole block.** When `ProcessAudio` fails to take the DSP lock, the adapter calls `buffer.clear()` (`PluginProcessorAdapter.cpp:394-398`). Any message-thread holder of the lock produces an audible gap.
- **Riff capture builds JSON and dumps it on the audio thread.** `ProcessRiffCaptureBlock` (`PluginControllerRiffs.cpp:1482` onward) does this at trigger and every 250 ms while capturing, with a growing peaks array. Hand the numbers to the message thread through atomics instead.
- **Resampler `std::cerr`:** see finding 2.

### 7. Benchmark hygiene (P3)

- `NAMPerformanceBenchmark.cpp:172` calls `nam::get_dsp` directly. It never enables fast tanh, so its numbers overstate the standard model's cost by about 20%.
- Nothing measures the whole chain against a bare model. That is the comparison this review needed.

## Recommendations, in order

| # | Change | Expected gain | Effort |
|---|---|---|---|
| 1 | Run one side when the stereo input's sides are identical (finding 1a), plus a plugin input-mode override (1b) | About -50% CPU on stereo tracks, which is most DAW tracks | Medium: reuses re-resolve and fades; needs a decision on the "never from audio" rule |
| 2 | Real-time fixes: DAW-param `try_lock`/SPSC, remove the resampler `cerr`, move riff-capture JSON off the audio thread | Removes spike and inversion sources | Small |
| 3 | Lanczos A=32 → A=12 after an `audio-ab --rate 44100` check | About -10% at 44.1 kHz and 96 kHz | Small |
| 4 | Rebase the NAM fork onto current upstream | -5 to -7% on every model | Medium (fork maintenance) |
| 5 | `-mfma` (or `-march=x86-64-v3`) on the gcc/clang avx2 tier, plus an AVX2 check in the installer | 4-13% on Linux and Intel Mac | Small |
| 6 | Guard: NAM library never built with `/GL` or `-flto` | Prevents a 1.3-2x regression | Trivial |
| 7 | macOS: join helpers to the audio workgroup. Linux: raise the standalone audio thread, and refuse helpers when SCHED_FIFO fails. Time-bound the spin | Stability at 128 samples and up, and with several rigs | Small to medium |
| 8 | Framework trims: node timing on meter blocks only, skip a fully bypassed post-chain, mono-only gather, Transpose history rings | Up to about 1 µs per 64-sample block (about 5% of an A2 nano) | Small each |
| 9 | Commit a parity benchmark (bare model against the full chain; mono, stereo, 44.1 and 48 kHz), and fix `NAMPerformanceBenchmark`'s missing fast tanh | Keeps this answerable | Small |
| 10 | Experiment: build the NAM library with clang-cl on Windows | 1.56x on A2 nano, 1.11x standard, 0.96x A2 full | Medium |

## Not Verified

- Only Windows x64 was measured. macOS (arm64 and x86_64), Linux and Android are judged from their flags.
- No listening test was done for Lanczos A=12.
- Framework overhead was not profiled; finding 4 attributes it by reading.
- The p99 and maximum block times from these runs are not interpreted, because the machine was loaded.
- A simple plugin's shipped build may differ from upstream built with CMake defaults. The comparison here is against that plain build of the same core, with and without fast tanh.
