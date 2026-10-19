# Zero-latency IR Cabinet

Investigation, 2026-10-16. The numbers come from a scratch prototype built against the
shipping `RealtimeConvolver`. Steps 1-4 of *Proposed change* are implemented in
`RealtimeConvolver` (Low Latency is now zero latency); 5 and 6 are not.

## Where the IR Cabinet's latency comes from today

All of it is the convolver's block. Speaker Drive, mic emulation, the cut filters and Air are
IIR or memoryless and add none.

| Setting | Reported latency | At 48 kHz |
| ------- | ---------------- | --------- |
| Low Latency on (default), host max block 64 | 64 samples | 1.3 ms |
| Low Latency on, host max block 128 | 128 | 2.7 ms |
| Low Latency on, host max block above 128 (most DAW setups) | 256 | 5.3 ms |
| Low Latency off | 256 to 2048, from the host max block | 5.3 to 43 ms |
| IR of 64 samples or fewer | 0 (direct FIR) | 0 |

That is HEAD. The shipped 1.6.0 is worse: before 35f67844 (2026-10-14, in no release tag yet) a
short IR with Low Latency on took the uniform partition, 512-2048 samples (10.7-43 ms) whenever
the host promised blocks over 256.

`RealtimeConvolver::BuildNonUniform` already splits the IR Gardner-style: a head partition at the
base block, then stages whose partition doubles, then a uniform tail. Every stage after the head has
partition P equal to its offset in the IR, and one shared delay line of `base` samples lines all of
them up behind the head. The whole engine therefore runs `base` samples late.

Two things the latency also breaks today:

- **Cab Mix below 100% comb-filters.** `IRCabEffect::Process` adds the dry input undelayed to a wet
  signal that is 64-256 samples late: notches every 750 Hz at 64 samples, every 187 Hz at 256.
- **The Convolution Reverb misreports its latency.** It reports the convolver's block, but its dry
  path is not delayed. A DAW compensating for that pulls the dry guitar 1.3-5.3 ms early. (The wet
  side being that late is harmless, it is predelay.)

## The zero-latency layout

A uniform stage with partition P emits its segment's convolution P samples late. A segment that
starts at IR offset `off` has to reach the output exactly `off` samples late. So a stage with
**P == off needs no extra delay at all**, and the existing layout already has that for every stage
but the head. Only h[0, head) cannot be done this way (it would need P = 0), so it becomes a
direct-form FIR:

```
h[0, H)        direct FIR, H taps            zero delay
h[H, 2H)       FFT stage, P = H               its own latency is the offset
h[2H, 4H)      FFT stage, P = 2H              "
...            doubling up to the tail partition
h[T, n)        uniform tail, P = T            "
```

Compared with today's non-uniform engine: the head FFT stage becomes the FIR, and the shared delay
line goes. Nothing else moves. The head length H is free: it no longer follows the host block, only
the CPU trade.

## Measured

Prototype: a copy of `RealtimeConvolver` with that layout (`BuildZeroLatency` /
`ProcessZeroLatency`, about 130 lines), a 4-output FIR kernel in plain C++ that the compiler
vectorises. Synthetic decaying-noise IRs, 48 kHz, one channel.

**Correctness.** Against an exact double-precision convolution, the prototype's worst sample error
is -114 to -127 dB below the output RMS for IRs of 480 to 40,000 samples and blocks of 37, 64 and
512: float rounding, the same as today's engine (-114 to -119 dB), and at latency 0 throughout.

**CPU**, ns per sample per channel, Release flags (`/Ox /fp:fast /arch:AVX2`), best of 3 runs.
This machine is noisy; read the ratios within a row, not single digits.

| IR (samples) | Host block | Low Latency today (latency) | Zero latency, H = 256 | H = 512 | Low Latency off (latency) |
| ------------ | ---------- | --------------------------- | --------------------- | ------- | ------------------------- |
| 480 (Economy) | 64 | 88 (64) | 30 | | 22 (256) |
| 2,064 (Standard cap) | 64 | 93 (64) | **33** | 39 | 26 (256) |
| 2,064 | 512 | 24 (256) | 32 | 36 | 22 (512) |
| 8,160 (High cap) | 64 | 111 (64) | **47** | 48 | 38 (256) |
| 8,160 | 512 | 51 (256) | 58 | 51 | 31 (512) |
| 24,000 (500 ms, Full) | 64 | 240 (64) | 116 | **102** | 136 (256) |
| 24,000 | 512 | 139 (256) | 113 | **90** | 67 (512) |
| 40,000 | 64 | 172 (64) | 125 | **87** | 149 (256) |
| 40,000 | 512 | 169 (256) | 127 | **112** | 95 (512) |

- At small host blocks (32-128), where latency matters most, zero latency is **1.5-3x cheaper**
  than today's Low Latency. Today's engine runs 64-sample partitions there: 128-point complex FFTs,
  with the head and the first stage each transforming the same input. A 256-tap FIR is cheaper.
- At large blocks it costs about the same for long IRs and up to ~35% more for short ones. In
  absolute terms that is nothing: 33 ns/sample is 1.6 ms of CPU per second, 0.16% of one core; a
  stereo two-IR cab (four convolvers) about 0.6%.
- Worst single callback: under 75 us at a 32-sample block (deadline 667 us), under 200 us at 1024
  (deadline 21 ms). The largest FFT still lands in one callback, as it does today.
- The SSE2-only build (no `/arch`) and clang-cl `-ffast-math` (Android's FP semantics, x86) keep the
  same order: Standard at block 64, 58 vs 144 ns (SSE2) and 43 vs 99 ns (clang).
- H = 256 is best up to ~8k-sample IRs, H = 512 above. A rule such as
  `H = min(TailPartitionForIR(n), 512)` matches every measured row. An IR no longer than H is a
  pure FIR.
- Low Latency off stays the cheapest at large blocks on long IRs, so it remains worth keeping.

Not measured: ARM (Android, Apple Silicon). The kernel is plain C++ and relies on the fast-math
vectorisation those builds already have, but check it on a low-end Android device.

## Leading silence in the IR files

The other latency a player hears is the IR's own pre-delay (mic distance). Of the 63 distinct
cab-length IRs (under 1 s) in `resources/` and the dev profile, 51 reach -40 dB of their peak
within 0.1 ms, 4 within 0.5 ms, 7 take 0.5-1.6 ms and one 3.5 ms. A load-time trim would remove that. With two IRs loaded it has to
trim both slots by the earlier onset, so their alignment (`slotBOffset`) is untouched. It changes
when a cab sounds relative to a parallel dry branch, so it wants its own toggle and is a separate
decision from the convolver change.

## Proposed change

1. In `RealtimeConvolver`, make Low Latency mean zero latency: build the head as a direct FIR of H
   taps and drop `mNuDelayLine`. `GetLatency()` returns 0 for it. The head no longer depends on
   `blockSize`.
2. Replace `ProcessDirect`'s modulo-per-tap loop with the same FIR kernel, and raise the
   pure-FIR threshold from 64 taps to H.
3. Nothing to change in `IRCabEffect` or `IRReverbEffect`: both already ask for Low Latency by
   default and report `GetLatency()`. That also fixes the cab Mix comb filtering and the reverb's
   misreported latency. A cab keeps its sound and only moves in time; a convolution reverb
   below 100% Mix changes slightly, as its dry and wet line up (see *As implemented*).
4. Update `RealtimeConvolverTests` (it pins the 64-256 latencies) and add a zero-latency
   impulse/noise test against direct convolution, at odd block sizes. `audio-ab` will note a latency
   change on chains with a cab or convolution reverb; that is the intended difference.
5. Optional CPU follow-up: the FFT stages use a complex FFT on real input and multiply full-length
   spectra. A real FFT on half spectra would roughly halve their cost.
6. Separate, opt-in: leading-silence trim (above).

Host side: the plugin reports 64-256 samples less latency once a preset with a cab loads, and the
host renegotiates delay compensation as it already does on any latency change.

## As implemented

`RealtimeConvolver` with Low Latency on: a FIR head of `min(TailPartitionForIR(n), 512)` taps
(256, or 512 from 16,448 samples), stages at P == offset, no delay line. An IR no longer than the
head runs on the FIR alone. The FIR takes a non-finite input sample as silence, and the engine's
output gets the same finite check and ±100 clamp as the FFT path.

Against HEAD's convolver (4700f92a), both Low Latency, AVX2 Release flags, one channel, ns per
sample, best of 4 interleaved runs. The new output equals HEAD's shifted by HEAD's latency to
within 1e-6 (unit-energy IR, ±1 noise) in every row.

| IR (samples) | Block 32 | 64 | 128 | 256 | 512 | 1024 |
| ------------ | -------- | -- | --- | --- | --- | ---- |
| 480 | 81 → 28 | 81 → 28 | 63 → 28 | 21 → 28 | 21 → 28 | 21 → 28 |
| 2,064 | 86 → 32 | 85 → 32 | 66 → 32 | 24 → 32 | 24 → 31 | 24 → 32 |
| 8,160 | 100 → 46 | 99 → 46 | 80 → 45 | 37 → 45 | 37 → 45 | 36 → 44 |
| 24,000 | 130 → 62 | 129 → 61 | 110 → 60 | 93 → 60 | 89 → 61 | 90 → 60 |
| 40,000 | 151 → 85 | 148 → 81 | 128 → 81 | 110 → 81 | 107 → 78 | 108 → 79 |
| 144,000 (3 s reverb) | 204 → 132 | 194 → 131 | 178 → 127 | 154 → 130 | 154 → 126 | 151 → 123 |

Worst single callback was about the same as HEAD's throughout, and under 240 us for the 3 s IR
at a 32-sample block (deadline 667 us).

`tools/audio-ab` HEAD against HEAD plus this change (48 kHz, 64-sample blocks): every effect and
chain with an IR Cabinet nulls below -127 dB once shifted 64 samples, except where a free-running
LFO downstream (the pedalboard chains' chorus) now meets the signal at a different phase: level and
every band within 0.03 dB, null -32 dB. The Convolution Reverb is the one real change, and an
intended one: below 100% Mix its dry and wet no longer sit 64-256 samples apart. The tool's
reverb case uses the cab IR at Mix 30%, the worst case (+0.7 dB, -8 dB at 250 Hz). On the factory
pack's two bundled reverb IRs it is up to 1.25 dB per third-octave band against the old 64-sample
offset, and 2.3-3.4 dB at 63 Hz (0.6 dB above) against the old 256.

Also checked: the fast-math Release semantics. A probe built with MSVC `/fp:fast` and clang-cl
`-ffast-math` fed NaN and infinity (parsed at run time, checked with a volatile bit test) into IRs
of 200 to 40,000 samples: nothing non-finite came out, and the signal returned.

## Is Low Latency off still worth having?

Measured 2026-10-17 at 22f67212: the current `RealtimeConvolver`, on against off, one channel,
48 kHz, `/Ox /fp:fast /arch:AVX2`, ns per sample, best of 5 interleaved runs. "Block" is both the
host's promised maximum and the block processed, which is off's best case: a host that promises
more than it sends gives off a larger partition, so more latency, for the same CPU.

| IR (samples) | Block 64: on / off | 256 | 512 | 1024 | Off's latency |
| ------------ | ------------------ | --- | --- | ---- | ------------- |
| 480 (Economy) | 29 / 20 | 28 / 20 | 28 / 20 | 28 / 21 | 256, 256, 512, 1024 |
| 2,064 (Standard, default) | 32 / 25 | 32 / 24 | 31 / 22 | 31 / 22 | " |
| 8,160 (High) | 46 / 39 | 45 / 39 | 44 / 32 | 45 / 24 | " |
| 24,000 (500 ms, Full) | 65 / 77 | 66 / 76 | 64 / 48 | 63 / 34 | " |
| 48,000 (1 s) | 102 / 193 | 97 / 190 | 102 / 102 | 93 / 51 | " |
| 144,000 (3 s reverb) | 161 / 505 | 151 / 535 | 143 / 251 | 141 / 119 | " |

On's latency is 0 in every row. Worst single callback was within about 2x either way, and under
450 us everywhere (3 s IR, block 256, off).

- On cab-length IRs (Economy to High) off saves 7-9 ns per sample per convolver below a 512 block:
  about 0.04% of one core each, 0.15% for a stereo two-IR cab. It pays for that with 5.3 ms of
  latency or more. Only at 1024-sample blocks does it near half the cost.
- On a Full-length cab IR or a convolution reverb, off costs more than on at every block up to
  256, and on a 3 s reverb up to 512 (1.2-3.7x). The tail's large partitions are what make on
  cheap there; off runs the whole IR at the small uniform partition.
- The output is the same either way apart from the shift, since off delays the dry to match
  (`DryPathDelay.h`, e44c16d5), so off has no sound advantage.

So off no longer earns a user-facing switch. What stops it being dropped today: ARM (Android, Apple
Silicon) is still unmeasured, and off is the only fallback if the FIR head turns out slow there.

**Retired 2026-10-17** from both IR effects, ARM still unmeasured. `RealtimeConvolver` is now
zero-latency only: `SetImpulse()` takes no block size, and `SetLowLatencyMode`, `GetLatency` and
`GetMaxLatency` are gone. The uniform engine stays as the FFT stages' builder, so a uniform mode for
ARM would be a few lines in `SetImpulse` if one is ever needed. `DryPathDelay.h` and the controller's
re-report of latency on a `lowLatency` change went with it. A saved `lowLatency` is ignored
(`IrZeroLatencyTests`).

## The Convolution Reverb

It needs no change to get any of this. It builds its convolvers with Low Latency on by default (no
factory preset turns it off), and the zero-latency engine lives in `RealtimeConvolver`. The reverb
gains the most from it: the dry and wet line up (it used to report latency while its dry ran
undelayed), and a 3 s IR costs 1.2-1.5x less at every block from 32 to 1024 (*As implemented*). Its Low
Latency switch was the one to retire first, since off cost 1.8-3.7x more on a 3 s IR at every
block up to 512 (table above); it went on 2026-10-17 with the cab's.

What is still open for it, checked 2026-10-17 on the 925 WAVs under `D:/NAM/Reverbs` (Bricasti,
adventurekid, Voxengo, EMT-140 and a few others):

- **Skipping leading silence would save nothing.** The median onset is 0 ms at -80 dB in every
  collection, and 5 files of 925 start 10.7 ms or later (the 512-tap FIR head at 48 kHz).
- **Full quality keeps the dead tail.** Where the energy still to come falls below -70 dB of the
  total, the median file has 25% left (Bricasti), 27% (adventurekid) and 19% (Voxengo), but 1-6%
  elsewhere. Full skips the energy truncation the other qualities do. Trimming that would buy
  less than its share of length, because the engine's cost grows more slowly than the IR (1 s
  97 ns, 3 s about 150 ns per sample).
- **True stereo does the FFTs twice.** 268 of the 804 Bricasti files have four channels, so they
  run four convolvers. LL and RL transform the same left input and LR and RR the same right,
  and LL's and LR's outputs are summed after two separate inverse FFTs. A convolver taking two
  IRs on one input, and summing in the frequency domain, would halve the true-stereo FFT work;
  the multiplies stay.
- **The real-FFT follow-up (step 5) pays most here.** A long reverb IR's cost is mostly FFT
  stages; a cab IR's is mostly the FIR head.

## Per-callback cost of a long IR (2026-10-19)

The layout above makes every stage's partition equal its offset (P == off), so a stage has no
slack: the block that closes at a boundary is wanted from that sample on, and its forward and
inverse FFTs run in that callback. The multiplies were already spread over the block; the FFTs
were not. With a 4096 tail every stage closes together every 4096 samples. On a Galaxy S21's A78
cores (release APK, 96-frame blocks, "Convoluted Acoustic Sim") the IR Reverb ran a median of
116-129 µs and a p90 of 325-500 µs, max 750-870, and was about 60% of the chain's load peaks.

Once the tail partition reaches 1024 (an IR over 32,768 samples), every stage after the FIR is now
a *spread* engine (`RealtimeConvolverSpread.cpp`): two partitions late, so it starts at off == 2P,
and it computes each block over the samples of the next one, a slice per callback in proportion to
how far that block has filled. The slices are weighted by measured per-item costs (a bit-reversed
load is about 2x a butterfly, a butterfly about 2x a multiply-accumulate on x64); with flat weights
the callback after a 4096 boundary did 4x its share. `SimdFFT` gained a stepped transform
(`LoadBitReversed`, `RunButterflies`, `GetResult`) with exactly the arithmetic of `Forward` and
`Inverse`, which now run through it.

Doubling the partition from stage to stage, as the boundary layout does, cost 10-25% more on
average than that layout. Growing 16x per stage, which for every tail up to 4096 means a single
256-sample stage of 2-30 partitions from 512 to twice the tail partition, then the tail, trades FFTs
for multiply-accumulates and costs less than the boundary layout did. A 3 s IR is the FIR to 512,
a 256 stage of 30 partitions to 8192, and the 4096 tail from there.

One convolver on a synthetic IR, 96-sample blocks, µs per block:

| | IR | before p50 / p90 / p99 / max | after p50 / p90 / p99 / max | mean before → after |
| --- | --- | --- | --- | --- |
| S21 A78 | 0.8 s | 13 / 52 / 136 / 290 | 27 / 30 / 36 / 315 | 27.2 → 27.7 |
| S21 A78 | 1.5 s | 14 / 53 / 310 / 356 | 29 / 32 / 35 / 159 | 34.5 → 29.1 |
| S21 A78 | 3 s | 15 / 54 / 659 / 924 | 34 / 37 / 41 / 93 | 44.2 → 34.3 |
| S21 A78 | 6 s | 24 / 63 / 674 / 773 | 42 / 45 / 60 / 103 | 51.8 → 41.9 |
| S21 A78 | 10 s | 33 / 73 / 679 / 934 | 52 / 56 / 73 / 281 | 61.9 → 52.1 |
| x64 AVX2 | 0.8 s | 4.0 / 17.6 / 38 / 140 | 7.5 / 8.3 / 15 / 77 | 8.0 → 7.7 |
| x64 AVX2 | 3 s | 4.9 / 22.5 / 179 / 271 | 9.4 / 13.8 / 24 / 101 | 13.1 → 10.3 |
| x64 AVX2 | 10 s | 16.4 / 40.5 / 197 / 418 | 21.2 / 28.9 / 54 / 169 | 25.5 → 21.9 |

The whole effect with the preset's Parking Garage IR (stereo, Standard quality, the dual lane on),
96-sample blocks: on the A78 p50 / p90 / p99 went from 35 / 111 / 285 to 64 / 70 / 83 µs at the same
mean (63 → 65), and on x64 from 11 / 32 / 54 to 14 / 20 / 33. The occasional max of 300 µs or so on
the phone is in both builds; the convolver alone does not show it. Against the boundary layout the
output nulls at -130 dB on synthetic IRs at 37-, 64-, 96- and 512-sample blocks and -144 dB on the
reverb, and IRs that keep the boundary layout are bit-identical. At 1024-sample blocks the mean is
also equal or lower.
