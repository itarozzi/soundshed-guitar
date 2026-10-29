# Multi-band Distortion

A distortion that splits the signal into four frequency bands and drives each on its own, with
its own amount, clipping character and level. Driving a whole chord through one clipper makes
every note intermodulate with every other, and the low strings' energy pushes the clipper
hardest, so the chord turns to mud and the lows flub. Split first and each band only
intermodulates with itself: the lows can stay tight and nearly clean while the mids scream,
and a chord keeps its notes. The node panel draws the bands over the live spectrum in and out,
with how hard each band is being driven into its clipper.

Effect id `multiband_distortion`, GUID `86222aa0-53a4-4768-a05a-c68ef29a2bf4`, category `drive`.
Engine: `core/src/dsp/effects/MultibandDistortionEffect.h` (+ `MultibandDistortionSupport.h`).
It shares its crossover with the Multi-band Gate and Compressor
(`core/src/dsp/LinkwitzRileyCrossover.h`), and its bands, crossover ranges and defaults with
the Compressor, so the two line up when used together.

## Signal flow

```
in ─ up ×N ─ LR4 split @x1..x3 ─┬─ band 1 ─ shape₁ ─ mix ─ × level₁ ─┐
                                ├─ …                                  ├─ Σ ─ down ×N ─ DC ─ × output ─ ceiling ─ out
                                └─ band 4 ─ shape₄ ─ mix ─ × level₄ ─┘

 shape(x) = T · clip(G·x / T) · comp(G) / G        T = nominal + 12 dB, G = Drive
 mix      = Mix · shape(band) + (1 − Mix) · band
```

- **Oversampled as a whole.** The input goes up once (the drive pedals' minimum-phase
  half-band IIRs, `dsp/HalfBandIir.h`), is split, shaped and summed at the high rate, and
  comes down once. One round trip per channel instead of one per band, and every band and its
  dry share the same phase, so Mix and the band sum stay coherent. 4× at 44.1/48 kHz, 2× at
  88.2/96, 1× above: about 192 kHz. Every clipper is antialiased as the drive pedals' are
  (first-order antiderivative, `drive::ClipCurve::Antialiased`), which is what lets 4× do
  where the pedals need 8× for their much harder gain.
- **Crossovers** are the Compressor's: LR4 with all-pass compensation, three movable splits
  kept a third of an octave apart (the ordering moves into `LinkwitzRileyCrossover.h` so all
  three effects share it), designed at the oversampled rate. With every band clean the four
  sum to a flat all-pass.
- **Drive per band** is 0 to 48 dB of gain into a clipper whose knee sits 12 dB above the
  nominal operating level (`LevelTargets.h`), so at 0 dB a normal note passes all but clean
  and at 48 dB it is a square wave.
- **Level-matched.** `comp(G)` is the ratio by which the clipper loses an RMS at the nominal
  level, relative to Drive 0, from a table built once per character (49 points, one per dB,
  interpolated). Turning Drive up changes how dirty the band is, not how loud: a nominal note
  comes out as loud as at Drive 0 (measured within 0.85 dB at 36 dB, every Character), and a
  quiet signal at Drive 0 passes at unity. Quieter playing comes up relative to it, as it does
  through any clipper, and so does whatever leaks into a band from its neighbours: at high
  Drive in every band the bands even out, which is what a multi-band clipper does.
- **Character per band**, one of five clipping curves, all clean and slope 1 for small
  signals:
  | Character | Curve | Sound |
  | --- | --- | --- |
  | Tape | atan, symmetric (`Knee::Gradual`) | the softest bend; odd harmonics that come in slowly |
  | Tube | soft knee, negative side at 0.7 | even harmonics too: warm, the default |
  | Diode | soft knee, symmetric | sharper than Tape, odd harmonics |
  | Hard | flat limit, symmetric | an op-amp's rails: bright and buzzy |
  | Fuzz | flat limit, negative side at 0.5 | hard and lopsided: gated, ragged |

  Changing one crossfades the band from the old curve to the new over 10 ms, so it does not
  click.
- **Level per band** (−24…+12 dB) after the mix; Mix blends each band's shaped signal with
  the same band clean, in phase. Drive, Level, Mix and Output glide over 20 ms.
- **DC blocker** at 2 Hz after the sum (the lopsided curves make DC), then the drive pedals'
  soft ceiling near 0 dBFS.
- **No latency** reported (the half-bands are minimum phase, as for the drive pedals).
- **Cost:** about 24 µs per stereo 64-sample block at 48 kHz with Release flags (1.8% of the
  1333 µs deadline); the 8× Distortion pedal is 30 µs, the Multi-band Compressor 3 µs.
- **Known roll-off:** the antialiasing averages successive oversampled samples, so a clean
  band still loses 0.3 dB at 16 kHz (0.07 dB at 8 kHz).

## Parameters

| Key | Name | Range | Default |
| --- | --- | --- | --- |
| `crossover1`…`crossover3` | as the Multi-band Compressor's (group Crossovers, log) | 40…1000, 150…5000, 600…12000 Hz | 200, 800, 3000 Hz |
| `drive1`…`drive4` | Drive, per band (groups Low, Low Mid, High Mid, High) | 0…48 dB | 18, 28, 36, 30 dB |
| `character1`…`character4` | Character, per band (enum) | Tape, Tube, Diode, Hard, Fuzz | Tube |
| `level1`…`level4` | Level, per band | −24…+12 dB | 0 |
| `mix` | Mix | 0…1 | 1 |
| `output` | Output | −18…+18 dB | 0 |

Factory presets: Standard (defaults), Tight Crunch (low band near clean), Chord Clarity,
Fat Lows, Mid Scream, Fizz-free Lead, Parallel Grit (Mix 0.4), Splatter (Fuzz up top).

## Visualisation: the bands over the spectrum

The Compressor's panel, generalised (`multibandPlot.ts`, `signalPath/paramsPanel/multibandPanel.ts`):

- Input spectrum shaded, output spectrum as a line over it: the gap between them up top is
  the harmonics each band is adding.
- Four band columns divided at the crossovers in use; the lines drag (an ordinary param
  edit).
- **Drive into the clipper** hangs from the top of each column (0–24 dB scale) with its
  number, where the Compressor hangs its gain reduction: how far the band's peaks are being
  pushed past the knee since the last frame.
- **The meter strip under the plot**: each band's peak level against the level at which its
  clipper starts to bend, so it is plain which bands are clipping and how hard.

### Engine side

Nothing new in the executor: a node whose effect has band meters already gets an output tap
and its meters sent in `sldS` (`o`, `b`). Two small additions:

- `BandMeter` gains `thresholdDb`: the input level, dBFS, where the band's curve starts to
  act. The Compressor reports its threshold; the Distortion reports `nominal + 12 − Drive`,
  which the UI cannot know without the nominal level. It is the fifth element of each `b`
  entry, which older readers ignore.
- For the Distortion, `gainReductionDb` carries the drive into the clipper, and `levelDb` the
  band's peak (not its mean square: a clipper acts on peaks), held over 300 ms.

### Web UI

- `multibandCompressorPlot.ts` becomes `multibandPlot.ts`, drawing for either effect from a
  small description (labels, the hanging bar's sign and colours, what the strip's mark
  means); `multibandCompressorPanel.ts` becomes `multibandPanel.ts`, which picks the
  description from the node's effect.
- `eqSpectrum.ts` parses the optional fifth element as `EqSpectrumBand.thresholdDb`.
- The Distortion's hanging bar is red and labelled "Drive"; its strip mark comes from
  `thresholdDb` (or, before the first frame, the nominal −18 dBFS less Drive).
- Nano Q gets the knobs from the catalogue like any effect, as for the Compressor.

## Tests (`core/tests/MultibandDistortionEffectTests.cpp`)

- Clean bands reconstruct an all-pass: every Drive 0 and a quiet signal, flat within 0.1 dB
  40 Hz–8 kHz at default and moved crossovers, at 48 and 96 kHz, and under half a dB down at
  12 and 16 kHz (the antialiasing's roll-off); Mix 0 at full drive is flat too.
- Oversampling factor per rate (4×, 2×, 1×).
- Band isolation: a low tone stays clean with only the top band driven hard (3rd harmonic
  −133 dB); driving the low band instead adds harmonics (−10 dB).
- Level match: a nominal tone with only its own band at 36 dB comes out within 1.5 dB of Drive
  0, for every character.
- Aliasing: a 5 kHz tone, every band Hard at full drive, leaves its fold-backs at 3, 7 and
  13 kHz more than 45 dB under it (measured 65).
- Character switch mid-note does not step the output.
- Level and Output move by what they say.
- Band meters: crossovers in use, the clip threshold, drive into the clipper where a band is
  driven.
- Mono/stereo parity, NaN recovery (`FastMathNanTests`), factory presets
  (`EffectFactoryPresetTests`), an executor case in `SpectrumTapTests`.
- UI: `sldS` parsing of the fifth `b` element; the plot description per effect.
