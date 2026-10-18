# IR Cabinet review, 2026-10-18

Scope: `core/src/dsp/effects/IRCabEffect.h` and `core/src/dsp/effects/SpeakerDrive.h` at 4fd2cc5f, with
Speaker Drive as the focus. Nothing was changed. The numbers below come from standalone probes
(`cl /O2` against `core/src`, built the way the standalone effect harness is). The probe sources are
summarised at the end.

## Findings

### 1. Speaker Drive's power compression hits its 6 dB floor at normal levels (Drive ≥ ~0.5)

**Fixed the same day (findings 1 and 2):** the detector now reads the input without the pre-gain,
so the threshold sits at the nominal -18 dBFS whatever Drive is. Its state is clamped at the limit
power (-6 dBFS, 12 dB over the threshold). Attack, release, the 2:1 ratio and the 6 dB limit are
unchanged. After the fix, at Drive 1 on a 1 kHz tone: -18 dBFS → -1.0 dB, -12 → -4.0, -6 → -6.0.
A step to -6 dBFS is -0.2 dB after 5 ms and -6 dB after 300 ms. Recovery from the limit is complete
in about 1.1 s at any sample rate. `core/tests/SpeakerDriveTests.cpp` pins all of this, and 8 of its
checks fail against the old header. Simple Cabinet presets and IR Cab presets that use Speaker
Drive get louder at normal levels, by up to about 5 dB at full Drive (changelog). No makeup gain was
added.

The detector works on `input² · preGain²`, and `preGain` goes up to +18 dB. At Drive 1 the -18 dBFS
threshold therefore sits at -36 dBFS of real input. The 6 dB floor is reached 12 dB above threshold,
at -24 dBFS. Anything a cab normally sees (-18 to -6 dBFS) holds the gain at the floor, so the stage
becomes a fixed volume cut rather than a compressor.

Steady-state level change, 1 kHz sine. The excursion stage does almost nothing at 1 kHz, so this is the
compressor alone:

| input RMS | Drive 0.25 | 0.5 | 0.75 | 1.0 |
|---|---|---|---|---|
| -40 dBFS | 0.00 | 0.00 | 0.00 | 0.00 |
| -30 dBFS | 0.00 | 0.00 | -1.28 | -4.00 |
| -18 dBFS | -0.71 | -2.32 | -4.07 | -6.00 |
| -12 dBFS | -1.16 | -2.49 | -4.07 | -6.01 |
| -6 dBFS | -1.16 | -2.49 | -4.08 | -6.04 |

The -12 and -6 rows are identical: there is no compression in the range a cab normally sees. On a
100 Hz sine the excursion clip adds its own loss on top, so Drive 1 costs -7.2 dB at -18 dBFS and
-11.4 dB at -6 dBFS. An 82 Hz saw loses -6.9 to -9.8 dB. The IR Cab has no makeup gain for this.
Simple Cab has Auto Level.

### 2. The detector winds up, so recovery takes seconds, not "a fraction of a second"

The detector state keeps rising far past the point where the gain is already at the floor. Release
therefore has to drain that excess before the gain moves at all. Here is a loud passage (2 s of
100 Hz at -12 dBFS), then quiet playing:

| Drive | +0.1 s | +0.5 s | +1.0 s | +1.5 s | +2.0 s | +3.0 s |
|---|---|---|---|---|---|---|
| 0.5 | -2.49 | -2.42 | -1.41 | -0.22 | 0.00 | 0.00 |
| 1.0 | -6.00 | -6.00 | -6.00 | -4.95 | -2.45 | 0.00 |

The same wind-up makes the attack about 3 ms instead of the documented 50 ms. On a step from -40 to
-12 dBFS, the full 6 dB is already in place 5 ms later.

A fix for findings 1 and 2 needs these changes:
- Take `preGain²` out of the detector, or apply only part of it, so the threshold stays near the
  -18 dBFS nominal level.
- Clamp the detector state to `16 · T`, where the gain reaches its floor, so release starts at once.
- Consider a soft knee.
- Consider some makeup gain, so moderate playing stays level-neutral as Drive goes up.

The SimpleCab tests and `TestSpeakerDriveCompressesLoudLows` only check "loud gets quieter", so they
pass either way. A test that checks the -12 and -6 dBFS rows differ, and that recovery takes less than
about 0.5 s, would pin the intended behaviour.

### 3. An IR change crossfades from an outgoing side that has no history

**Fixed the same day:** `CapturePreviousConvolvers()` now moves the live convolvers into the
outgoing slots and leaves the live ones empty for the load to build. A moved-from convolver keeps its
`mInitialized` flag but loses its buffers, so the live one is replaced with an empty convolver
rather than left as moved. `LoadResource` now takes the capture after the WAV parse, so a file that
won't load leaves the cab playing. `ExpectCrossfade` now renders its outgoing reference with the
history the cab had. The new `TestReloadWhilePlayingHasNoDip` reloads the playing IR through both
load paths and requires every 1 ms window of the fade to stay within 1 dB of a cab that was never
reloaded. The old code gave -7.6 dB and fails 14 checks in `IRCabLoneSlotTests`.

`CapturePreviousConvolvers()` rebuilds the outgoing convolvers with `SetImpulse`, which starts them
empty. It does not move the live ones across. The incoming side starts empty too, so the first moments
of the 30 ms fade are both sides warming up from silence. Simulated on `421 1960.wav` → `Fat Boy.wav`
with a continuous low-E signal:

| window after the load | as built | outgoing keeps history | ideal crossfade |
|---|---|---|---|
| first 1 ms | -38.5 dB | -5.7 dB | -6.4 dB |
| first 3 ms | -8.0 dB | -5.0 dB | -5.5 dB |
| first 10 ms | -8.1 dB | -3.0 dB | -3.8 dB |

The result is a ~1 ms near-dropout and a few dB low for ~10 ms on every IR load: a tick when you swap
IRs while playing. `CommitDeferredRebuild()` already swaps the live convolvers into the outgoing slots
"history and all". `LoadResource(s)` could do the same: swap live into prev, then build the new ones.
`ExpectCrossfade` in `IRCabLoneSlotTests.cpp` currently writes the empty-history behaviour into its
expected output. It would need its reference changed to a cab that has been playing.

### 4. Auto Gain Comp assumes the two IRs are uncorrelated

`ComputeBlendCompGain` normalises by `sqrt(a² + b²)`. That is right for uncorrelated IRs, but two mics
on one speaker are strongly correlated. For `421 1960.wav` / `906 1960.wav`, ρ = 0.82, so IR Blend 0.5
plays **+2.6 dB** louder than either IR alone (`10·log10(1+ρ)` on white noise). Different cabs come
out nearer +1 dB. The comp also ignores IR A/B Level, and it is only valid with Normalize on. Fix:
take ρ as the dot product of the two built, normalised impulses at build time, then use
`a² + b² + 2abρ`.

### 5. Stereo IR truncation cuts one channel without a fade (low)

When Quality is below Full, `BuildConvolverPair` truncates L and R separately. Each gets its own
energy point and its own 64-sample fade. Both are then cut to the shorter length, so the longer channel
is cut off mid-signal with no fade. `GetProcessedImpulseStereo()` already does the joint truncation,
but nothing calls it.

### 6. Speaker Drive keeps its detector state through bypass (low)

With the cab disabled or at Mix 0, `Process()` returns before Speaker Drive runs, so its state freezes.
Re-enable after a loud passage and finding 2's wind-up carries over: up to ~1-3 s at -6 dB.
Switching Drive itself off and on is fine. The first sample at drive 0 clears the state.

### 7. Smaller items

- Dead code: `GetProcessedImpulseStereo`, `ComputeSignalEnergy` and `ComputeSignalEnergyStereo` are
  never called.
- The IR Cab leaves the excursion corner at the 180 Hz default. Simple Cab tunes it to the cabinet's
  resonance. An IR's low resonance peak could be estimated at build time. This is an enhancement.
- `SpeakerDrive.h`'s comment says the cubic "makes only the third harmonic". That is true only below
  the clamp at |u| = 1.5, which Drive 1 passes from about -14 dBFS on low notes: H5 is -22 dB at
  -6 dBFS. The no-oversampling conclusion still holds (next section).

## Checked and correct

- Drive 0 is exactly transparent. The band split is an identity (`input + drive·(bent − low)`), so
  small signals pass unchanged at any Drive. The dry path stays clean
  (`TestSpeakerDriveLeavesTheDryClean`).
- The 2:1 gain law `(T/P)^(1/4)` is correct for the power domain.
- Aliasing without oversampling is negligible. With an 82 Hz sine at Drive 1, even deep in the clamp
  region, the energy off the harmonic grid stays below -110 dB relative to the fundamental.
- Drive smoothing works, and so does the NaN/Inf guard. Turning Drive back up from 0 starts from clear
  state.
- Denormals in the one-pole states are covered by `ScopedNoDenormals` in the JUCE adapter and in Nano Q.
- IR normalisation (L2, anchored to 48 kHz) and resampling (preserves coefficient area) agree.
  `TestCabIR192kHzLevelMatches48kHz` covers them.

## Probes

All three are single `.cpp` files built with `cl /std:c++20 /O2 /I core/src`:

- **Speaker Drive levels, harmonics and recovery:** sines and a band-limited saw, run through
  `SpeakerDrive` directly. Gives the steady-state level change, the harmonics and off-grid aliasing
  (82 Hz on a 2 Hz DFT grid), and the step and recovery envelopes.
- **IR swap:** direct convolution of the two test IRs (L2-normalised, cut to the Standard 43 ms),
  with a linear 30 ms fade. Compares an outgoing side that starts empty with one that keeps its
  history.
- **Blend correlation:** ρ of two normalised IRs from `core/tests/testdata/assets/ir`.
