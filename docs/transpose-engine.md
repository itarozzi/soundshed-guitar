# Transpose engine: SpliceTransposer

Record of the Low Latency transpose engine (`core/src/dsp/SpliceTransposer.h`): what it does,
why, and the measurements behind its constants. The global transpose always runs it, and the
`transpose` and `pitch_shift` effects offer it as Engine: Low Latency beside Signalsmith Stretch
(High Quality, the default for nodes, so presets saved before it sound as they did).

## Why another engine

Transpose ran Signalsmith Stretch at an 80 ms window (`SignalsmithSupport.h`). Below about 60 ms
that engine's pitch and purity fall away, so it cannot get near the 10-20 ms a player needs for
tight rhythm parts, and it smears bass: a low E1 at -5 st came out 34 cents off with its
inter-partial noise above its partials (+4.9 dB, table below). `transpose_stft` and
`transpose_hybrid` reach low latency but do not shift at all at -1 or -2 st
(`docs/plans/transpose-improvements.md`, defect 5).

A time-domain splicer avoids both problems: a delay line read at the pitch ratio, with each jump
placed where the waveform matches, the idea behind the Eventide H949's "de-glitch" and the
drop-tune pedals that followed it. Bass is no harder for it than guitar, and its latency is set
by the window it splices across, not by a frequency resolution. Our first one,
TimeDomainPitchShifter (built for Pitch Shift's pedal sweeps, and still the Auto Arpeggiator's),
searches a span too short for a low E1's period and knows nothing of attacks. SpliceTransposer is
the one built for transposing, and it has since replaced that one in Pitch Shift too.

## How it works

A delay line and one read tap moving at the pitch ratio `r = 2^(st/12)`, so the tap's delay
drifts: deeper while shifting down, shallower while shifting up, across a window (30 ms by
default; 15-60 ms) above a 2 ms floor. At the end of the window the tap jumps. Everything happens
per sample, so the output does not depend on the block size.

- **Where to land.** Each legal landing point is scored by `(1 - ncc) / run`: how badly the
  waveform there matches what the tap is playing (normalised cross-correlation over 25 ms, longer
  than a low E1's 24 ms period), per sample of run the jump buys before the next one. A long run
  with a good match beats a short perfect one, so sustained notes splice rarely. The search runs
  on a mono copy low-passed and decimated to 24 kHz (`WaveformHistory.h`), spread over the 3 ms
  before the jump is due rather than in one burst, then refined at the full rate within two
  decimation steps, and finally to a fraction of a sample by a parabola through the best match
  and its neighbours.
- **Fractional jumps.** A period is rarely a whole number of samples (220 Hz at 48 kHz is
  218.18), and two octaves down the splices come often enough that rounding the jump to a sample
  is heard: a 220 Hz tone at -24 st was 4.4 cents off with whole-sample jumps and is now within
  0.03. The tap's position is fractional anyway (4-point Hermite).
- **How to cross.** A raised-cosine crossfade whose gains are normalised by the measured
  correlation `rho`, `1 / sqrt(a² + b² + 2ab·rho)`: matched waveforms add as amplitudes, unrelated
  ones as powers, so the level holds either way (shifted noise stays within 0.4 dB over 50 ms
  blocks). The poorer the match, the longer the fade: 30 ms at ncc 0.95, up to 120 ms at 0.6, so
  a chord's mismatched partials drift across instead of stepping. A fade never outlasts the room
  the new tap has before its own next search.
- **Attacks.** `PickAttackDetector.h` watches the signal's power above 1.5 kHz and fires when it
  rises 9 dB over its recent quietest level and 4 dB over its recent loudest (the second test
  tells a pick from strings beating, and from the spiky high band of a low note), or 1.6x the
  loudest if the rise is sudden (15 dB within a millisecond or two: a quiet pick straight after a
  louder note that was damped). On a pick the tap jumps to 2-4 ms behind the input with a 2 ms
  fade, wherever it had drifted. An attack may cut into a crossfade that is still running: the fade in progress is frozen and faded out as one signal. Shifting up,
  the tap eats input faster than it arrives and must soon jump back, so an attack is landed deep
  enough that that jump can stay clear of the pick's first 6 ms (capped at 16 ms deep); the pick
  then comes slightly later rather than twice.
- **Upshift geometry.** A splice is clean only at a whole number of periods, so the range of
  jumps on offer has to hold the lowest period wanted and span an octave. Shifting up, the guard
  the tap jumps back from is fixed so the longest jump is 82% of the window (24.6 ms of 30 ms, a
  low E1's period); the longest fade is what that guard leaves the tap being faded out, and the
  shortest fade and the search lead shrink as the shift grows until the range spans its octave.
- **Stereo** shares one tap and one control path (the channels' mean); each channel has its own
  ring, so the image cannot smear.
- **Latency** reported is the tap's mean delay, `(floor + window) / 2`: 16 ms at the default.
  The felt latency is the attacks', 3-6 ms shifting down.

## The bench

A scratch harness (not in the repo) ran each engine over one corpus at each interval, in
64-sample blocks, and scored the output black-box against the dry signal:

- **Corpus.** Synthetic plucked strings from `core/tests/helpers/GuitarPhraseSynth.h`, so pitch,
  partials and pick times are known exactly: 12 guitar notes (E2-G4) and 5 bass notes (E1-D2)
  held for 3 s; chords (E power chord, open E, a major third E2+G#2) and bass dyads (E1+B1,
  E1+A1); three 12 s phrases with known picks (a guitar riff, a bass riff, strummed chords; about
  50 picks each); and the three guitar DIs in `core/ui/demo`.
- **Pitch** (cents): per-frame f0 by normalised autocorrelation, frames of 4.5 periods, against
  the dry's f0 times the ratio. Mean, and the 98th percentile of the worst note.
- **Sideband** (dB): power off the expected shifted partials against power on them (within 25
  cents or 3 bins), over a 1.4 s steady stretch.
- **Bumps**: the 25th-percentile spectral floor between 100 Hz and 1.5 kHz (4096-point frames
  every 2.5 ms) against its running median; excursions over 6 dB per second, and the 95th
  percentile. This is the splice "chuff" on held chords.
- **Attacks** (riffs): arrival of each pick in the output, by the first rise of the >1.5 kHz
  envelope over 30% of its peak and 4x its level before the pick, against the dry's own; the
  share of picks heard twice (a second pick-sized peak 5-80 ms later where the dry has none) or
  lost (under 25% of the expected level).
- **CPU**: microseconds per 64-sample block on `DI_Guitar_L.wav`, Release flags. (That is the
  demo DI guitar, a WAV when these were measured; it ships as `core/ui/demo/DI_Guitar_L.mp3`
  since 2026-10-04, and the benchmark decodes that.)

Calibration: a pure delay scores its own delay exactly on every pick, with no doubled or lost
ones. The doubled and lost counts are unreliable at +12 st, where every engine repeats the pick.

## Results

48 kHz, 30 ms window. `ss80` is Signalsmith Stretch as Transpose ran it, `tdps` the
TimeDomainPitchShifter (Pitch Shift's previous Low Latency engine), `new` SpliceTransposer. Lower
is better everywhere.

| st | engine | guitar pitch mean / p98 | guitar sideband | bass pitch mean / p98 | bass sideband | chord sideband | dyad sideband | attack median / p90 ms | heard twice | lost |
|---|---|---|---|---|---|---|---|---|---|---|
| -12 | ss80 | 3.98 / 21.1 | -38.4 | 16.9 / 61.2 | -25.4 | -10.1 | +4.4 | 79.8 / 80.1 | 19% | 3% |
| -12 | tdps | 0.85 / 5.2 | -40.6 | 41.9 / 166 | -25.5 | -5.8 | -2.4 | 16.4 / 23.5 | 27% | 23% |
| -12 | **new** | **0.35 / 2.2** | **-45.8** | **0.52 / 1.7** | **-42.5** | **-29.7** | **-16.0** | **6.2 / 7.9** | **6%** | **3%** |
| -5 | ss80 | 6.10 / 14.3 | -43.6 | 33.8 / 100 | +4.9 | -1.6 | +0.7 | 79.9 / 80.0 | 17% | 3% |
| -5 | tdps | 0.51 / 3.5 | -42.2 | 24.8 / 146 | -26.6 | -2.9 | +2.6 | 14.0 / 21.4 | 31% | 15% |
| -5 | **new** | **0.45 / 2.9** | **-47.9** | **0.75 / 2.9** | **-43.6** | **-25.9** | **-3.5** | **4.3 / 5.3** | **14%** | **3%** |
| -2 | ss80 | 4.97 / 13.8 | -45.9 | 33.1 / 53.1 | -0.9 | -4.0 | +0.4 | 79.9 / 80.0 | 11% | 3% |
| -2 | tdps | 0.51 / 3.3 | -43.8 | 14.8 / 197 | -27.8 | -5.1 | -5.6 | 14.4 / 19.5 | 11% | 6% |
| -2 | **new** | **0.49 / 3.2** | **-48.4** | **0.85 / 3.5** | **-45.3** | **-30.3** | **-11.7** | **3.9 / 7.2** | **8%** | **0%** |
| +4 | ss80 | 6.19 / 15.9 | -45.6 | 10.2 / 31.4 | -0.9 | -3.2 | +2.6 | 80.0 / 80.1 | 34% | 3% |
| +4 | tdps | 0.58 / 3.9 | -46.9 | 17.3 / 202 | -24.7 | -1.8 | +1.2 | 7.0 / 14.6 | 54% | 1% |
| +4 | **new** | **0.49 / 3.3** | **-49.4** | **0.93 / 3.7** | **-44.1** | **-25.6** | **-2.8** | 9.6 / 12.8 | 39% | **0%** |
| +12 | ss80 | 3.53 / 9.1 | -43.8 | 16.0 / 31.2 | -9.1 | -9.5 | +4.5 | 82.4 / 91.0 | 79% | 52% |
| +12 | tdps | 0.82 / 5.6 | -47.6 | 14.8 / 113 | -30.5 | -0.9 | +4.2 | 10.8 / 15.0 | 88% | 24% |
| +12 | **new** | **0.48 / 3.0** | -46.4 | **0.90 / 3.6** | **-42.6** | **-24.4** | **0.0** | **10.1 / 14.2** | 88% | 30% |

- Against Signalsmith Stretch, the engine Transpose ran: guitar pitch is 0.35-0.49 cents out
  against 3.5-6.2, bass under 1 cent against 10-34, held chords -24 to -30 dB against -1.6 to
  -10, and picks arrive 3.9-10 ms late against 80. Two things go the other way. Shifting up,
  picks are repeated a little more often (+4: 39% against 34%), since a splicer has to replay
  input where Stretch smears it. And at -12 the DIs' level ripples about 1 dB more, where
  Stretch's smearing evens it out.
- Against TimeDomainPitchShifter: single guitar notes are as clean or cleaner (its sidebands are
  1 dB lower at +12 only), bass is in tune (under 1 cent against 15-42), held chords are far
  cleaner (-24 to -30 dB against -1 to -6), and picks arrive 3.9-6.2 ms late shifting down
  against 14-16. It lands picks sooner shifting up (+4: 7.0 against 9.6 ms), at the cost of more
  repeats (54% against 39%), and it costs less CPU.
- SpliceTransposer's own figures are much the same at 44.1 and 96 kHz.

CPU on `DI_Guitar_L.wav` at 48 kHz, Release flags, microseconds per block (mean / 99th
percentile / worst), over -12, -5, -2, +4 and +12 st. A 64-sample block has 1333 us, a
16-sample one 333 us:

| engine | 64-sample blocks | 16-sample blocks |
|---|---|---|
| ss80 | 11.6-12.7 / 172-193 / 233-277 | 2.9-3.6 / 161-186 / 240-308 |
| tdps | 0.9-1.6 / 5.3-5.6 / 8-18 | 0.2-0.5 / 0.5-4.5 / 8-31 |
| **new** | 1.6-3.4 / 8-21 / 24-48 | 0.5-1.0 / 3.1-5.4 / 10-50 |

Pedal sweeps, for Pitch Shift: 0 to +12, down to -12 and back, the target moved every 64
samples with Snap off (a 4 ms glide), on a held A2, a held open E chord and `DI_Guitar_L.wav`:

| sweep | engine | A2 pitch along the sweep, mean / p95 cents | largest step, x the input's | quietest 10 ms on the chord (dry) |
|---|---|---|---|---|
| half an octave a second | tdps | 4.8 / 6.1 | 1.03 note, 1.77 DI | -30.3 dB (-27.0) |
| half an octave a second | **new** | **4.7 / 5.6** | 1.03 note, 1.85 DI | -30.0 dB |
| four octaves a second | tdps | 10.0 / 35.1 | 1.21 note, 1.94 DI | -10.5 dB (-9.3) |
| four octaves a second | **new** | **9.2 / 33.9** | 1.25 note, 1.96 DI | -12.7 dB |

Both engines take a new shift on the next sample (a step from -5 to +7 has half its energy at the
new pitch 3.0 ms later on either), and what pitch error remains is mostly the moving pitch
smearing the 40 ms measurement frames. The steps grow with the shift itself: at +12 the input
moves twice as fast. The one cost is on a chord in a very fast sweep, where the quietest moment
dips 2 dB deeper.

Known limits:

- At +12 st every pick is repeated: the shortest jump back after a pick needs more fresh input
  than there is before the tap catches up, unless the pick lands 25 ms deep. The 16 ms cap keeps
  the latency instead. The "lost" count there is the scorer latching on to the repeat.
- A 20 ms window cannot hold a low E1's period: bass at that window is tens of cents out.
- A dissonant bass dyad has no common period inside the window; its splices leave sidebands near
  the partials' level.

## Tests

`core/tests/SpliceTransposerTests.cpp` (engine and detector) and
`core/tests/TransposeEffectTests.cpp` (the effect, and the global transpose through
`GlobalChainEditor` and a real executor). `TransposeBenchmark` has a "Transpose (Low Latency,
splice)" variant, so the HyperTune and Archetype comparison in
`docs/plans/transpose-improvements.md` covers it.

## Global transpose

The global chain's transpose node used to be switched off at 0 st, so the executor stopped
calling it. Its next start resumed Stretch on whatever it had held since it was last on (the
stale-buffer defect fixed for the node's own 0 st path, but not this one), and both edges were a
hard switch between 0 and 80 ms of delay. Driven through the editor on HEAD, a shift restarted
after a stretch at 0 st played the old input as loud as the current (a 1 kHz tone 0.5 dB under
the 300 Hz one now playing; -53 dB with the fix), and each knob change stepped the output by 35
times a 220 Hz tone's own largest step. Now `GlobalChainEditor::LivePreChain` builds the
executor's copy with the node always running, on the Low Latency engine, at the configured shift
while the node is on and 0 while it is off. The node is transparent at 0 st, fades in and out of
that over 10 ms, and keeps recording its input, so a shift starts on the audio playing now. The
saved config still holds the node on only while it transposes, which is what both UIs show.
