# Multi-band Gate

A noise gate that splits the signal into six frequency bands and turns down only the bands
whose level has fallen below their own threshold. When a note decays, its top end (where the
hiss is) fades out first while the body of the note keeps playing, so there is no single moment
where everything shuts. Learn listens to the noise with the strings muted and sets all six
thresholds.

Effect id `dynamics_multiband_gate`, GUID `51e2f89c-c174-4c1b-aa40-2e5a08dc745c`, category
`dynamics`. Engine: `core/src/dsp/effects/MultibandGateEffect.h` (+ `MultibandGateSupport.h`).

## Signal flow

```
in ─ LR4 split @150 ─┬─ band 1 ──────────── AP 400 · AP 1k · AP 2.5k · AP 6k ─ × g1 ─┐
                     └─ LR4 split @400 ─┬─ band 2 ─── AP 1k · AP 2.5k · AP 6k ─ × g2 ─┤
                                        └─ … @1k, @2.5k, @6k                         ├─ Σ ─ out
                                                                     band 6 ─ × g6 ─┘
```

- **Bands.** Fixed crossovers at 150, 400, 1000, 2500 and 6000 Hz: lows (hum, rumble), low
  mids (body), mids, upper mids, presence, air (most of the hiss). Fixed, so a learned
  threshold keeps meaning the same band.
- **Crossovers are Linkwitz-Riley 4th order** (two Butterworth sections each side) in a
  cascade, and each lower band runs through the second-order all-passes of the crossovers above
  it, so the six bands sum to one all-pass: flat magnitude when every band is open. A split by
  subtraction (what Noise Reduction's hiss stage does) adds back exactly, but its bands leak at
  about 6 dB/octave: a 1 kHz note reads only 8 dB down in the >6 kHz band, which would hold the
  air band open on every note and colour the mids whenever it closed. LR4 bands fall 24 dB/oct.
  Cost: 30 biquads per channel per sample.
- **Detector per band:** the band's mean square, smoothed by a one-pole whose time constant
  suits the band (20 ms lows, 8 ms low mids, 4 ms mids, 3 ms above), so a low note's ripple
  stays within about a dB. Linked across channels (the louder side decides, as the gate does),
  separate in dual mono.
- **Gain per band: a soft downward expander,** not a switch. Below a band's threshold its gain
  falls 4 dB per dB (1:5 expansion) through a 6 dB soft knee, down to Range. Worked out every
  16 samples; the applied gain ramps per sample, opening at Attack and closing at Release.
  There is no hysteresis or hold: a smooth gain curve does not chatter.
- **No latency.** Zero-latency IIR throughout.

## Parameters

| Key | Name | Range | Default |
| --- | --- | --- | --- |
| `threshold1`…`threshold6` | Lows, Low Mids, Mids, Upper Mids, Presence, Air (group Thresholds) | −96…0 dB | −65 dB |
| `offset` | Offset — moves all six together | −24…+24 dB | 0 |
| `attack` | Attack | 0.1…50 ms | 1 ms |
| `release` | Release (log taper) | 5…1000 ms | 120 ms |
| `range` | Range — the deepest cut | −80…0 dB | −40 dB |

Factory presets leave the thresholds and Offset alone (they belong to the player's pickups,
as the gate's Threshold does) and set the timing and Range.

## Learn

Reuses the noise gate's Learn end to end rather than adding a second protocol:

- **Engine.** Six `GateNoiseLearner`s, one per band detector, started under one session and fed
  in lockstep. A small interface, `NoiseLearnHost` (in `NoiseGateLearn.h`), gives
  `GateLearnService` the learners and the threshold each suggests; `NoiseGateEffect` implements
  it with one band, `MultibandGateEffect` with six. Threshold per band = floor + 8 dB margin
  (so the floor itself is cut by about 20 dB), clamped to the range. Offset is not included:
  Learn sets the thresholds as heard, and Offset stays the player's trim.
- **Message.** `gateLearn` is unchanged. `gateLearnStatus` keeps its single-band fields for the
  noise gate; a multi-band listen adds `bands: [{levelDb?, noiseFloorDb?, spreadDb?,
  thresholdDb?}]` (live levels while listening, results when done). A listen fails as
  `playing` if any band heard playing.
- **Web UI.** The node panel's Noise Floor section draws one meter row per band (noise heard,
  threshold, live level) with one Learn button; `gateLearn.ts` grows a band-aware view and the
  preset applier sets `threshold1..6` as ordinary param edits (undo, dirty, host).
- **Nano Q.** Gets the effect's knobs from the catalogue like any effect. Its Learn view is
  single-band; a six-row version is a follow-up.

## Tests (`core/tests/MultibandGateEffectTests.cpp`)

- Unity gains reconstruct an all-pass: output magnitude flat within 0.1 dB, 20 Hz–20 kHz.
- Band isolation: a tone in one band below threshold is cut; a loud tone in another band does
  not open it.
- A decaying note: the air band closes before the lows (the point of the effect).
- Noise below every threshold comes out at least Range-ish lower; noise above passes.
- Learn: noise-only listen gives six floors and thresholds within a dB or two of the band
  levels; a played note fails as playing.
- Mono/stereo parity, dual mono, NaN recovery (also in `FastMathNanTests`), factory presets
  (`EffectFactoryPresetTests`), protocol and alias parity via the existing checks.
