# Multi-band Compressor

A compressor that splits the signal into four frequency bands and compresses each on its own
level, with its own threshold, ratio and makeup. A boomy low string can be held down without
the pick attack up top being squashed with it, or a fizzy top end tamed without thinning the
body. The node panel draws the bands over the live spectrum so the player can see which band
is being compressed and by how much.

Effect id `dynamics_multiband_compressor`, GUID `2bdf5236-8d4a-485c-9302-3a0479c3614d`, category
`dynamics`. Engine: `core/src/dsp/effects/MultibandCompressorEffect.h`
(+ `MultibandCompressorSupport.h`); the crossover is shared with the Multi-band Gate in
`core/src/dsp/LinkwitzRileyCrossover.h`.

## Signal flow

```
in ─ LR4 split @x1 ─┬─ band 1 ─ AP x2 · AP x3 ─ × g1 ─┐
                    └─ LR4 split @x2 ─┬─ band 2 ─ AP x3 ─ × g2 ─┤
                                      └─ LR4 split @x3 ─┬─ band 3 ─ × g3 ─┼─ Σ ─ × output ─ out
                                                        └─ band 4 ─ × g4 ─┘
 g = mix · compression · makeup + (1 − mix)
```

- **Bands.** Four, split by three crossovers the player can move: Low (boom, default below
  200 Hz), Low Mid (body and mud, 200–800 Hz), High Mid (bite, 800 Hz–3 kHz) and High (pick
  and fizz, above 3 kHz). A crossover is kept at least a third of an octave above the one
  below it, whatever the knobs say, so a band never closes up.
- **Crossovers are Linkwitz-Riley 4th order with all-pass compensation**, the Multi-band
  Gate's (see its plan for why not a subtraction split): with every band at unity the four
  sum to a flat all-pass. The gate's crossover becomes a template on the band count with the
  split frequencies passed in, and both effects use it. Moving a crossover redesigns its
  biquads at the next block; the state carries over.
- **Detector per band:** the band's mean square over a short window that suits the band
  (8, 4, 2 and 1 ms, low band first), so a low note's ripple does not read as level.
  Linked across channels (the louder side decides), separate in dual mono.
- **Gain computer per band:** soft knee (6 dB), threshold and ratio per band, in dB, worked
  out every 16 samples. The gain reduction is smoothed in the dB domain with Attack and
  Release; the low band's times are doubled and the low mid's 1.5×, since a fast release on a
  low note pumps and distorts its waveform.
- **Parallel mix per band.** Mix blends each band's compressed signal with the same band
  uncompressed, so dry and wet share the crossover's phase and a half-way Mix does not comb.
  Makeup applies to the compressed part only; Output trims the sum. Makeup, Mix and Output
  glide over 20 ms, as the VCA compressor's do.
- **No latency.**

## Parameters

| Key | Name | Range | Default |
| --- | --- | --- | --- |
| `crossover1` | Low / Low Mid (group Crossovers, log) | 40…1000 Hz | 200 Hz |
| `crossover2` | Low Mid / High Mid (log) | 150…5000 Hz | 800 Hz |
| `crossover3` | High Mid / High (log) | 600…12000 Hz | 3000 Hz |
| `threshold1`…`threshold4` | Threshold, per band (groups Low, Low Mid, High Mid, High) | −60…0 dB | −24 dB |
| `ratio1`…`ratio4` | Ratio, per band (log) | 1…20 | 3 |
| `gain1`…`gain4` | Makeup, per band | −12…+12 dB | 0 |
| `attack` | Attack (log) | 0.1…100 ms | 10 ms |
| `release` | Release (log) | 10…1000 ms | 120 ms |
| `mix` | Mix | 0…1 | 1 |
| `output` | Output | −18…+18 dB | 0 |

Factory presets: Standard (defaults), Tame the Boom, Tighten Lows (high gain), Even Out
(clean), Smooth Top (fizz), Glassy Clean, Parallel Squash, Gentle Glue.

## Visualisation: the bands over the spectrum

The node panel opens with a plot on the EQ's log-frequency axis:

- **Input spectrum** as the shaded backdrop, as behind an EQ curve.
- **Output spectrum** as a line over it, so the gap between them in a band is what that band
  is losing (and makeup puts back).
- **Four band columns**, divided at the crossovers the engine is using. Each crossover line
  can be dragged to move it (an ordinary param edit: undo, dirty, host automation).
- **Gain reduction** hangs from the top of each column, on a 0–24 dB scale, with its number,
  coloured while the band is compressing.
- **A meter strip under the plot**, one per band: the band's level against its threshold
  marker, so it is plain how far over the threshold each band is being driven.

### Engine side

- The spectrum feed (`setSpectrumWatch` → `sldS`, `core/src/dsp/SpectrumTap.h`) taps a node's
  input. A node whose effect has **band meters** is also tapped after it runs, through a
  second tap that only that kind of node gets, so the EQ's feed costs nothing more.
- `EffectProcessor` gains `BandMeterCount()` and `ReadBandMeters(span<BandMeter>)`: per band,
  the frequency range the engine is using, the deepest gain reduction since the last read and
  the detector level, both in dB. Atomics written by the audio thread once a block; read on
  the message thread with no allocation, as the analyzer's telemetry is.
- `SignalGraphExecutor::ReadWatchedSpectrum` fills a `WatchedSpectrum` (input bins, optional
  output bins, band meters). `sldS` gains `o` (output bins, same range as `s`) and `b`:
  `[[lowHz, highHz, gainReductionDb, levelDb], …]`, both only for such a node. Older readers
  ignore them; the EQ's frames are unchanged. Each band also carries its threshold
  (`BandMeter::thresholdDb`, a fifth element), added for the Distortion; this panel still
  draws the mark from the knobs so it follows a turn at once.

### Web UI

- `eqSpectrum.ts` parses `o` and `b` into `EqSpectrum.outputBinsDb` and `.bands`.
- `signalPath/paramsPanel/multibandPanel.ts` builds the section, draws on the shared
  `eqPlot.ts` axis and spectrum, and handles crossover drags; `multibandPlot.ts` draws. Both
  are shared with the Multi-band Distortion (docs/plans/multiband-distortion.md). CSS in
  `css/multiband.css`.
- Nano Q gets the knobs from the catalogue like any effect. A native band view is a follow-up.

## Tests (`core/tests/MultibandCompressorEffectTests.cpp`)

- Below every threshold the four bands reconstruct an all-pass: flat within 0.1 dB,
  20 Hz–20 kHz, at default and moved crossovers.
- Band isolation: a loud low tone is compressed by the expected static curve; a quiet high
  tone alongside it keeps its level.
- Static curve: a steady tone above threshold settles to threshold + over/ratio (within the
  knee's reach), per band.
- Crossover ordering: crossovers set out of order are kept a third of an octave apart.
- Mix 0 is the uncompressed all-pass; Makeup and Output move the level by what they say.
- Band meters report the crossovers in use, and gain reduction where a band is compressing.
- Mono/stereo parity, dual mono, NaN recovery (also in `FastMathNanTests`), factory presets
  (`EffectFactoryPresetTests`).
- Executor: a watched multi-band node yields output bins and band meters; an EQ node does not.
- UI: `sldS` parsing of `o` and `b`; the panel's crossover drag maths.
