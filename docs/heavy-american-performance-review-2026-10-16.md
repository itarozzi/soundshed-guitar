# Heavy American Performance Review

**Date:** 16 October 2026
**Baseline:** `fabb3ea8` on `main` (the amp's headers are unchanged since `d85f717e`)
**Scope:** the CPU cost of the Heavy American (`amp_builtin`, `BuiltinAmpEffect.h` and its `BuiltinAmp*.h` helpers), where that cost goes, and what would make it cheaper.

## Status (16 October 2026)

Findings 1-4 are in `78221428` and finding 5 in `3147b865`. The resampler running a chunk at a time is applied on top (uncommitted). The amp now costs about a seventh of what it did before this review at its defaults, and an eighth at high gain. See [Finding 5 applied](#finding-5-applied) and [The resampler a chunk at a time](#the-resampler-a-chunk-at-a-time) below.

Findings 1-4, as applied in `78221428`:

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

### Finding 5 applied

The signal path runs a chunk of 64 host samples at a time, each stage over the whole oversampled chunk before the next.

| Change | Where |
|---|---|
| The controls step once per host sample through the chunk first, into per-sample arrays the loops read. Once a step changes nothing, the rest of the chunk holds its values | `StepControls` in `BuiltinAmpEffect.h`; `ChunkControls` |
| Each linear section is one loop with all its biquads per sample, their state in locals: pre-filters (2), tone stack (4), voicing, speaker filters and the 25 Hz high pass (6, the sag envelope between the 5th and 6th) | `RunBiquads` in `BuiltinAmpChunk.h` |
| A preamp stage's coupling filter runs inside the next linear section's loop where no other clipper or fade comes first: the last stage before the tone stack, and the last before the power stage | `LaterStage` in `BuiltinAmpRender.h` |
| Each clipper is a loop over the chunk, compiled once per knee kind (tanh, soft + tanh, tanh + hard, soft, hard), and vectorised | `ClipAs`, `PowerClipAs`, `ShapeEach` |
| While Character glides, the clippers fall back to a sample at a time; Power Drive and Bias glides only move the power clipper there | `ClipChunk`, `PowerClipChunk` |
| `KneeTanh` and `HardKneeShape` clamp with comparisons, `KneeTanh` tests `x * x` against 0.0004² instead of `std::abs(x)` against 0.0004, and the knees are force-inlined. The same results for every float (checked over all 2³²) | `BuiltinAmpVoicing.h` |
| The per-channel state (resamplers, filters, sag envelope), Reset and CopyChannelState move into one class | `SignalPath` in `BuiltinAmpRender.h` |

`BuiltinAmpEffect.h` is 646 lines (from 830), so its size pin is gone. `BuiltinAmpChunk.h` is 644 and `BuiltinAmpRender.h` 416.

**Measured** (the probe and flags above, interleaved, best of 8 rounds, µs per block):

| Config | `78221428` | Chunked | Speed-up |
|---|---|---|---|
| defaults | 17.28 | 8.15 | 2.1x |
| Voice 1 | 17.60 | 8.16 | 2.2x |
| high gain, 4 stages | 26.54 | 9.24 | 2.9x |
| Character 0 / 0.25 / 1 | 15.23 / 18.38 / 15.26 | 8.04 / 8.24 / 7.98 | 1.9-2.2x |
| Power Drive, Sag, Bias | 20.42 | 9.78 | 2.1x |
| stereo, defaults | 33.87 | 16.20 | 2.1x |
| stereo, high gain | 52.36 | 18.45 | 2.8x |
| Gain moved every block | 23.87 | 10.62 | 2.2x |
| Character moved every block | 29.52 | 18.12 | 1.6x |

clang-cl (`-ffast-math -mavx2 -mfma`): defaults 13.48 → 8.04 µs, high gain 22.10 → 9.35.

Against the review's baseline (33.7 / 50.4 µs), the amp now costs 24% at its defaults and 18% at high gain. Against a standard NAM model (about 57 µs) that is 0.14x and 0.16x, as the timing prototype predicted.

**Where the time goes now**, µs per block at the defaults (high gain where it differs):

| Piece | µs |
|---|---|
| 4x resampling, up and down | 3.7 |
| Voicing, speaker filters and post high pass, with the last stage's coupling | 1.7 |
| Tone stack, with stage 2's coupling | 0.9 |
| Stage one's coupling, standing alone because stage 2's clipper follows | 0.7 (and stage 3's at high gain) |
| Pre-filters | 0.7 |
| Clippers | 0.14 each |
| Controls | 0.2 |

The resampler is now 45% of the cost. A section's loop is bound by its filters' recursions, which overlap from one filter to the next, so the 6-biquad section, with the sag envelope and a coupling filter in its loop as well, costs 2.3 times the 2-biquad one.

**What MSVC needed.** The prototype measured the shape. Getting the compiler to produce it took four more things, each measured:
- **Coefficients read from memory, only the state in locals.** With all 20 tone-stack coefficients in registers too, MSVC spilled the filter state to the stack, putting a store and a reload on every recursion. Tone stack 1.37 → 0.94 µs.
- **No `std::clamp` or `std::abs` in the knees, and the knees force-inlined.** MSVC will not vectorise a loop over `std::clamp` (it returns a reference) or one that tests `std::abs`, and it called `KneeTanh` rather than inline it. The soft and tanh knees also have to be two loops, or neither vectorises. Stage-one clipper 0.83 → 0.15 µs.
- **The controls stop stepping once settled.** Stepping all of them through every host sample cost 0.6 µs a block; now 0.2.
- **Block-size invariance.** The output of a sample-at-a-time amp cannot depend on how the host splits its blocks, and `TestDynamicsAndBlocks` checks that this still holds to 1e-6. It failed in RelWithDebInfo, and the probe found three causes:
  - A vectorised loop works out the last few samples one at a time, and which ones depends on where the chunk ends. Under `/fp:fast` MSVC estimates a vector division (`vrcpps` and one Newton step) and divides a scalar exactly; clang does the same for the soft knee's square root (`vrsqrtps`). The clip loops now compile with `#pragma float_control(precise)` and always run whole lanes of 16, the extra samples unused.
  - A section that ran for gliding coefficients and for settled ones in different loops compiled each with different arithmetic. Now a chunk glides one host sample at a time only while the coefficients move, then finishes in the other loop, so which loop a sample runs in does not depend on the chunk.
  - The sample-at-a-time fallback while Character glides now inlines the same knees as the clip loops.

  Probe: the same playing rendered in blocks of 47, 100, 256, 1 and 17 against 64. With the controls still, the output is identical, under MSVC and clang. With Gain, Character, Voice, stages and Power Drive moving at fixed times, the worst difference is 3e-8 (-133 dB) under MSVC and 7e-7 (-105 dB) under clang, from Character's fallback against the vector lanes' estimates. `78221428` gives 0 in every case.

**Verification:**
- **Output against `78221428`, the probe built with `/fp:precise`:** bit-identical in all 26 scenarios. The 13 timing configs each continue with Gain, Character, stages, Treble, Power Drive and Voice moved. Then:
  - 96, 192 and 44.1 kHz, plain and high gain;
  - blocks of 1 to 256, some longer than a chunk;
  - Preamp Stages stepped both ways, including a change landing mid-fade;
  - Voice switched both ways, with Bright and Pre Emphasis;
  - Power Drive gliding to 0 and back, with Sag, Bias and Character moving;
  - every tone and speaker control, Input Trim and Output;
  - Gain, Character, Presence and Bias moved every block;
  - stereo with different sides, flipping to mono and back;
  - the nominal level moved.
  
  So the chunked path does the per-sample path's arithmetic exactly; only the compilers' fast-math choices move it.
- **The same with the shipping flags:** RMS -107 to -115 dB against the output, peaks -73 to -100 dB. That is what `78221428` differs from itself by between `/fp:fast` and `/fp:precise` (-107 to -115 dB, peaks -73 to -99) or between MSVC and clang (-107 to -115 dB). clang against clang: -111 to -118 dB.
- **Debug CMake build** of this worktree: `BuiltinAmpEffectTests`, `BuiltinAmpLegacyMigrationTests`, `EffectFactoryPresetTests`, `EffectProcessorTests` and `FastMathNanTests` pass. Every figure `BuiltinAmpEffectTests` prints is identical to the `78221428` build's. The suite runs in 43 s instead of 66.
- **RelWithDebInfo** (`/fp:fast`): `FastMathNanTests` and `BuiltinAmpEffectTests` pass, including the mono path's exact match and the block partition. The aliasing check reads -131.7 dBc.

**Not done, and found on the way:**
- **The 4x resampler** is now 3.7 of 8.2 µs. `Upsample` and `Downsample` still run a sample at a time, each output a 32-tap dot product ending in a horizontal sum. A polyphase FIR over the whole chunk, vectorised across outputs, would cost a few multiply-adds per output. The shorter inner half-band in 6 is the other lever. Since done: see [The resampler a chunk at a time](#the-resampler-a-chunk-at-a-time).
- **Glides stopped short of their targets: fixed** (on top of `be62efeb`). `Glide` snapped within 1e-6, but a float step of 0.2% of the remaining distance stops changing anything about 1.4e-5 away. Voice rested at 0.999986, Character at 0.499993 or 0.500014, Gain at 0.899986 for 0.9. So after a live Voice switch both legs of stage one were clipped and crossfaded for good, and after Character moved back to 0.5 two knees ran for good: finding 2's skip and the single-knee path only applied after a Reset or preset load.
  - `Glide` now also lands once a step changes nothing. Input Trim, Sag and Output, which stepped with a bare `+=` and stalled the same way, go through it too. Every glide lands within 94-138 ms at 48 kHz (80-138 ms at 192 kHz), under MSVC `/fp:fast` and `/fp:precise` and clang-cl `-ffast-math`.
  - `liveVoice1` and `liveChar05` now cost what `voice1` and `default` do: 7.61 and 7.60 µs against 7.60 and 7.59, from 7.77 and 7.73 (interleaved, best of 8, on a faster day for the machine than the table above).
  - The output changes only after a move: -84 to -115 dB RMS against the output, peaks -61 to -96 dB, the most at 192 kHz, where the stall is four times as far. Without a move it is bit-identical. Every figure `BuiltinAmpEffectTests` prints is unchanged, in Debug and RelWithDebInfo.
  - `BuiltinAmpGlideTests` moves Voice, Gain, Power Drive, Sag, Input Trim and Output live during silence, then checks that the amp plays bit for bit like one set there, at 48 and 192 kHz. 12 of its 14 checks fail against the old `Glide`.
  - The filter corners still stop short. `StageFilter` and `GlidingBiquad` settle where a step changes nothing (finding 3), which is short of the target. Nothing skips work on them, so it costs no time, but after a live Character move the amp differs from one set there by up to 7e-4 on a 0.1 sine at 48 kHz. Nearly all of that is `StageFilter`'s float corners: 4e-7 once those snap too.
- **The controls while they move.** Moved every block, Gain spends 2.5 µs a block in the controls and Character 6.1. The level makeup, and for Character `Clippers::SetCharacter`, run every host sample while they glide, as they always did.
- The unmeasured items in 6.

### The resampler a chunk at a time

Each half-band stage takes a whole chunk in one call, and works out its odd phase across the chunk's outputs instead of one output at a time.

| Change | Where |
|---|---|
| `UpsampleBlock` (n samples in, 2n out) and `DownsampleBlock` (2n in, n out) replace the per-sample `Upsample` and `Downsample`. `SignalPath` makes one call per stage per chunk, the 2x signal between the stages in `mTwice` | `BuiltinAmpOversampling.h`, `BuiltinAmpRender.h` |
| Each direction keeps a line instead of a mirrored ring: the last 32 samples, then the block's own. Every sample a block's outputs need is in a row, and the next block keeps the last 32 | `BuiltinAmpHalfband2x` |
| The odd phase runs eight taps a pass over all the block's outputs, so each output costs one multiply-add per tap and no horizontal sum. Each output still adds its taps in order from lag 0, from 0.0f, as the dot product did | `OddPhase` |
| Outputs are worked out in whole groups of 16, the ones past the block's end unused, so none falls to a loop's scalar remainder | `WholeGroups` |

The latency (48 host samples at 4x, 32 at 2x), the 2x and 1x paths at 96 and 192 kHz, `Reset` and `CopyChannel` are unchanged. `TestHalfbandLatency` now drives the block calls, and checks that blocks of 1, 7, 17 and 47 give the same impulse response as blocks of 64, exactly.

**Measured** (the same probe and flags, interleaved, best of 8 rounds, µs per block; "Chunked" is finding 5 as above, measured again here):

| Config | Chunked | Resampler in chunks | Speed-up |
|---|---|---|---|
| defaults | 8.17 | 5.02 | 1.6x |
| Voice 1 | 8.15 | 5.02 | 1.6x |
| high gain, 4 stages | 9.25 | 6.10 | 1.5x |
| Character 0 / 0.25 / 1 | 8.07 / 8.29 / 7.99 | 4.90 / 5.17 / 4.87 | 1.6x |
| Power Drive, Sag, Bias | 9.71 | 6.51 | 1.5x |
| stereo, defaults | 16.06 | 9.88 | 1.6x |
| stereo, high gain | 18.31 | 11.89 | 1.5x |
| Gain moved every block | 10.57 | 7.49 | 1.4x |
| Character moved every block | 18.40 | 15.27 | 1.2x |

clang-cl: defaults 7.85 → 5.04 µs, high gain 9.09 → 6.26, stereo 15.78 → 9.93.

The resampler, up and down, went from 3.8 to 0.6 µs a block at the defaults (phase counters in the probe): from 9.8 ns per 32-tap output to 1.6. Against the review's baseline (33.7 / 50.4 µs), the amp now costs 15% at its defaults and 12% at high gain. Against a standard NAM model (about 57 µs) that is 0.09x and 0.11x.

**How the odd phase is written.** Each kernel was timed in the amp at the defaults, in a race of their own, so compare them within the table (MSVC / clang-cl, µs per block):

| Kernel | µs |
|---|---|
| one tap a pass over the outputs | 5.37 / 5.39 |
| four taps a pass | 5.22 / 5.12 |
| **eight taps a pass** | **4.94 / 5.11** |
| sixteen taps a pass | 5.22 / 5.17 |
| 16 outputs at a time, their sums held in registers through all 32 taps | 6.49 / 5.52 |

One tap a pass loads and stores every sum once per tap; eight taps a pass, once per eight. Holding the sums in registers is slowest: each output's sum is then a chain of 32 dependent multiply-adds, and 16 outputs is only two vectors to overlap them with. More outputs at a time would hide that, but would need a bigger group, and so more unused outputs for short blocks.

**Verification:**
- **Output against finding 5, the probe built with `/fp:precise`:** bit-identical in all 26 scenarios, under `/arch:SSE2` and under `/arch:AVX2`.
- **The same with the shipping flags:** RMS -108 to -117 dB against the output, peaks -78 to -100 dB. Finding 5's code differs from itself between `/fp:fast` and `/fp:precise` by the same: RMS -108 to -117 dB, peaks -78 to -100. clang against clang: RMS -115 to -128 dB.
- **Block-size invariance**, the probe's partition (blocks of 47, 100, 256, 1 and 17 against 64): with the controls still, identical under MSVC, clang and `/fp:precise`. With the controls moving, the worst difference is 3e-8 (-133 dB) under MSVC and 6e-7 (-107 dB) under clang, both from Character's fallback as before; finding 5 gave 3e-8 and 7e-7.
- **Debug CMake build** of this worktree: `BuiltinAmpEffectTests` (with `TestHalfbandLatency`, `TestAliasing` and `TestDynamicsAndBlocks`), `BuiltinAmpLegacyMigrationTests`, `EffectFactoryPresetTests`, `EffectProcessorTests` and `FastMathNanTests` pass. Everything `BuiltinAmpEffectTests` prints is byte-identical to finding 5's build, the aliasing check's -131.1 dBc included.
- **RelWithDebInfo** (`/fp:fast`): the same five pass, including the mono path's exact match and the block partition. Its printed figures move in the fifth or sixth digit against finding 5's RelWithDebInfo build. The aliasing check reads -130.5 dBc against -131.7, the float-rounding jitter described under 1-3.

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

Applied: see [Finding 5 applied](#finding-5-applied). The real version reaches 8.2 µs at the defaults and 9.2 at high gain.

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
3. **Then decide on block rendering (5).** It is the step that makes the amp a small fraction of a NAM model at every setting. It needs a real implementation and a full test pass. Done; see Status.

Against NAM: a standard model costs about 57 µs per block. That figure was measured through the executor on 2026-09-21; the amp figures here are the bare effect, which the executor adds a little to.

| | Defaults | High gain |
|---|---|---|
| Today | 0.58x | 0.88x |
| After 1-3 | 0.29x | 0.59x |
| After 1-4 | 0.29x | 0.45x |
| With 5, predicted | 0.14x | 0.19x |
| With 5, measured | 0.14x | 0.16x |
| With the resampler in chunks, measured | 0.09x | 0.11x |
