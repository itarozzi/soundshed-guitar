# NAM Stereo Dual-Lane Threshold (2026-10-16)

Recommendation 4 of the [NAM DSP performance review](nam-dsp-performance-review-2026-10-16.md):
re-measure when a stereo NAM node hands its right channel's model to the dual-lane helper, now
that the fused WaveNet pass makes a model 2-4.8x cheaper. The review expected the 96-sample
threshold to rise. The data says the opposite: the hand-off paid off well below 96 samples. The
line it should follow is a lane's cost in microseconds, not a block size.

**Change.** The NAM amp and NAM Blend time their left lane and hand the right lane to the helper
once it takes 15 µs or more (`rtparallel::StereoLaneGate`, `core/src/dsp/RealtimeParallel.h`).
The IR cab and IR reverb keep the 96-sample rule; they were not measured.

## Method

`NAMDualLaneBenchmark` (core/tests, opt-in under ctest), Release, MSVC, AVX2, Ryzen 7 8845HS
(8 cores, 16 threads), 48 kHz. Models: `JCM800 Hi P6 B8 M4 T7 G6.nam` (standard WaveNet 16/8) and
`Boosted-6505+-A2-Chugs-FullRig.nam` (A2), both from core/tests/testdata, each on the fused
pass and on the NAM core. Two passes:

- **Direct:** two model instances called as `OptimizedNAMAmpEffect::ProcessModels` calls them:
  `DualLaneExecutor::Run` against both lanes on the calling thread, at every block size. It
  records whether the helper took the right lane and how long after dispatch it started.
- **Effect:** the NAM amp in a stereo graph, its own rule against `ScopedSharedHelpersRefused`.

Both run serial and dual in four alternating rounds, about 3 s of audio each. The calling thread
is raised to the audio class, as the helper is. **Paced** runs sleep out each block's period, as
a host's audio thread does between callbacks, so every block wakes a sleeping helper. **Back to
back** runs keep it warm. CPU is process CPU per block, both threads (`QueryProcessCycleTime`).

SteadyStateProfiler can't measure this. It turns multi-core processing off, so every helper is
refused.

## What a hand-off is worth, by lane cost

Direct pass, the lanes under 25 µs (lane = the serial block / 2):

| Lane (µs) | Wall | Case |
|---:|---:|---|
| 6.4 | -18% | A2, fused, back to back, 16 samples |
| 7.7 | -11% | A2, fused, back to back, 24 samples |
| 8.0 | +3% | Standard, fused, back to back, 16 samples |
| 9.1 | -1% | Standard, fused, back to back, 24 samples |
| 10.8 | -1% | A2, fused, back to back, 32 samples |
| 12.3 | +3% | Standard, fused, paced, 16 samples |
| 13.0 | +8% | A2, fused, paced, 16 samples |
| 13.6 | +3% | A2, fused, back to back, 48 samples |
| 13.9 | +12% | Standard, fused, back to back, 32 samples |
| 17.0 | +16% | Standard, fused, back to back, 48 samples |
| 17.1 | +14% | A2, fused, paced, 24 samples |
| 17.4 | +17% | A2, NAM core, back to back, 16 samples |
| 18.5 | +18% | Standard, fused, paced, 24 samples |
| 18.8 | +20% | A2, fused, back to back, 64 samples |
| 20.9 | +11% | Standard, fused, back to back, 64 samples |
| 21.9 | +17% | Standard, fused, paced, 32 samples |
| 22.5 | +23% | Standard, NAM core, back to back, 16 samples |
| 24.5 | +29% | A2, fused, paced, 32 samples |

A lane of about 11 µs broke even; from about 15 µs the hand-off saved 10-30%, and up to 46% on
larger lanes. The helper took the right lane in 96-100% of blocks at every size. It started 8-19 µs
after dispatch when paced, and 1-10 µs when warm (medians per block size). The same 15 µs falls at 16 samples on the NAM core
and at 32-64 on the fused pass, so no block size can stand in for it. A second sweep of a heavier
8/8 model (7 dilations of kernel 6) won by 17-43% at every size.

## Before and after: the NAM amp, paced

Wall time saved by the dual lane against helpers refused. Before is the 96-sample rule, after is
the 15 µs gate; brackets are serial → dual µs per stereo block, after.

| Block | Standard, fused | A2, fused | Standard, NAM core | A2, NAM core |
|---:|---|---|---|---|
| 16 | -3% → **+8%** (29 → 27 µs) | -2% → **+9%** (28 → 25 µs) | +4% → **+25%** (64 → 48 µs) | -2% → **+27%** (65 → 48 µs) |
| 24 | -5% → **+13%** (37 → 32 µs) | +3% → **+12%** (36 → 31 µs) | +3% → **+23%** (87 → 67 µs) | -1% → **+30%** (84 → 59 µs) |
| 32 | 0% → **+22%** (47 → 37 µs) | -2% → **+19%** (43 → 35 µs) | +4% → **+34%** (111 → 73 µs) | +1% → **+32%** (97 → 66 µs) |
| 48 | -5% → **+24%** (61 → 46 µs) | +1% → **+25%** (62 → 46 µs) | +2% → **+31%** (142 → 99 µs) | -3% → **+29%** (128 → 90 µs) |
| 64 | +2% → **+20%** (76 → 61 µs) | +1% → **+23%** (66 → 51 µs) | +3% → **+29%** (171 → 121 µs) | -1% → **+32%** (157 → 106 µs) |
| 96 | +29% → **+28%** (105 → 75 µs) | +20% → **+22%** (87 → 67 µs) | +32% → **+33%** (248 → 166 µs) | +32% → **+31%** (209 → 143 µs) |
| 128 | +25% → **+22%** (128 → 100 µs) | +26% → **+28%** (115 → 83 µs) | +40% → **+33%** (318 → 212 µs) | +34% → **+38%** (298 → 186 µs) |
| 256 | +27% → **+21%** (241 → 190 µs) | +30% → **+29%** (204 → 145 µs) | +35% → **+36%** (625 → 403 µs) | +35% → **+33%** (505 → 336 µs) |
| 512 | +29% → **+26%** (451 → 333 µs) | +27% → **+27%** (384 → 279 µs) | +33% → **+32%** (1227 → 833 µs) | +34% → **+33%** (984 → 660 µs) |

Below 96 samples "before" is two serial runs, so ±5% is the noise. At 96 and up both rules hand
over every block, and the two columns agree within run-to-run noise. The dual lane costs about
30% more process CPU (10-50%) wherever it runs, before and after alike.

## Back to back, and where the helper lands

Back-to-back effect rows were noisy between runs. At 128 samples, where both rules hand over every
block, the standard fused model showed +24% in one run and +2% in the next, with the dual lane's CPU up to 1.8x the serial. Limiting the process to
one logical CPU per physical core (affinity 0x5555) made them consistent. The standard fused model
then gained 27-43% from 48 samples and the NAM core 21-49% from 16, and the CPU overhead at 512
samples fell from +75% to +8%. So the noise is the helper sharing a physical core with the audio
thread (its SMT sibling), with the caller spinning while both run at half speed. Paced runs did
not show it. Nothing here addresses that; it is a separate change (the helper's CPU placement).

## Not measured

- The IR cab and IR reverb, still on 96 samples.
- Other CPUs and operating systems. On a faster core a lane takes less time but the wake does not
  shrink as much, so the gate brings the hand-off in at larger blocks there, as it should. arm64
  still runs the NAM core (no NEON fused kernel yet), where lanes are dear and the gate hands
  over from the smallest blocks.
- Oversampled NAM and the NAM Blend with several models. Both make a lane cost more, which the gate
  reads directly.

## Reproduce

```
cmake --build core/build --config Release --target NAMDualLaneBenchmark
core/build/tests/Release/NAMDualLaneBenchmark.exe            # direct and effect, both models
core/build/tests/Release/NAMDualLaneBenchmark.exe --effect   # the effect rows only
core/build/tests/Release/NAMDualLaneBenchmark.exe --direct path/to/model.nam
```

About 15 minutes for the full run; the paced rows run in real time.
