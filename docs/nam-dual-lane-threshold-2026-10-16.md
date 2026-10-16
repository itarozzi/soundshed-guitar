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
not show it here, but later runs did: see the next section, which confirms the cause and changes
where the helper runs.

## Where the helper runs (follow-up)

`NAMDualLaneBenchmark --placement` records, for each dual block, the logical processor the calling
thread dispatched from and the one the helper started the right lane on
(`GetCurrentProcessorNumberEx`; in the effect pass through the pool's claim hook). It sorts the
blocks into four cases: the helper on another physical core, on the caller's SMT sibling, on the
caller's own CPU, or not at all (the caller ran both lanes).

**The cause, confirmed.** Left to Windows, the helper woke on the caller's SMT sibling in 70-98% of
the NAM amp's blocks on an otherwise idle machine (under 10% load), paced and back to back alike.
Within one row, a block with the helper on the sibling took 1.21x as long as one with the helper
on another core in the direct pass, and 1.13x in the effect pass, whose block holds more than the
two lanes. Across 248 rows, those with under 10% sibling blocks saved 37% on average, and those
with half or more saved 21% (correlation -0.52). The processor numbers show how Windows chooses.
Caller and helper sat mostly on CPUs 2, 3, 6 and 7 (cores 1 and 3), and the helper hardly ever
ran on the ideal processor Windows gave it (CPU 10). Windows seems to prefer the idle sibling
of a busy core to a sleeping core, perhaps because that core is already awake. With the fix below,
the helper's paced start (dispatch to its lane starting) rose from 11-13 µs to 18-29 µs at 512
samples, where cores sleep longest between blocks.

**What was tried.** Two variants were rejected:

- An ideal processor on another core, re-aimed when the caller changes core: the helper was still
  on the sibling in up to 54% of back-to-back blocks, and gains stayed noisy.
- CPU sets leaving out the caller's last two cores rather than one: the paced sibling share fell
  from 9-22% to 2-10%, but most rows at 16-128 samples lost ground, by up to 13 points. Waking a
  sleeping core costs more than a short lane on a shared one.

The CPU sets that leave out only the caller's latest core gave 0% sibling blocks back to back.
That is the change.

**The change.** `DualLaneExecutor` starts its helper with `avoidDispatcherCore`
(`RealtimeTaskPool::Start`). Each `Run()` stores the processor number it dispatches from, which
costs about 1 ns. After its lane, before it sleeps, the helper looks that processor's physical core
up. The SMT topology comes from `GetSystemCpuSetInformation`, read once when the pool starts. If
the core is not the one it already avoids, the helper narrows its own selected CPU sets to the
other cores (`SetThreadSelectedCpuSets`, about 0.5 µs). Back to back that was rare, since the caller
mostly stayed on one CPU; paced, it happened whenever the audio thread woke on another core. The
audio thread gains nothing beyond reading its processor number.

Where a thread's hard affinity leaves none of its CPU sets (a host limited to one core), Windows
follows the affinity, so the helper still runs. Both functions are looked up at run time, so
nothing changes before Windows 10. The mixer's and the graph executor's pools are left as they
were.

**Results.** Release build, the same machine, two to three runs of each placement, alternated,
each started at under 12% machine load. Each cell is the wall time the dual lane saved against
helpers refused, before → **after**, with dual:serial process CPU in brackets. These runs and the
earlier sections' were taken hours apart, and absolute gains differed between sessions (the
earlier paced table had the old placement at +22% where these runs had +1%). Compare within a
table only.

NAM amp effect, paced (2 runs each):

| Block | Standard fused | A2 fused | Standard core | A2 core |
|---:|---|---|---|---|
| 16 | 0% → **-2%** (1.02 → 1.03x) | -1% → **0%** (1.02 → 1.00x) | +7% → **+28%** (1.64 → 1.28x) | +9% → **+28%** (1.62 → 1.26x) |
| 32 | +1% → **+15%** (1.60 → 1.29x) | 0% → **+11%** (1.59 → 1.35x) | +13% → **+23%** (1.56 → 1.30x) | +15% → **+25%** (1.54 → 1.29x) |
| 48 | +6% → **+15%** (1.54 → 1.34x) | +2% → **+12%** (1.58 → 1.34x) | +14% → **+26%** (1.58 → 1.28x) | +18% → **+29%** (1.50 → 1.25x) |
| 64 | +8% → **+19%** (1.58 → 1.33x) | +4% → **+13%** (1.60 → 1.30x) | +18% → **+30%** (1.53 → 1.24x) | +18% → **+27%** (1.52 → 1.27x) |
| 128 | +13% → **+22%** (1.57 → 1.32x) | +16% → **+19%** (1.50 → 1.32x) | +21% → **+37%** (1.50 → 1.17x) | +20% → **+31%** (1.52 → 1.25x) |
| 256 | +15% → **+30%** (1.59 → 1.26x) | +15% → **+29%** (1.56 → 1.24x) | +21% → **+38%** (1.54 → 1.17x) | +22% → **+37%** (1.52 → 1.20x) |
| 512 | +16% → **+32%** (1.61 → 1.28x) | +15% → **+32%** (1.62 → 1.25x) | +21% → **+40%** (1.56 → 1.17x) | +25% → **+37%** (1.45 → 1.21x) |

NAM amp effect, back to back (3 runs each):

| Block | Standard fused | A2 fused | Standard core | A2 core |
|---:|---|---|---|---|
| 16 | 0% → **-1%** (1.00 → 1.01x) | +3% → **+1%** (0.97 → 0.99x) | -3% → **+29%** (1.87 → 1.30x) | -1% → **+33%** (1.85 → 1.22x) |
| 32 | 0% → **0%** (1.00 → 1.00x) | +2% → **0%** (1.01 → 1.00x) | 0% → **+38%** (1.90 → 1.16x) | +4% → **+38%** (1.82 → 1.16x) |
| 48 | -1% → **+1%** (1.08 → 1.04x) | +1% → **+1%** (1.30 → 1.05x) | +7% → **+40%** (1.78 → 1.14x) | +5% → **+42%** (1.81 → 1.10x) |
| 64 | -6% → **+33%** (1.92 → 1.20x) | -7% → **+32%** (1.95 → 1.20x) | +6% → **+43%** (1.82 → 1.09x) | +10% → **+42%** (1.73 → 1.10x) |
| 128 | +1% → **+33%** (1.88 → 1.22x) | +5% → **+29%** (1.77 → 1.29x) | +7% → **+44%** (1.83 → 1.08x) | +13% → **+41%** (1.70 → 1.11x) |
| 256 | +3% → **+32%** (1.88 → 1.26x) | +14% → **+39%** (1.63 → 1.13x) | +14% → **+45%** (1.69 → 1.07x) | +10% → **+43%** (1.76 → 1.09x) |
| 512 | +5% → **+38%** (1.85 → 1.19x) | +3% → **+41%** (1.87 → 1.11x) | +14% → **+47%** (1.69 → 1.05x) | +15% → **+46%** (1.67 → 1.05x) |

The fused rows at 16-48 samples back to back, and at 16 paced, are the gate keeping the block on
one thread, before and after. Blocks with the helper on the caller's sibling fell from 70-84% to
12-20% paced, and from 78-98% to 0% back to back. What is left paced is the audio thread waking on
another core, since the helper's CPU sets follow it one block late.

The direct pass (two models, no gate) moved the same way. Paced, the gain went from -14% to +23%
before to 0% to +41% after. Back to back, the A2 fused model and the NAM core gained 16-46 points
on the old placement. The one case Windows had already placed well was the standard fused model
back to back (0% sibling blocks before). It came out 2-5 points lower at 16-64 samples, about 1-2 µs
a block, and level from 96; the cause was not found.

**macOS and Linux.** Nothing changed on either.

- macOS: Apple silicon has no SMT. Intel Macs do, but the one placement hint is the affinity tag
  (`THREAD_AFFINITY_POLICY`), which groups threads to share an L2 rather than keeping them apart,
  and Apple silicon ignores it.
- Linux: the fair scheduler's wake-up path looks for a wholly idle core first. But the helper runs
  `SCHED_FIFO` where rtprio allows, and real-time placement goes by the priority of what each CPU
  is running, not by SMT siblings (from the kernel source, not measured). The tool there would be
  a hard affinity mask: `sched_getcpu()` on the audio thread, the siblings from
  `/sys/devices/system/cpu/cpu*/topology/thread_siblings_list`, then `sched_setaffinity` on the
  helper.

Neither is trivial or measured, so both are left alone.

## Not measured

- The IR cab and IR reverb, still on 96 samples.
- Other CPUs and operating systems. On a faster core a lane takes less time but the wake does not
  shrink as much, so the gate brings the hand-off in at larger blocks there, as it should. arm64
  still runs the NAM core (no NEON fused kernel yet), where lanes are dear and the gate hands
  over from the smallest blocks.
- Oversampled NAM and the NAM Blend with several models. Both make a lane cost more, which the gate
  reads directly.
- The helper's placement anywhere but this Ryzen and Windows 11: Intel's hybrid parts (its CPU sets
  still include the efficiency cores, so Windows picks), Ryzens with two CCDs (the helper may land
  across the fabric from the audio thread), and a DAW whose own audio threads keep other cores busy.

## Reproduce

```
cmake --build core/build --config Release --target NAMDualLaneBenchmark
core/build/tests/Release/NAMDualLaneBenchmark.exe            # direct and effect, both models
core/build/tests/Release/NAMDualLaneBenchmark.exe --effect   # the effect rows only
core/build/tests/Release/NAMDualLaneBenchmark.exe --direct path/to/model.nam
core/build/tests/Release/NAMDualLaneBenchmark.exe --effect --placement --paced --blocks 32,64,128
```

About 15 minutes for the full run; the paced rows run in real time. `--fused`/`--core`,
`--paced`/`--back-to-back` and `--blocks` narrow it; `--placement` adds the per-row placement line.
The placement tables came from runs alternated with a build that started the helper without
`avoidDispatcherCore`, each started only once machine load was under 12%.
