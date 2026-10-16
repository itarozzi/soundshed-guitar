# NAM DSP Performance Review

**Date:** 16 October 2026
**Baseline:** `2a86bc7e` on `main`, NAM core pinned at `DLC86/NeuralAmpModelerCore@bea723b`
**Scope:** the NAM model itself: the inference loops, model loading and prewarm, and the wrapper around
them. The chain around the amp, thread priorities and compiler flags were reviewed on 14 October
([dsp-performance-review-2026-10-14.md](dsp-performance-review-2026-10-14.md)). This review starts from
those numbers.
**Tools:** [tools/nam-benchmark](../tools/nam-benchmark/README.md). It holds the harness and
`FusedWaveNet.h`, the prototype every "fused" figure below comes from.

## Executive Summary

The NAM core spends most of its time getting to the arithmetic rather than doing it. Whole models run
at 13-16 GMAC/s on the test machine, about a fifth of what one core can do. The 6- and
4-channel layers run at 3.7-4.9 GMAC/s. That is why a "lite" model costs 92% as much as a standard one with
half the arithmetic, and why a "feather" costs half as much with a fifth of it.

A prototype fused forward pass, written for this review, takes every NAM model on the test machine:
2,460 models in 1,666 model files, both submodels of every A2 file included. It runs them 2.0-4.8x
faster:

| Model (share of files on the test machine) | NAM core today, ns per sample | Fused | Speed-up |
|---|---|---|---|
| A2 full, the app's default size (47%) | 786 | 279 | 2.8x |
| Standard 16/8 (47%) | 821 | 303 | 2.7x |
| A2 nano | 307 | 87 | 3.5x |
| xSTD, 4 × 8 channels (3%) | 882 | 293 | 3.0x |
| Lite 12/6 | 754 | 186 | 4.1x |
| Feather 8/4 | 445 | 96 | 4.6x |
| 32/8 | 3774 | 1878 | 2.0x |

All figures are at 48 kHz with 64-sample blocks, MSVC with the shipping flags and fast tanh on; feather
and 32/8 come from a separate run of the same harness. The fused output is the NAM core's to within
float rounding: 108-137 dB SNR. It is bit-identical whatever the host block size, which the NAM core's
is not.

Loading is the second finding. A model-cache hit still costs 7-46 ms to build a model. The prewarm in
our `Prepare` adds 2-37 ms. Both land on every preset switch, for every model and channel. The fused
path builds from parsed data in 0.03-0.15 ms. A WaveNet has no feedback, so its silent steady state can
be written in one pass rather than computed over thousands of samples. That takes its prewarm to
0.0-0.4 ms, and leaves the same state, bit for bit.

Recommendation 1 is to move the prototype into the core behind the model cache. At 64 samples, a mono
standard-model chain would go from about 58 µs per block to about 25.

## Method

- **Machine:** Ryzen 7 8845HS (Zen 4), Windows 11. MSVC from VS 18, and clang-cl 22 from the same
  install. About 13% background load.
- **Harness:** `tools/nam-benchmark`.
  - It compiles the fetched NAM core with the core's Release flags: `/Ox /fp:fast /arch:AVX2`, or
    `-O3 -ffast-math -mavx2 -mfma` under clang-cl, with `NAM_ENABLE_A2_FAST=1` and fast tanh on.
  - It pins itself to one core at High priority, with FTZ/DAZ set.
  - It runs 1 s of a guitar-like signal, with the models interleaved, and keeps the best of 4-7 rounds.
- **Models:** one per shape, from `resources/`, `core/tests/testdata` and the local library. Shape
  counts come from all 1,672 `.nam` files on the machine: the repo, the app's library and a model
  folder on a second drive. The C++ tools could not open two of them, whose paths run past 260
  characters, so the coverage figures cover the other 1,666 model files.
- **Accuracy:** SNR of the fused output against the NAM core's, on the same input from the same reset.
  For scale, the NAM core disagrees with itself by 106-146 dB between host block sizes.

## Where the time goes today

### The model shapes in use

| Shape | Files | What runs it now |
|---|---|---|
| A2 slimmable container: 23 layers, LeakyReLU, kernels 6 and 15, 16-tap head; 3-channel nano and 8-channel full submodels | 794 | The fork's `A2FastModel`: fixed-size Eigen GEMMs per tap |
| Standard: 16 then 8 channels, 10 layers each, kernel 3, Tanh | 790 | Generic WaveNet: dynamic-size Eigen, about 10 passes per layer |
| xSTD: four 8-channel arrays, kernels 6 and 15 | 54 | Generic |
| Lite 12/6, feather 8/4, 32/8, a 96 kHz 16/8, a 10-channel model | 29 | Generic |
| A lone A2-shaped WaveNet, not in a container | 1 | `A2FastModel` |
| Not NAM models: another product's patch exports saved as `.nam` | 4 | Neither |

`NAM_USE_INLINE_GEMM` is off in our build, so the fork's hand-unrolled small-matrix paths never run.
They covered 2-8 channels only.

### One standard layer, step by step (`parts.exe`, 64-sample block)

| Step | 16 channels | 8 channels | 6 channels |
|---|---|---|---|
| Dilated conv: 3 Eigen GEMMs, one per tap | 1987 ns (57%) | 788 ns | 1133 ns |
| 1x1 conv | 822 ns (23%) | 339 ns | 447 ns |
| Fast tanh | 218 ns (6%) | 105 ns | 76 ns |
| Input mixin (1 → C, as a GEMM) | 186 ns (5%) | 133 ns | 205 ns |
| Sum, head accumulate, residual | 291 ns (8%) | 121 ns | 93 ns |
| Whole layer | 3504 ns, 19 GMAC/s | 1487 ns, 11 GMAC/s | 1954 ns, 4.9 GMAC/s |

A 256-sample block does no better: 18.7 GMAC/s at 16 channels. So the cost is not call overhead that
bigger blocks would amortise. Eigen's general kernel packs both operands for every product. At depth
16 or less, and 8 or 6 rows, the packing and edge handling dominate. Each step is also a separate pass
over memory.

## Findings

### 1. A fused, frame-vectorised forward pass is 2.0-4.8x faster (P1, prototyped and measured)

**How the prototype works** (`tools/nam-benchmark/FusedWaveNet.h`):

- **Layout.** Each activation buffer holds one contiguous time row per channel, so the SIMD lanes run
  across 8 frames. Any channel count fills them, 6 and 12 as well as 8 and 16.
- **One kernel for every product.** The dilated conv, 1x1, rechannel and head all run through a single
  register-tiled kernel: 4 outputs × 16 frames, broadcast weights, FMA. A K-tap conv is that kernel over
  K × C input rows, each a pointer into a ring.
- **Rings.** Power-of-two, with a mirror of the first block, so every tap's window is contiguous. There
  is no rewind copy.
- **One pass per layer** for bias, mixin, activation and head sum.
- **Coverage.** It takes any WaveNet whose layers are non-gated Tanh or LeakyReLU, with layer1x1 on and
  no head1x1, FiLM, grouping, condition DSP or post-stack head. Any channel count, kernel, dilation and
  head kernel. Anything else returns nullptr and stays on the NAM core.

**Speed** (ns per sample; the speed-up against today's path in brackets):

| Model | MSVC AVX2, 64 | clang-cl AVX2, 64 | MSVC, 16 | MSVC, 256 |
|---|---|---|---|---|
| Standard | 303 (2.7x) | 291 (2.5x) | 395 (2.9x) | 333 (2.6x) |
| A2 full | 279 (2.8x) | 237 (3.6x) | 389 (3.0x) | 247 (3.0x) |
| A2 nano | 87 (3.5x) | 82 (2.4x) | | |
| Lite | 186 (4.1x) | 164 (4.8x) | 259 (4.2x) | 165 (4.3x) |
| xSTD | 293 (3.0x) | 269 (3.5x) | | |

The standard model now runs at about 44 GMAC/s, half the core's peak. The prototype beats the fork's own
A2 fast path by 2.8x at full size and 3.5x at nano.

**Accuracy and determinism** (`invariance.exe`, `coverage.exe`, `sensitivity.exe`):

- **Against the NAM core:** 108-137 dB SNR at 64 samples, and 107-132 dB on the very first block after
  prewarm. Oversampling time scales of 2 and 4 come out at 113-126 dB.
- **Host block size:**
  - The fused output is bit-identical for blocks of 64, 47 and 1, for mixed sizes, and for blocks past
    the prepared size. Every frame takes the same arithmetic, and padded lanes are never output.
  - The NAM core is not invariant. Its renders differ by 106-146 dB between block sizes, because of
    Eigen's remainder columns and the scalar tail of its tanh loop.
- **Across the whole library:** 2,454 of 2,460 models are at least 90 dB from the NAM core. The other
  six are three models, each stored twice: a JCM800 D.I. capture at 71 dB and two EQ pedals at 81 dB.
  - On the guitar-like input they score 118-126 dB. The low figures appear only on the low swept tone
    the coverage run uses.
  - `sensitivity.exe` shows the cause. Nudging their input by one part in 10⁷ moves the NAM core's own
    output to 77 and 85 dB. Those models amplify float rounding, and the fused path is no further off
    than that nudge.

**Tuning that was measured, and what it showed:**

- **Tile shape** (outputs × 8-frame vectors).
  - 4 × 3 beats the 4 × 2 default by 1-9% on every model. 2 × 4 is best on A2 nano: 68 ns against 81.
  - 3 × 4 and 8 × 1 are 15-57% slower. The first spills registers, and the second is load-bound.
  - The choice is worth a few percent, not more. Every tile gives the same bits, because each output
    sums its inputs in the same order.
- **Activation.** It is 6-10% of the fused time. Swapping its divide for a reciprocal estimate plus one
  Newton step gains nothing.
- **Large host blocks.** At 512 and 1024 the working rows leave the 32 KB L1. The standard model slows
  to 327-359 ns at 512, and to 465 at 1024.
  - An internal 128-frame chunk holds it at 290-296 ns.
  - It takes the 32/8 model from 3654 to 1762 ns at 1024.
  - The chunking is exact, so it is the prototype's default.
- **128-bit vectors** (two SSE halves, the shape a NEON build takes):
  - MSVC: 1.8-2.7x over the NAM core's SSE2 build.
  - clang: 1.2-2.3x. Sixteen accumulators overflow SSE's 16 registers. NEON has 32, so ARM should land
    nearer the AVX2 ratios, but that needs measuring on ARM.
  - The plain C++ fallback, with no intrinsics, is 0.3-0.4x under MSVC. A NEON version has to be written,
    not left to the auto-vectoriser.

### 2. Model builds and prewarms dominate a preset switch (P1, measured)

Per model, at 48 kHz with 64-sample blocks (`loadtime.exe`):

| Model | Parse and build from file | Cache hit: `get_dsp` from parsed data | Our `Prepare`: `Reset(48000, 65)` | Fused build from parsed data | Fused `Reset` |
|---|---|---|---|---|---|
| Standard | 13 ms | 8.9 ms | 5.1 ms | 0.04 ms | 0.0 ms |
| A2 | 22 ms | 16.6 ms | 4.7 ms | 0.05 ms | 0.0 ms |
| Lite | 9 ms | 7.1 ms | 4.5 ms | 0.03 ms | 0.0 ms |
| xSTD | 26 ms | 23 ms | 17.4 ms | 0.06 ms | 0.1 ms |
| 32/8 | 72 ms | 46 ms | 36.7 ms | 0.15 ms | 0.4 ms |

**What a cache hit still pays for:**

- `NamModelCache` saves the file read, but `get_dsp` re-walks the JSON config. For an A2 file, that is
  both submodels' weights, held as JSON numbers.
- It builds **both** A2 submodels, and each one's construction runs a prewarm.
- Every construction prewarms at the core's default 4096-frame buffer (`create_dsp`), so an A2 file
  runs three prewarms: one per submodel, then the active one again. Our `Prepare` then resets the model
  and prewarms it once more at the real block size.
- A stereo connection pays all of this twice, and a right model built after a mono-to-stereo change
  pays it again.

**One-pass prewarm.** A prewarm feeds silence for longer than the receptive field. A WaveNet has no
feedback, so afterwards each layer's history holds one constant column. The prototype writes that
column into every history slot, layer by layer, in a single 8-frame pass. The output hash after the
one-pass fill equals the hash after a real prewarm, for all seven test models.

### 3. The NAM core overruns past its prepared size (P3, confirmed, guarded)

- **Generic WaveNet:** handed more frames than `Reset` was given, it writes past its buffers, because
  the guard is an `assert`, which Release compiles out. The invariance harness crashed this way.
- **`A2FastModel`:** reallocates instead, which is an allocation on the audio thread.
- **Our wrappers:** `OptimizedNAMAmpEffect` and `MultiModelNAMAmpEffect` clamp to `mMaxBlockSize`.
  `NamOversamplingProcessor` prepares the model for the resampler's largest rendering block. So neither
  can happen today.
- **Fused path:** it chunks internally, so it has neither failure.

### 4. Smaller items (measured or read)

- **Wrapper.** `OptimizedNAMAmpEffect` adds about 5 ns per sample: float↔double, the 5 Hz DC blocker
  and output gain. That is under 2% of a fused standard model. Nothing to do.
- **Stereo helper threshold.** `ShouldParallelizeStereoWork` hands the right model to the dual-lane
  helper from 96 samples (`RealtimeParallel.h:313`). A helper wake costs 5-50 µs. A fused standard model
  takes about 28 µs at 96 samples, and 40 µs at 128. Re-measure the threshold once fused models ship;
  it probably wants to rise.
- **Benchmark hygiene.** `NAMPerformanceBenchmark` still calls `nam::get_dsp` without fast tanh, as the
  14 October review found. Its standard-model figure overstates the cost by about 20%.
- **Docs.** `docs/neural-amp-modeler-core-integration.md` still names `sdatkinson@v0.4.0.rc3` as the
  fetch source. The core fetches the DLC86 fork at `bea723b`.
- **Rejected:**
  - Reduced precision and low-rank weights, from the 18 September compression experiment: only fewer
    MACs buys speed.
  - Assembly, from 6 September: the ceiling over intrinsics is about 10%.
  - A reciprocal divide, above.

## Recommendations, in order

| # | Change | Expected gain | Effort |
|---|---|---|---|
| 1 | Move `FusedWaveNet.h` into `core/src/dsp`, behind the model cache (details below) | 2.0-4.8x on every NAM model on the test machine. A mono standard chain at 48 kHz/64 goes from about 58 to about 25 µs per block, and a stereo one from 117 to about 45 | Medium: 1-2 days with tests |
| 2 | Cache the packed fused weights in `NamModelCache`, shared by every instance and channel, and use the one-pass prewarm | Model swap from 12-82 ms (cache hit plus `Prepare`) to under 0.6 ms per model and channel | Small, with 1 |
| 3 | A NEON vector (a pair of `float32x4_t` with `vfmaq_f32`), measured on Apple silicon and Android | Brings the gain to macOS arm64 and Android. Without it the fused path must stay off there, because the plain C++ fallback is slower than the NAM core | Small to write, needs ARM hardware to measure |
| 4 | Re-measure the dual-lane threshold (96 samples) with fused models | Avoids paying a helper wake for a model that now takes less time than the wake | Small |
| 5 | Fix `NAMPerformanceBenchmark` (fast tanh, and a fused row) and the stale integration doc | Keeps the published numbers true | Trivial |
| 6 | Downgrade the fork rebase (14 October, #4) and the clang-cl experiment (#10) | Both promised 5-11% on code that recommendation 1 takes off the audio path for every model measured here | None |

**Recommendation 1 in detail:**

- **Where it plugs in.** `NamModelCache::GetModel` already holds the parsed `dspData`.
  - For a WaveNet, try the fused build. On a nullptr, fall back to `nam::get_dsp`.
  - Copy the metadata `create_dsp` applies: loudness, and input and output level.
  - For a `SlimmableContainer`, build a small container of fused submodels that implements
    `nam::SlimmableModel`. Use it only if every submodel fuses.
  - The NAM fork stays untouched. Its registry throws on a second `WaveNet` parser, so the hook belongs
    in our cache, not in a parser registration.
- **What the model must support.** Our code calls `SetTimeScale`, `Reset` and `process` on a model,
  plus `SetSlimmableSize`. The prototype has the first three; the container adds the fourth.
- **A switch** to force the NAM core, for A/B listening and support.
- **Tests** (`FusedWaveNetTests`):
  - every shape in `core/tests/testdata` at least 100 dB from the NAM core;
  - bit-identical across host block patterns;
  - one-pass prewarm equal to a real prewarm;
  - time scales 2 and 4;
  - the detector rejecting gated, FiLM, head1x1, grouped and condition-DSP configs, and wrong weight
    counts.

  Run them in Release and in the clang-cl build, since fast-math is where these break. Then run
  `tools/audio-ab` against HEAD on the NAM chains; expect "rounding".

## Not Verified

- Only Windows x64 was measured. The macOS, Linux and Android figures are inferred from clang-cl and SSE
  stand-ins; recommendation 3 needs real ARM numbers.
- The chain figures in recommendation 1 are estimates: the 14 October chain overhead (about 5 µs a
  block) plus the fused model. The prototype was not run inside the app or through the executor.
- No listening test. The differences are at float rounding: 108 dB or more on all but three models, and
  those three sit within their own rounding sensitivity.
- p99 and worst-block times were not interpreted, because the machine was loaded.
- LSTM and ConvNet models were not reviewed. None of the 1,672 files uses them.

## Outside this review

Two model files in the local Preset Pack Vol. 1 install have paths of 282 and 292 characters. Their file
names carry the content hash twice (`<hash>-<hash>-name.nam`). The benchmark tools could not open them,
because they have no long-path manifest. No `longPathAware` setting turned up under `juce/`, so the app
may fail on them too. That was not checked.
