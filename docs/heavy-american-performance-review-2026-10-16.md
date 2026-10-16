# Heavy American Performance Review

**Date:** 16 October 2026
**Baseline:** `fabb3ea8` on `main` (the amp's headers are unchanged since `d85f717e`)
**Scope:** the CPU cost of the Heavy American (`amp_builtin`, `BuiltinAmpEffect.h` and its `BuiltinAmp*.h` helpers), where that cost goes, and what would make it cheaper.

## Status (16 October 2026)

Findings 1-4 are applied (uncommitted):

| # | Change | Where |
|---|---|---|
| 1 | Polyphase half-band with mirrored rings | `BuiltinAmpOversampling.h` |
| 2 | No power clipper at Power Drive 0; only the heard Voice leg is clipped | `BuiltinAmpEffect.h` (`ProcessAmpSample`) |
| 3 | Coefficient glides stop once a step changes nothing; designs go through `SetTarget` | `BuiltinAmpFilters.h`, `BuiltinAmpEffect.h` |
| 4 | `KneeTanh`, a 13/6 rational within 5 ulp of tanh (2.8e-7), replaces `std::tanh` in the knees | `BuiltinAmpVoicing.h` |

Measured the same way as below, HEAD's headers against the applied ones, interleaved on a quiet machine, µs per block:

| Config | HEAD | Applied | Speed-up |
|---|---|---|---|
| defaults | 33.7 | 17.2 | 2.0x |
| Voice 1 | 35.0 | 17.5 | 2.0x |
| high gain, 4 stages | 50.4 | 26.6 | 1.9x |
| Character 0 / 0.25 / 1 | 29.1 / 35.4 / 29.5 | 15.2 / 18.3 / 15.6 | 1.9x |
| Power Drive, Sag, Bias | 34.5 | 20.3 | 1.7x |
| stereo, defaults | 64.8 | 34.0 | 1.9x |
| stereo, high gain | 98.3 | 52.3 | 1.9x |

Against a standard NAM model (about 57 µs) the amp is now 0.30x at its defaults and 0.47x at high gain.

The applied code runs 4-5% slower than the scratch prototype of the same changes, in every config. Aligning the resampler's arrays made no difference, and a later run was too noisy (another build was using the machine) to look further. It is likely code layout. It is not chased.

**Verification:**
- **Output against HEAD:** -113 to -116 dB RMS in every config, including the mid-stream moves. Peaks are -76 to -87 dB against the output's RMS.
- **Debug CMake build** (`C:\t\lyb`, configured on this checkout): these pass.
  - `BuiltinAmpEffectTests` and `BuiltinAmpLegacyMigrationTests`;
  - `BiquadNyquistTests`, `ChannelLayoutTests`, `DriveLegacyMigrationTests` and `EffectFactoryPresetTests`;
  - `IdenticalInputFoldTests`, `ModulationStereoTests`, `FastMathNanTests` and `EffectProcessorTests`.
- **RelWithDebInfo build** (`/fp:fast`) in the same tree: `FastMathNanTests` and `BuiltinAmpEffectTests` pass.

In the Debug build, the returning-stage "just before" figure is back to 0. The aliasing check reads -131.1 dBc.

Not done: 5 (block rendering) and the unmeasured items in 6.

## Summary

The amp costs 33 µs per 64-sample block at its defaults and 50 µs at four stages and high gain, mono at 48 kHz. The deadline at that block size is 1333 µs, so that is 2.5% and 3.8% of it. A standard NAM model costs about 57 µs per block on the same machine, so at high gain the amp is nearly as expensive.

About half of that is overhead, not modelling:

- **The 4x resampler is 40% of the cost** because of how it is written, not what it computes. It wraps every tap with a modulo, and the decimator walks all 65 taps of which 32 are zero.
- **Work whose result is thrown away.** The power clipper's tanh and two divides run every oversampled sample even at Power Drive 0 (the default), where the result is multiplied by zero. Both Voice legs are clipped even though Voice is a switch.
- **The tone filters' coefficient glides run forever.** They step every sample even once settled.

Fixing those three changes the sound by float rounding at most, and halves the cost: 33.3 → 16.5 µs at the defaults, 50 → 34 µs at high gain, 65 → 32 µs in stereo. The amp's own test suite passes with them.

After that, tanh dominates at high gain (51%). The preamp stages are one serial chain per oversampled sample, so it cannot vectorise as written.
- A rational tanh is transparent and takes high gain 34 → 26 µs.
- Rendering a block at a time per stage lets the clippers vectorise across time and the filters stay in registers. A timing prototype of that reaches 8.2 µs at defaults and 10.7 µs at high gain: 4x and 4.7x faster than today.

## Method

A standalone probe compiles the amp headers directly with the shipping core's flags (MSVC `/O2 /Ox /fp:fast /arch:AVX2`). Variants are shadow copies of the headers with each change behind a macro, so every change is timed alone and together. They were built from the same probe source and run interleaved at High priority on one pinned core, taking the best of 15 rounds. The machine is the 8845HS dev PC.

- **Input:** 6 s of plucked-string synthesis over two octaves with palm-muted chugs, at -18 dBFS RMS. ProcessMono with 64-sample blocks at 48 kHz, so the amp oversamples 4x.
- **Configs:**
  - defaults;
  - Voice 1;
  - "high gain": Voice 1, Gain 0.8, 4 stages;
  - Character 0, 0.25 and 1;
  - Power Drive 0.6 with Sag 0.5 and Bias 0.3;
  - stereo `Process` at the defaults and at high gain.
- **Output checks:** every config was rendered, then moved mid-stream (Gain, Character, stages, Treble, Power Drive, Voice) so the glides and stage fades are exercised too. Each variant was compared sample by sample against the baseline.
- **Attribution:** knockout builds replace one piece with a near-free stand-in (tanh with a clamp, a biquad with a gain, the resampler with a copy). The time each saves is that piece's cost.
- **Tests:** `core/tests/BuiltinAmpEffectTests.cpp` was compiled against the shadow headers (with `EffectRegistry.cpp` and the audio decoder) and run in full for each variant.
- **Second compiler:** clang-cl with `-ffast-math -mavx2 -mfma` repeated the timings, as a stand-in for the macOS, Linux and Android compilers.

## Where the time goes

| Piece | Baseline, ns per host sample | Notes |
|---|---|---|
| 4x up and down resampling | 203 | Three `Upsample` and three `Downsample` calls per host sample. Each tap pays a `%` by 33 or 65; the decimator branches on 65 taps, 32 of them zero |
| Clippers (tanh) | ~150 | 16 `std::tanh` calls per host sample at the defaults. Each is a scalar call, about 9 ns on small inputs and 17 ns on the large ones a high-gain stage sees, on the serial stage chain |
| 12 double-precision biquads, 4x | 55 | Latency-bound: each sample waits for the filter before it |
| Coefficient glides | 8 | 60 double multiply-adds per host sample, settled or not |
| Everything else | ~100 | Control glides, stage filters, sag, member state reloaded every call |

Per host sample the defaults run 48 biquads and 16 tanh calls, 4 of them in the unused power clipper and 4 in the unheard Voice leg.

## Findings

### 1. The resampler pays for its indexing, not its taps

`BuiltinAmpHalfband2x` (in `BuiltinAmpOversampling.h`):
- keeps its histories in rings of 33 and 65, and indexes every tap with `%`;
- runs the decimator over all 65 taps with a `!= 0` branch, although the odd half-band taps are zero except the centre;
- cannot vectorise either loop.

**Change:** polyphase with mirrored rings.
- The even phase is a pure 16-sample delay.
- The odd phase is a 32-tap dot product over a contiguous window.
- Each sample is written twice, 32 apart, so the newest 32 are always in a row.
- The interface is the same and so are the taps; only the order of summation moves.

**Measured:** defaults 33.3 → 23.3 µs, high gain 50.1 → 40.2 µs. Output differs from today's by float rounding: -117 dB RMS against the output's RMS, peaks -85 to -90 dB. `TestHalfbandLatency` and `TestAliasing` pass.

**Effort:** about 60 lines in one header.

### 2. Dead work at the most common settings

- **The power clipper at Power Drive 0.**
  - What runs: `PowerClip` (a divide for the sag headroom, one in the clipper, and a tanh) every oversampled sample, which is then multiplied by `mPowerDriveSmoothed`. That is 0 by default, and the glide lands on exactly 0.
  - Change: skip the clip when the drive is 0, and keep the sag envelope following, so turning Power Drive up later starts from the same envelope.
  - Output: bit-identical.
- **Both Voice legs.**
  - What runs: the clean and drive legs of stage one are both clipped every sample and crossfaded by the smoothed Voice. Voice snaps to 0 or 1, so outside its glide one leg is multiplied by zero.
  - Change: clip only the leg that is heard.
  - Output: bit-identical at Voice 0; at Voice 1, `clean + (drive - clean)` is not exactly `drive` in float, so it differs by rounding (-117 dB RMS).

**Measured:**
- defaults: 33.3 → 28.4 µs, with both skips;
- high gain: 50.1 → 46.0 µs, with both;
- the Power Drive config: 34.6 → 31.2 µs, with the Voice skip alone.

**Effort:** about 25 lines in `ProcessAmpSample`.

### 3. Coefficient glides never stop

`AdvanceFilters` steps all 12 `GlidingBiquad`s (5 doubles each) and the 4 `StageFilter`s every host sample. Once a step leaves every coefficient unchanged, every later step would too, since the inputs are the same, so a filter can mark itself settled until its next design.

**Change:** add a `settled` flag, cleared by a new `SetTarget()`, which replaces the direct `.target =` writes.

**Measured:** about -1 µs per block. Bit-identical in every config, including the mid-stream moves.

**Effort:** about 30 lines.

### 1-3 together

| Config | Baseline µs/block | 1-3 µs/block | Speed-up |
|---|---|---|---|
| defaults | 33.3 | 16.5 | 2.0x |
| Voice 1 | 34.8 | 19.1 | 1.8x |
| high gain, 4 stages | 50.1 | 33.8 | 1.5x |
| Character 0 / 0.25 / 1 | 28.9 / 35.0 / 29.2 | 14.2 / 17.4 / 14.1 | 2.0x |
| Power Drive, Sag, Bias | 34.6 | 19.4 | 1.8x |
| stereo, defaults | 65.2 | 31.9 | 2.0x |
| stereo, high gain | 97.7 | 67.8 | 1.4x |

clang-cl shows the same: defaults 31.1 → 14.4, high gain 48.5 → 31.9.

All of `BuiltinAmpEffectTests` passes. Its printed measures (THD per preset, Character spectra, level holds, sag spans, mono path 0 difference) agree with today's to the printed precision. Two figures moved:
- The aliasing check's folded harmonic reads -130.4 dBc against -132.7. This figure jitters by ±2 dB with any change at float-rounding level: a build with only changes 2 and 3, which never touch the resampler, reads -131.0. It is the decimator's float noise floor, not aliasing.
- `TestReturningStageStartsFresh` reports 7.45e-9 "just before" against 0. That is one float ulp of a 0.1 output, and appears only with the changes combined; each alone gives 0. Its checked "after" figure is unchanged at 1.49e-8.

### 4. tanh is half the high-gain cost

With 1-3 in, the knockouts split the remaining time:

| Config | Total µs/block | tanh | Biquads | Resampler | Rest |
|---|---|---|---|---|---|
| defaults | 16.7 | 4.7 | 4.8 | 3.3 | ~4 |
| high gain | 34.4 | 17.7 | 5.5 | 3.3 | ~8 |

`std::tanh` costs more on the large inputs a high-gain stage sees. Each stage's tanh waits on the stage before, so the calls can't overlap.

**Change:** a 13/6 rational (the usual float tanh approximation, clamped at ±7.9) in `KneeBlend::Shape` and `Slope`.
- **Measured:** high gain 34.4 → 25.6 µs, Voice 1 19.1 → 16.8; no change at the defaults, where `std::tanh`'s small-input path is already about as fast.
- **Output:** -113 dB RMS against `std::tanh`, the same order as float rounding. The tests pass.
- It also has no library call, so clang and gcc can vectorise it, which they can't do with `std::tanh` (see the NAM notes); finding 5 needs that.
- clang-cl: high gain 31.9 → 21.8 µs.

### 5. Block-at-a-time rendering

`Render` runs the whole chain one oversampled sample at a time. That keeps every filter's state in member arrays and serialises every clipper behind the one before it. The clippers have no memory, though: run a block at a time per stage, each one is a loop that vectorises across time. Each linear section can then run as one loop with its biquads' state in locals. The sections are pre-filters, tone stack and post-preamp/speaker filters.

**Prototype:** timing only. It covers the same chain with settled controls, mono, 4x and the tanh knee, and matches the effect's output to -112 dB.

| | defaults | high gain |
|---|---|---|
| 1-4 in the current structure | 17.1 µs | 26.5 µs |
| block per stage, one loop per biquad | 14.7 µs | 17.2 µs |
| block per stage, one loop per linear section | **8.2 µs** | **10.7 µs** |
| the same under clang-cl | 8.1 µs | 9.3 µs |

One loop per *biquad* loses the overlap that sample order got for free: each filter waits on its own recursion for 256 samples. One loop per *section* keeps that overlap and adds register-resident state.

**What a real version needs:**
- Controls (gain, Voice, makeup, fades, Power Drive, Sag) glide per host sample. Precompute each block's per-sample values into small arrays the stage loops index.
- Coefficient glides still step once per host sample, inside the section loop, while a section is unsettled.
- A Character glide changes the knee and the clip offsets per host sample. It is rare, so a per-sample fallback for the clip loops is acceptable while it moves.
- Sag's envelope is serial but cheap: one loop builds it, then the power clip vectorises.
- `CopyChannelState` and the mono/stereo paths follow the new state layout. `TestMonoPath` already guards that.

**Effort:** a rewrite of `Render` and `ProcessAmpSample`, likely into a new `BuiltinAmpRender.h` to respect the 800-line budget. A day's work with the tests. It is not bit-identical, but would be at the rounding level shown above.

### 6. Not measured

- **A shorter inner half-band.** The 2x → 4x stage only has to clear images of the host band, so its transition band is wide. A 21- or 25-tap filter could replace the 65-tap one.
  - Saving: perhaps 1.5 µs per block.
  - Latency: 48 → 37 or 38 host samples.
  - Cost: the response changes, so `TestAliasing` and a listen decide it.
- **Pre-filters and the 25 Hz post high-pass at host rate.**
  - The pre high-pass, the pre-emphasis shelf and the input trim are linear and come before the first clipper. The post high-pass and output gains come after the last one.
  - Saving: 9 of the 48 biquad runs per host sample, about 1 µs.
  - Cost: the shelf's response near Nyquist moves with the bilinear warp.
- **Stereo across SIMD lanes.** True stereo runs every channel's identical work twice. Identical sides already fold to mono upstream (`IdenticalInputDetector`), so this only helps genuinely different sides, which an amp rarely gets.
- **ADAA and 2x oversampling.** It would halve nearly everything, as the drive pedals' rework did, but it is a voicing change with its own calibration (level tables, presets) and is out of scope here.

## Recommendation

1. **Apply 1-3 now.** Two to three hours with a header split for the line budget. The output is the same to float rounding, the tests pass, and the amp gets 1.5-2x cheaper. Re-run `BuiltinAmpEffectTests` and `BuiltinAmpLegacyMigrationTests` in the real build, and include the factory preset level check.
2. **Add the rational tanh (4) with them** if a -113 dB difference is acceptable. It is a one-function change and the high-gain win is 25%.
3. **Then decide on block rendering (5).** It is the step that makes the amp a small fraction of a NAM model at every setting. It needs a real implementation and a full test pass.

Against NAM: a standard model costs about 57 µs per block. That figure was measured through the executor on 2026-09-21; the amp figures here are the bare effect, which the executor adds a little to.

| | Defaults | High gain |
|---|---|---|
| Today | 0.58x | 0.88x |
| After 1-3 | 0.29x | 0.59x |
| After 1-4 | 0.29x | 0.45x |
| With 5 | 0.14x | 0.19x |
