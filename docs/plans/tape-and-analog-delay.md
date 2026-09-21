# Tape Delay and Analog Delay Plan

Status: implemented (2026-09-21), not yet committed. Adds two character delays alongside
the existing `delay_digital` and `delay_doubler`: `delay_tape` (magnetic tape echo) and
`delay_analog` (bucket-brigade delay). The user-facing reference is now
`docs/fx-library.md`; this doc keeps the design reasoning. **Read "As built" first** —
measurement overturned several things the plan below proposed, and where the two
disagree, "As built" is right.

## As built

Six headers under `core/src/dsp/effects/`, split along what each part is for (and so that
every new file is under the 800-line budget):

| File | Holds |
|---|---|
| `DelayLineSupport.h` | Shared primitives: the Hermite delay line, glide ramp, LFO, filters with `Magnitude()`, saturators, `RailClip`, `EnvelopeFollower`, `IdleGate` |
| `DelayEffectSpec.h` | Shared scaffolding: `ParamSpec`, `FindParam`, `MakePreset`, `BuildParameterDefs`, tempo resolution, `SaturationDrive` |
| `TapeTransport.h` | Tape only: wow/flutter (`TapeTransport`) and dropouts (`TapeDropout`) |
| `Compander.h` | Analog only: the matched NE571-style pair |
| `TapeDelayEffect.h`, `AnalogDelayEffect.h` | The effects, their parameter tables and factory presets |

Tests: `core/tests/DelayCharacterTests.cpp` (60 checks), plus both effects in
`FastMathNanTests`' recover-from-non-finite-input cases. Every behaviour below was measured
with a standalone probe before it was written down.

### Where the build departed from the plan, and why

Each of these was found by measurement, not review.

1. **Wow and flutter are a speed error, not a fraction of delay time.** The plan scaled the
   time offset with delay length. That is the wrong physics: a capstan running fast shifts
   pitch by the same fraction however far apart the heads are. The model specifies pitch
   deviation and derives time swing as `dev × sr / (2π f)`. Measured: flutter makes 16
   cents from only 0.15 ms of swing, and wow's 54 cents peak-to-peak is identical at 60 ms
   and 1200 ms. The plan's test 6 ("more variation at 1200 ms") was replaced by its opposite.
2. **The analog delay feeds back before the expander.** With the tap after it (the plan's
   diagram), the compander added loop gain: Feedback 0.80 ran away to an amplitude of 14.
   The whole loop now lives in the compressed domain, so loop gain cannot exceed Feedback.
3. **The compander's clamps are matched**, the expander's being the compressor's raised to
   (1−a). Unmatched, the pair measured +2.8 dB above 0 dBFS.
4. **A fixed device rail replaces feedback-driven saturation.** The plan's "force drive
   when Feedback > 1" was measured backwards — more feedback gave a *quieter* runaway (0.49
   at 0.95 down to 0.08 at 1.15), since a tanh's ceiling is 1/gain. A tanh floor under the
   drive then cost 2.4 dB at −6 dBFS with Saturation at 0. `RailClip` is linear to −6 dBFS
   and approaches full scale, so Saturation 0 is clean and the loop is still bounded.
5. **Glide is speed-capped.** An exponential ramp alone moved the read head faster than
   real time: 300 → 600 ms read down to 28 Hz from 440 Hz. The cap keeps playback between
   half and 1.5× speed. Glide 0 is exempt, so it stays instant.
6. **A Time set before any audio snaps.** The executor applies stored parameters after
   Prepare, so every freshly loaded preset glided in from the default Time.
7. **Read before write.** Writing first put every repeat one sample early.
8. **The tape's feedback is normalised by the whole playback EQ's measured peak**, not the
   head bump's alone. Low Cut sits under the bump, so bump-only normalisation was so
   conservative that Feedback 1.10 could not self-oscillate at the defaults.
9. **Both effects gate their noise on the input** (`IdleGate`). Only the tape's hiss was
   planned to; the analog's BBD noise floor hissed at −95 dBFS forever. The gate needs a
   floor, or hiss was still measurable four seconds after playing stopped.
10. **Non-finite input is scrubbed on entry.** One NaN poisoned the compander's detector and
    left the analog repeats silent for good — in the IEEE build; `/fp:fast` happened to
    compare its way out. FastMathNanTests now covers both effects, wet only, because at the
    default Mix a silenced repeat path hides behind the dry signal.
11. **Multi-head modes feed back the sum of their heads** (at 1/n, so loop gain stays at or
    under Feedback), not the last head. This is what builds the patterns; it is a modelling
    choice to verify against a real machine, like the head ratios.
12. **`headMode` has seven entries with Head 3 first**, so the default echo is at exactly
    Time. The plan's first entry was Head 2, which would have put it at 0.633 × Time.
13. **The analog repeat lands at Time plus about 0.19 ms**, the four-pole reconstruction
    filter's own lag, as a real pedal's does. Its timing test checks "never early, and
    inaudible" rather than exactness.

Parameter counts are 17 (tape) and 16 (analog), not the 18 and 17 the tables below claimed.

### Measured

| | Tape | Analog |
|---|---|---|
| CPU, stereo 64-sample block @ 48 kHz | 4.0 µs (1 head), 5.1 µs (3 heads) | 3.7 µs |
| Share of the 1333 µs deadline | 0.3-0.4% | 0.3% |
| Repeat timing | exact at 44.1/48/96 kHz | Time + 0.19 ms |
| Self-oscillation | 1.02 → 0.019, 1.06 → 0.074, 1.10 → 0.14 | 1.02 → 0.04, 1.08 → 0.13, 1.15 → 0.22 |

The 25 µs gate held with room: per-sample costs are a Hermite read or three, a few filter
sections and a saturator, with no `std::sin`, `%` or allocation.

### Not done

- **`delaychar` profile in `ProfilerPresets.h`.** CPU was measured with standalone
  probes instead (`standalone-effect-harness` in memory), which isolate each effect. Add
  the profile if the effects ever need profiling inside a full chain.
- **Clock aliasing and whine** (analog), **tape hysteresis** and **ping-pong** remain
  deferred as below.
- **The open questions below** — RE-201 head ratios, the Echoplex time range, whether `age`
  earns being a macro — still need a listen or a service manual.

The point of both is *not* "digital delay with a low-pass on it". Each has one
mechanism that makes it recognisable, and if we only ship the tone controls we will
have built two more digital delays with different defaults:

- **Tape**: the delay time is a moving mechanical thing. Wow and flutter modulate it
  continuously, and changing the time *glides* the read head, which bends pitch.
- **Analog**: the delay time *is* the clock rate, so bandwidth and delay time are the
  same control. Longer delay means a slower clock means a lower anti-alias corner.
  That coupling is why a BBD pedal gets darker as you turn Time up, and it is the
  thing to get right.

Everything else (saturation, tone, noise, companding) is seasoning on top.

## Goals

1. Two new registered effects in the `delay` category, both usable live at
   48 kHz / 64 samples with CPU well inside the per-callback budget.
2. Tape: musical, controllable time-glide and a wow/flutter model that sounds like
   transport instability rather than a chorus LFO.
3. Analog: delay time drives BBD clock rate drives bandwidth, from one `stages`
   choice, so picking a chip and a time reproduces the right darkness.
4. Both self-oscillate safely — feedback above unity is a feature, non-finite output
   is not.
5. No regression to `delay_digital`, and no new UI message types.

## Non-goals (deferred, listed here so they are not silently dropped)

- **Ping-pong.** Neither machine does it, and `delay_digital` already covers that
  use. `spread` gives stereo width in v1. Revisit only if asked for.
- **Full BBD stage-by-stage simulation.** A physical BBD model that simulates charge
  transfer per stage is the gold standard and is far too expensive per sample for a
  live chain. See "BBD: what we model and what we fake".
- **Tape hysteresis (Jiles-Atherton).** A proper magnetic hysteresis model is a
  separate piece of work; v1 uses a static asymmetric waveshaper plus per-pass HF
  loss, which carries most of the audible result for an echo (as opposed to a
  tape-machine-on-the-mix-bus plugin).
- **A tape-echo spring reverb.** The RE-201's reverb is a separate tank; we already
  ship `reverb_spring`. Chain it.
- **Tap tempo UI.** Tempo sync via `syncMode`/`syncDivision` is enough for v1 and
  matches `delay_digital`.

## Current state

| Effect | GUID const | What it is |
|---|---|---|
| `delay_digital` | `kDelayDigital` | Clean stereo digital delay: HP/LP on repeats, ping-pong, LFO mod, feedback drive, ducking. 13 parameters, 2.1 s buffer. |
| `delay_doubler` | `kDelayDoubler` | Short delayed copy for stereo width. |

Sources: `core/src/dsp/effects/DelayEffect.h`, `DoublerEffect.h`; registration in
`BuiltinEffects.h`; UI catalog stub in `core/ui/ts/presetV2.ts`.

Two things in `DelayEffect` that the new effects should **not** copy:

- It computes `mWritePos = (mWritePos + 1) % bufSize` and two more `%` per channel
  per sample in `ReadInterp`. That is three integer divisions per sample per
  channel. Use a power-of-two buffer and a mask.
- It calls `std::sin` once per sample for the LFO. Use a recursive quadrature
  oscillator or a table.

Fixing those in `delay_digital` is a worthwhile separate change, not part of this
one.

`DelayEffect::SetParam("time", ...)` also snaps `mDelaySamples` immediately, which
clicks. The tape effect must ramp instead — and for tape that ramp is the feature,
not a workaround.

## Tape Delay (`delay_tape`)

Target machines: Maestro Echoplex EP-3 (single sliding playback head, the classic
slapback-to-long-repeat pedalboard echo) and Roland RE-201 Space Echo (three fixed
playback heads, capstan speed sets the time, head combinations give rhythmic
multi-tap patterns).

### What the model has to do

**1. Transport instability (wow and flutter).** The defining artifact. Model as a
per-sample time offset, in samples, added to the read position:

- *Wow*: slow, roughly 0.5-2 Hz, from reel and capstan eccentricity. Two
  incommensurate sines (e.g. 0.63 Hz and 1.17 Hz at unequal weights) so it never
  sounds like one LFO.
- *Flutter*: faster, roughly 6-30 Hz, from the capstan/idler and tape scrape. One
  sine near 11 Hz plus band-passed white noise, which is what stops it sounding like
  vibrato.
- Depth scales with delay time: on a real machine the *speed* error is what varies,
  so the *time* error grows with the length of tape between the heads. Model the
  wow/flutter offset as a fraction of the current delay time, not a fixed ms figure.
  This is why a 60 ms slapback stays stable and a 700 ms repeat warbles.

**2. Time changes glide.** `SetParam("time", ...)` sets a target; the read position
ramps toward it over `glide` ms. Because the read pointer is moving, the pitch of
material already on the tape bends — the sound of grabbing the Echoplex slider. At
`glide = 0` it behaves like a digital delay (useful for tempo-synced work); at
2000 ms a big time change is a long dive.

**3. Tape bandwidth, per pass.** Repeats darken cumulatively because the filters sit
inside the feedback loop, which is where `delay_digital` already puts them. Tape
adds two things on top:

- A **low-frequency head bump**: a gentle resonant lift around 80-120 Hz from the
  repro head geometry. This is what makes tape repeats sound fat rather than thin.
- **Age**: per-pass HF loss beyond the fixed `highCut`, plus dropout and hiss.

**4. Saturation.** Record amplifier plus magnetic saturation: an asymmetric soft
clip in the record path, referenced to the project's nominal operating level
(`kDefaultNominalOperatingLevelDbfs`, -18 dBFS, from `core/src/dsp/LevelTargets.h`)
so the knob means the same thing regardless of how hot the chain runs.

**5. Dropouts and hiss.** Driven by `age`. Dropout is a slow random amplitude and HF
wander (oxide shedding), *not* periodic. Hiss is filtered noise added inside the
feedback loop so it accumulates on repeats the way real tape noise does — but it
must be gated at idle so a preset sitting untouched is silent.

**6. Multiple playback heads.** The RE-201 feature: three read taps at fixed
physical spacing, so they scale together with capstan speed. `headMode` selects
which are active; `time` is the longest head. Feedback is taken from the *last
active* head, which is how the mode selector changes the repeat rhythm.

Head ratios: the model uses `head3 = time`, `head2 = 0.633 x time`,
`head1 = 0.317 x time`. **These ratios are an approximation and need verifying**
against an RE-201 service manual or a measured unit before we call the effect
RE-201-voiced in user-facing text (see Open questions).

### Parameters

18 parameters. Groups render as blocks in the node panel and `advanced: true`
parameters go to the Advanced tab (`core/ui/ts/signalPath/paramsPanel/panel.ts`).

| id | display | default | min | max | unit | group | adv |
|---|---|---|---|---|---|---|---|
| `time` | Time | 400 | 20 | 1500 | ms | Time | |
| `syncMode` | Sync | 0 | 0 | 1 | enum | Time | |
| `syncDivision` | Division | 4 | 0 | 14 | enum | Time | |
| `glide` | Glide | 120 | 0 | 2000 | ms | Time | |
| `feedback` | Feedback | 0.35 | 0 | 1.10 | amount | Time | |
| `headMode` | Heads | 0 | 0 | 5 | enum | Heads | |
| `wow` | Wow | 0.25 | 0 | 1 | amount | Tape | |
| `flutter` | Flutter | 0.25 | 0 | 1 | amount | Tape | |
| `age` | Tape Age | 0.35 | 0 | 1 | amount | Tape | |
| `saturation` | Saturation | 0.30 | 0 | 1 | amount | Tape | |
| `highCut` | High Cut | 5000 | 500 | 16000 | Hz | Tape | yes |
| `lowCut` | Low Cut | 90 | 20 | 800 | Hz | Tape | yes |
| `headBump` | Head Bump | 0.35 | 0 | 1 | amount | Tape | yes |
| `mix` | Mix | 0.30 | 0 | 1 | amount | Output | |
| `level` | Level | 0 | -12 | 12 | dB | Output | |
| `spread` | Spread | 0 | 0 | 50 | ms | Output | yes |
| `ducking` | Ducking | 0 | 0 | 1 | amount | Output | yes |

`headMode` labels: `Head 2`, `Head 3`, `Heads 1+2`, `Heads 2+3`, `Heads 1+3`,
`Heads 1+2+3`. Index 0 is a single mid head, i.e. a plain single-tap echo, so the
default is the EP-3 behaviour and the multi-head patterns are opt-in.

`GetParam("effectiveTimeMs")` returns the resolved time the way `delay_digital`
does, so the UI can read out a tempo-synced value.

Max time is 1500 ms rather than the digital delay's 2000: past that the wow/flutter
scaling makes it warble more than it echoes, and no target machine goes there.

## Analog Delay (`delay_analog`)

Target pedals: Boss DM-2 (one MN3005, roughly 20-300 ms) and EHX Deluxe Memory Man
(two MN3005 in series, roughly to 550 ms), both with NE570/571 companding.

### BBD: what we model and what we fake

A bucket-brigade device is an analog shift register of `N` stages clocked at
`f_clock`. Two clock phases are needed per stage transfer, so:

```
delay_seconds = N / (2 * f_clock)
```

Turning the Time knob changes `f_clock`. That means **the BBD's own Nyquist
frequency is `f_clock / 2`, and it moves with the Time knob**. The pedal's job is to
keep everything below that, which is why BBD pedals have steep Sallen-Key anti-alias
filters in front and reconstruction filters behind, and why the repeats collapse in
bandwidth as Time goes up.

Worked numbers for the parameter defaults (`N = 4096`, MN3005):

| Time | `f_clock` | BBD Nyquist | Usable audio bandwidth |
|---:|---:|---:|---:|
| 40 ms | 51.2 kHz | 25.6 kHz | full |
| 100 ms | 20.5 kHz | 10.2 kHz | bright |
| 300 ms | 6.8 kHz | 3.4 kHz | dark — DM-2 at max |
| 600 ms | 3.4 kHz | 1.7 kHz | very dark, telephone |

The same table for `N = 1024` (MN3007) shows why those chips only ever appeared in
short delays and choruses: at 300 ms the clock is 1.7 kHz and Nyquist is 853 Hz.

**What we model:** the clock rate derived from `stages` and the effective delay time,
and an anti-alias/reconstruction filter pair whose corner tracks
`min(0.40 * f_clock, fixed_ceiling)`. That single coupling produces the
characteristic time-to-darkness behaviour and is cheap.

**What we fake:** we do *not* resample the delay line to the BBD clock. The line runs
at host rate with Hermite interpolation on a fractional read. What that loses is
genuine clock aliasing and clock whine — audible on a real DM-2 as a faint high
whistle, and arguably a flaw rather than a feature. We also approximate charge
transfer loss (each stage bleeds a little into the next) as a fixed extra one-pole
per pass scaled by `stages`, rather than an N-stage cascade.

If a listening test later says the fake is not convincing, the escalation is a
2x-8x oversampled variable-rate inner line, not a bigger filter. Measure before
building it.

**Companding.** A BBD's noise floor is bad, so the pedal compresses into it and
expands out of it. Model as a matched RMS-detector compressor/expander pair (roughly
2:1 in, 1:2 out) around the delay line. Two audible consequences, both wanted:

- Repeats *breathe*: the expander's release pumps the tail.
- Noise pumps with signal rather than sitting constant.

`compander = 0` bypasses the pair for a clean modern voicing; `1` is full NE571-ish
breathing.

### Parameters

17 parameters.

| id | display | default | min | max | unit | group | adv |
|---|---|---|---|---|---|---|---|
| `time` | Time | 320 | 20 | 800 | ms | Time | |
| `syncMode` | Sync | 0 | 0 | 1 | enum | Time | |
| `syncDivision` | Division | 4 | 0 | 14 | enum | Time | |
| `glide` | Glide | 40 | 0 | 500 | ms | Time | yes |
| `feedback` | Feedback | 0.35 | 0 | 1.15 | amount | Time | |
| `stages` | BBD Stages | 3 | 0 | 3 | enum | BBD | |
| `tone` | Tone | 0.50 | 0 | 1 | amount | BBD | |
| `compander` | Compander | 0.70 | 0 | 1 | amount | BBD | |
| `saturation` | Saturation | 0.35 | 0 | 1 | amount | BBD | |
| `noise` | Noise Floor | 0.20 | 0 | 1 | amount | BBD | yes |
| `modRate` | Mod Rate | 0.40 | 0 | 8 | Hz | Modulation | |
| `modDepth` | Mod Depth | 0 | 0 | 12 | ms | Modulation | |
| `mix` | Mix | 0.30 | 0 | 1 | amount | Output | |
| `level` | Level | 0 | -12 | 12 | dB | Output | |
| `spread` | Spread | 0 | 0 | 50 | ms | Output | yes |
| `ducking` | Ducking | 0 | 0 | 1 | amount | Output | yes |

`stages` labels: `1024 (MN3007)`, `2048 (MN3008)`, `3328 (MN3011)`, `4096 (MN3005)`,
default index 3. Step 1, so it is a discrete enum.

Max time 800 ms: past a doubled MN3005 the model has nothing left to be faithful to.
`feedback` reaching 1.15 is deliberate — analog delays self-oscillate musically, and
the compander plus saturator is what keeps the runaway bounded (see Safety).

`glide` is advanced and short by default: a BBD's clock VCO settles fast, so this is
a click-avoidance ramp with a little character, not the tape effect's pitch dive.

## Shared support: `core/src/dsp/effects/DelayLineSupport.h`

Both effects need the same primitives, and the 800-line budget for a new file
(`node tools/check-cpp-file-sizes.js`) means neither header should carry its own
copy. New header, namespace `guitarfx::delay_line`:

| Piece | Why |
|---|---|
| `FractionalDelayLine` | Power-of-two buffer, mask wrap (no `%`), `Write`, `ReadHermite`, `ReadLinear`, `Resize`, `Clear`. Hermite matters: a modulated read pointer through linear interpolation has a frac-dependent HF loss, which on a wow/flutter signal *is* audible as a shimmer. |
| `GlideRamp` | One-pole ramp of a delay-time target, in samples, with a settable time constant. Drives both `glide` parameters. |
| `TapeTransport` | Wow + flutter generator: recursive quadrature oscillators plus band-passed noise, returning an offset as a fraction of delay time. |
| `QuadOscillator` | Recursive sine, so nothing calls `std::sin` per sample. |
| `OnePoleLp` / `OnePoleHp` / `LowShelf` | Tone shaping; the shelf covers the tape head bump. |
| `SoftSaturate` / `AsymSaturate` | Shared waveshapers, referenced to the nominal operating level. |
| `FlushDenormal` | Same idiom as `WahEffect`. |

Estimated sizes: support header ~280 lines, `TapeDelayEffect.h` ~620,
`AnalogDelayEffect.h` ~600. All under budget with room.

Both effect headers follow the `WahEffect.h` structure, which is the current house
style for a new effect: a `Param` enum, one `constexpr std::array<ParamSpec, N>` that
registration *and* `SetParam` clamping both read, a `FindParam`, and
`FactoryPresets()`.

## Factory presets

Both ship `EffectPresetDefinition` sets, as `WahEffect` does, so a user gets the
sound without learning 18 knobs. Proposed:

**Tape** — `Slapback` (90 ms, low feedback, minimal wow, head 2), `Echoplex`
(400 ms, moderate age and saturation, single head — the default), `Space Echo`
(500 ms, heads 1+2+3, more wow), `Worn Tape` (600 ms, age 0.85, saturation 0.6),
`Dub` (450 ms, feedback 1.02, high saturation — self-oscillating).

**Analog** — `DM-2` (300 ms, 4096 stages, compander 0.8 — the default),
`Memory Man` (500 ms, 4096, modDepth 6), `Short Analog` (80 ms, 1024 stages),
`Clean Analog` (250 ms, compander 0.1, tone 0.75), `Runaway` (400 ms,
feedback 1.12).

Mark one `isDefault` in each so a freshly added node sounds right. Note from
`EffectRegistry.h`: preset order maps to enum indices that saved presets reference,
so new presets get appended, never inserted.

## Safety: self-oscillation and fast math

Feedback above 1.0 on both effects means the loop is deliberately unstable and
bounded only by the saturator. Per `core/src/dsp/FiniteCheck.h` and
`core/tests/FastMathNanTests.cpp`:

- Detect non-finite values with `FiniteCheck.h`. Never `std::isnan`/`std::isfinite`
  directly — Release builds with `/fp:fast` or `-ffast-math` fold those away, and
  clang folds NaN constants too.
- Never use NaN or infinity as a sentinel anywhere in either effect.
- Add a hard ceiling in the feedback path: if the fed-back sample exceeds a safe
  magnitude, clamp it. The saturator should make this unreachable; it is there
  because "should" is not a guarantee when a host changes sample rate mid-tail.
- Both effects get a `FastMathNanTests` case: drive feedback to max, run to
  self-oscillation for several seconds at 44.1/48/96 kHz, sweep `time` across its
  full range during it, and assert every output sample finite and bounded.
- A Debug pass proves nothing here. These cases must run in the Release and clang-cl
  builds (the clang-cl configure line is in `docs/agent-quickstart.md`).

Audio-thread rules apply as everywhere: no allocation, no locks, no I/O in
`Process`. All buffers are sized in `Prepare` for the maximum time plus modulation
headroom; `SetParam` only sets targets and recomputes coefficients.

## Stereo behaviour

Both are mono machines. Implementation:

- `SupportsMonoProcessing()` returns true, with a real `ProcessMono` fast path — a
  mono chain should pay for one delay line, not two.
- `ProducesStereoOutput()` returns true when `spread > 0`. Note that
  `DelayEffect::ProducesStereoOutput()` currently only reports the ping-pong mode
  and ignores its own `spread`; do not copy that.

## Change footprint

| File | Change |
|---|---|
| `core/src/dsp/EffectGuids.h` | `kDelayTape = c46cecdc-d800-416e-a9cf-b9a13bd3ab35`, `kDelayAnalog = 5c3965fc-cf52-4d14-9e5a-8a7120a9aae4` under the Delay heading |
| `core/ui/ts/effectGuids.ts` | The same two constants, plus `delay_tape` / `delay_analog` in `EFFECT_ALIAS_MAP` |
| `core/protocol/effect-aliases.json` | Two entries; `EffectAliasParityTests` and `tests/effectAliases.test.ts` then check both sides automatically |
| `core/src/dsp/effects/DelayLineSupport.h` | New shared header |
| `core/src/dsp/effects/TapeDelayEffect.h` | New |
| `core/src/dsp/effects/AnalogDelayEffect.h` | New |
| `core/src/dsp/effects/BuiltinEffects.h` | Two includes, two `Register...()` calls under "Time-based effects" |
| `core/ui/ts/presetV2.ts` | Two `EFFECT_STUBS` entries under `// Delay` |
| `core/ui/ts/iconAssets.ts` | Two `effectIcons` entries |
| `core/ui/ts/signalPath/visualization.ts` | Rack images — reuse `studio-rack-delay.png` unless art is made |
| `core/tests/DelayCharacterTests.cpp` | New; add to `core/tests/CMakeLists.txt` |
| `core/tests/FastMathNanTests.cpp` | Self-oscillation cases for both |
| `docs/fx-library.md` | GUID table (~line 70), category table (~line 145), parameter sections after Digital Delay |
| `docs/features.md` | Section 2.7 delay table |

Nothing here adds a UI message, so `core/protocol/ui-messages.json` is untouched.
`node tools/check-protocol.mjs` still has to pass — it compares `EffectGuids.h`
against `effectGuids.ts` name by name and value by value.

## Verification

- `node tools/check-protocol.mjs` — GUID parity.
- `node tools/check-cpp-file-sizes.js` — new files under 800 lines.
- `cd core/build && ctest -C Debug --output-on-failure` — including the new
  `DelayCharacterTests` and `EffectAliasParityTests`.
- Release + clang-cl `FastMathNanTests` — the self-oscillation cases.
- `cd core/ui && npm run verify`.
- `node tools/agent-ui-debug/smoke-test.mjs` — catches the import-cycle TDZ crash
  `tsc` cannot see. Worth running because `presetV2.ts` and `iconAssets.ts` both move.
- CPU: `SteadyStateProfiler` built RelWithDebInfo, run from the repo root. Add a
  `delaychar` profile to `core/tests/helpers/ProfilerPresets.h` rather than editing
  `applive`, whose numbers are a baseline we compare against.
- Live check in the running app over WebView2 remote debugging
  (`tools/agent-ui-debug/README.md`): add each effect, sweep Time while sound is
  playing, confirm the tape glide bends pitch and the analog delay darkens, and
  confirm the node panel groups and Advanced tab render. Then
  `taskkill //F //IM "Soundshed Guitar.exe"` and clean any test presets out of
  `%APPDATA%\Soundshed Guitar\`.

### CPU budget

At 48 kHz with a 64-sample ASIO buffer the whole callback has ~1333 us. Per-sample
cost for either effect is one Hermite read, two to four one-pole/biquad sections, a
waveshaper, and (analog only) a compander detector pair — order 100 ns per sample per
channel, so roughly 13 us per 64-sample stereo block, about 1% of the deadline. The
tape effect's three-head mode costs three reads instead of one.

**Gate: 25 us per 64-sample stereo block at 48 kHz, measured, per effect.** If either
misses it, the cause is a design mistake (per-sample coefficient recalculation, a
`std::sin`, a `%`), not a reason to raise the number.

## Test plan

`core/tests/DelayCharacterTests.cpp`, asserting mechanism rather than taste:

**Both**

1. Impulse in, feedback 0: the first repeat lands within a sample or two of the set
   time, at 44.1, 48 and 96 kHz.
2. Tempo sync: `syncMode = Tempo`, a division and a BPM give the expected
   `effectiveTimeMs`, matching the existing `delay_digital` case in
   `EffectProcessorTests.cpp` (quarter note at 120 BPM = 500 ms).
3. `mix = 0` is dry passthrough to within epsilon.
4. `Reset()` clears the tail: process noise, reset, process silence, assert output is
   silent.
5. Time swept from min to max while running produces no sample-to-sample
   discontinuity above a threshold — the glide ramp is doing its job.

**Tape**

6. `wow = flutter = 0` gives a stable delay time; with `wow = 1` the measured period
   of a repeated impulse train varies, and the variation is larger at 1200 ms than at
   60 ms (the time-proportional scaling).
7. Successive repeats are monotonically darker: measure the spectral centroid of
   repeats 1, 2 and 3 and assert it falls.
8. `headMode` with three heads produces three distinct taps at the expected ratios.
9. `age = 0` produces no added noise in silence; `age = 1` produces some.

**Analog**

10. Bandwidth tracks time: measure the -3 dB point of the first repeat at 50, 150,
    300 and 600 ms with `stages = 4096` and assert it falls monotonically and lands
    near `0.40 * f_clock`.
11. Bandwidth tracks `stages`: same time, 1024 stages is darker than 4096.
12. Companding: with `compander = 1`, a loud burst followed by silence shows a
    measurable noise-floor decay (pumping) that is absent at `compander = 0`.
13. Self-oscillation at `feedback = 1.15` reaches a bounded steady state rather than
    growing without limit.

## Build order

1. `DelayLineSupport.h` plus unit tests for `FractionalDelayLine` (Hermite accuracy
   against a known sine, mask wrap correctness at buffer boundaries) and `GlideRamp`.
   Nothing registered yet.
2. `AnalogDelayEffect.h` first, not tape. It is the simpler transport (no multi-head,
   short glide) and its headline behaviour — clock-rate-driven bandwidth — is a
   closed-form thing the tests can pin exactly. Register it, wire the UI stubs, get
   tests 1-5 and 10-13 green.
3. `TapeDelayEffect.h`: single head first, then wow/flutter, then the head modes.
4. Factory presets for both.
5. Docs, then the live app check.

Steps 2 and 3 are each a reviewable change on their own. Do not land both effects in
one commit.

## Open questions

1. **RE-201 head ratios.** The 1 : 0.633 : 0.317 spacing above is an approximation.
   Before any user-facing text claims Space Echo voicing, check a service manual or
   measure a unit. If we cannot verify, keep the ratios but describe the effect as
   "three-head tape echo" rather than naming the machine.
2. **Echoplex time range.** The EP-3's sliding head gives roughly 75-700 ms. The
   proposed 20-1500 ms range is deliberately wider than any single machine. Confirm
   that is the call — the alternative is a narrower, more opinionated range.
3. **Does `age` earn being a macro?** It currently drives per-pass HF loss, dropout
   depth and hiss together, with `highCut`/`lowCut` as separate explicit controls.
   The risk is that `age` and `highCut` feel like they fight each other. Worth
   deciding at the first live listen.
4. **Icons and rack art.** Both reuse the existing `delay` icon and
   `studio-rack-delay.png` in this plan. If distinct art is wanted, that is a separate
   task and should not block the DSP.
5. **Should these ship behind the experimental flag first?**
   `core/ui/ts/fxSelector.ts` gates `kTransposeStft` and `kTransposeHybrid` behind
   `Features.ExperimentalEffects`. These two are self-contained and low-risk, so the
   proposal is to ship them unflagged — but flagging the first landing is cheap if
   preferred.
