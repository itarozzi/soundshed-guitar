# Splitter routing modes

Today the Splitter copies its input to every branch. This plan gives it a **Mode**, so each
branch can get only part of the signal: a frequency band, a range of notes, a dynamic range,
or a share set by a pedal. The branches, the mixer, the branch-count control and collapsing
a split all stay the same. The difference is what each branch receives.

Status, 2026-10-29: Copy, Blend, Switch, Dynamics, Pitch and Stereo (left/right only) are
built, uncommitted. docs/fx-library.md (Splitter) describes them as built; the tests are
`core/tests/SplitterRoutingTests.cpp`. Not built: Frequency (Phase 1), mixer branch alignment
(Phase 4), mid/side, branch sleep, and the per-branch activity marker. The routing line under
Branches depends only on the mode and branch count, not the knob values, so the Nano Q page
never rebuilds mid-drag. Answered: bypass copies; Unpitched defaults to All branches at equal
power; Switch is a mode of its own beside Blend. "Where things stand" below is the code as it
was before.

## Where things stand

- `EffectGuids::kSplitter` is registered in `BuiltinEffects.h` with a `PassthroughProcessor`
  and no parameters.
- The executor does nothing for a splitter (`SignalGraphExecutorPlan.cpp`, `isSplitter`).
  Each branch's first node gathers from the splitter's `bufferLeft/Right`, so every branch
  receives the same signal.
- `GraphEdge::fromPort` exists and is saved in presets, but only to order the branches
  (`SplitBranchSupport.h`). `PlannedEdge` doesn't carry it.
- A split has 2 to 4 branches (`kMaxSplitBranches` = `MixerEffect::kMaxInputs` = 4). The
  graph is the only record of the count.
- The mixer has per-input level, pan, delay and mute. There is no automatic latency
  alignment between branches (`GetTotalLatencySamples` takes the max path; nothing pads the
  shorter paths).
- Parts that already exist:
  - LR4 crossover cascade with all-pass compensation: `MultibandGateSupport.h`
  - `biquad::AllPass`
  - `PitchTracker`, plus `NoteTracker`, which turns its output into note-on/off events
  - `PickAttackDetector`
  - `SilenceSleepGate`
  - Per-mode parameter visibility (`EffectParamSpec::visibleKey` / `visibleFor`)

## Recommendation: a mode on the Splitter, not a new effect

Keep one node type. Every split feature looks for `kSplitter`:
- `ChainLayout`, `SignalFlow` and `NodeLabels`
- `SplitBranchSupport` (count, collapse, join-mixer search)
- the web UI's `splitBranches.ts`, `layout.ts` and `chainRow.ts`
- Nano Q's chain strip and dialogs
- the tone builder

A second splitter type would have to be taught to all of these, and kept in step with them.
A Mode parameter needs none of that, and players find it where they already make splits.

Compatibility:
- An old preset has no `mode` param, so it loads as Copy and sounds exactly as before.
- A new preset opened in an older release ignores the unknown params and copies to every
  branch. It plays, just louder and unfiltered. With a new GUID the older release would
  instead show an unknown node in the middle of the split, which is worse.

The Splitter gets a real processor, `SplitterEffect`, with the Mode enum and the parameters
for each mode, shown only when that mode is selected.

## Modes

Ranked by how useful and dependable each is against what it costs to build.

| Mode | Each branch gets | Latency | Reliability | Cost |
| --- | --- | --- | --- | --- |
| **Copy** (today) | everything | 0 | exact | free |
| **Frequency** | a band between crossovers | 0 | exact | 2-4 biquads/ch per crossover |
| **Blend** | a share set by Position (pedal or automation) | 0 | exact | gains only |
| **Dynamics** | soft playing to one branch, hard to the other | 0 (attack-led) | good | envelope follower |
| **Pitch** | notes below or above a split note | 20-55 ms before a note's pitch is known | fair; chords unpitched | ~2.4 µs/block tracker |
| **Stereo** | left / right, or mid / side | 0 | exact | needs layout-pass work |

### Frequency (crossover)

- Branch count N gives N−1 crossovers: `crossover1..3`, log taper, defaulting to 250 Hz,
  1.5 kHz and 5 kHz. Each crossover is held above the one below it.
- **Slope**: 24 dB/oct (LR4, the default) or 12 dB/oct (LR2, a gentler overlap for amp
  blends). Each lower band runs through the all-passes of the crossovers above it, so with
  all branches empty the bands add back to an all-pass: flat magnitude. The multiband gate
  already does this.
- Move the cascade out of `MultibandGateSupport.h` into `dsp/Crossover.h`, and keep the gate
  sample-identical (`tools/audio-ab` hash).
- Crossover changes glide. The mode recomputes coefficients in blocks with the existing
  parameter smoothing, so automating a crossover doesn't zip.
- Uses:
  - bass amp on the lows, guitar amp on the highs
  - a clean low end under a distorted top (tight high-gain without a muddy low string)
  - reverb or a stereo widener on the top band only
- **Flat recombination needs equal latency in every branch.** Built-in amps, cabs and drives
  add none, so this works with them. A hosted plugin or a latent pitch shifter in one band
  makes the bands comb-filter at the crossover. See *Branch alignment* below.

### Blend (A/B/Y with a position)

- **Position** 0..1 crossfades across the branches with equal-power gains. With 3-4
  branches it walks from branch to branch. A **Curve** setting picks equal power or linear.
- This is the expression-pedal mode: sweep from rig A to rig B. Automating or MIDI-mapping
  Position works like any other parameter (realtime path, no allocations).
- **Switch** quantises Position to whole branches with a short crossfade. This is an A/B
  box: tails ring out, because both rigs keep running, unlike a scene change.
- This mode is the cheapest of the lot. It also builds the per-branch gain machinery that
  Dynamics and Pitch both use, so it goes first among the gain-based modes.

### Dynamics (pick strength)

- An envelope follower on the splitter's input, with `PickAttackDetector` to start each note
  quickly. Above **Threshold** (dB) the signal goes to branch 2, below it to branch 1.
- **Width** (dB) sets how gradual the crossover between the two is, from a blend to a hard
  switch. **Hold** (ms) stops a decaying hard note from falling back to the clean rig mid-note.
- Use: picking lightly gives a clean amp and digging in gives the dirty one, beyond what
  turning one amp's gain up and down can do.
- More dependable than Pitch. The level is known right at the attack, so there is no 20-55
  ms wait. A good candidate to ship alongside Frequency.

### Pitch (note range)

The mode you asked for. It's worth building, but these limits need to go in its description:

- **Use `NoteTracker`, not raw pitch.** Decide once per note, at note-on, and keep that
  branch until note-off. A bend or vibrato across the split point then never flips rigs
  mid-note. Hysteresis only matters for legato runs that cross the split.
- **The attack comes before the pitch.** On real hard-picked low notes, pitch shows up a
  median 55 ms after the pick (86 ms worst) on the demo riffs (see the guitar-to-MIDI
  review). Until then nobody knows which branch the note belongs to. **Undecided** sets what
  happens during that time:
  - *Both* (default): the attack goes to every branch, then the losing branches fade out
    over **Crossfade** (5-30 ms). The pick is heard on both rigs for up to ~50 ms, which
    sounds natural.
  - *Last*: stay on the previous note's branch. Right for runs that stay on one side of the
    split, and wrong on the first note after crossing it.
  - *Branch N*: a fixed default.
  - Not offered: lookahead. Delaying the dry signal by ~50 ms so the decision is ready in
    time is unplayable live. It could be added later as a studio-only option that reports
    its latency.
- **Chords don't have a pitch** to a mono tracker: power chords flip between root, fifth and
  octave, and full chords are unpitched. Chords fall under **Undecided** too. The honest
  advice is that "bass rig under the low strings" works better as a Frequency split, and true
  per-string routing needs a hexaphonic pickup, which this design doesn't cover.
- Parameters:
  - **Split note** (note names, E1-E6; one per crossover, so 1-3)
  - **Lowest note** (the tracker's existing E2/D2/B1/F#1 choice)
  - **Undecided**
  - **Crossfade**
- Track the splitter's input. A split placed after a drive tracks worse, so the panel should
  say "place before drive/amp".
- Test on the demo riffs, not only on synthesised lines (see *test on real recordings*):
  score each note's branch against hand labels.

### Stereo (left/right or mid/side)

- Branch 1 gets left (or mid), branch 2 gets right (or side). Uses:
  - with stereo input (the dual-mono work), two sources into two rigs
  - with M/S, processing the stereo spread separately
- It needs the most executor work. The layout pass gives a node one input layout, and here
  each port is mono while the splitter's input is stereo. It goes last.

### Considered and set aside

- **Chord vs single note** (chords to a rhythm rig, single lines to a lead rig) uses YIN's
  periodicity (dip depth) as the chord detector. It's attractive, but a mono tracker calls
  bends, vibrato and fast runs "aperiodic" too often. Revisit once Pitch has shipped and its
  numbers on real playing are known.
- **LFO / tempo-synced rig switching** is cheap, but it's Blend with automation. A Blend LFO
  could come later if people want it.
- **Harmonic / percussive separation** (spectral) adds an FFT's worth of latency and real
  CPU. Not for a live splitter.
- **Routing between Multi-Rig rigs** instead of split branches: the same router could sit in
  `MultiPresetMixer`. Branches come first because they live inside one preset, which
  shares, undoes and saves like any other preset. The router class should not depend on the
  executor, so Multi-Rig can reuse it.

## Engine changes

1. **Per-port splitter output.**
   - `PlannedEdge` gains `fromPort`.
   - A routing splitter's `NodeState` gets port buffers for up to 4 stereo pairs, allocated
     in Prepare and never on the audio thread.
   - Gather reads `source->port[fromPort]` when the source is a routing splitter, and
     `bufferLeft/Right` as now otherwise.
   - Copy mode writes no port buffers, so today's splits cost exactly what they cost now.
2. **`SplitterEffect`** replaces `PassthroughProcessor` for `kSplitter`. Its interface is
   `ProcessSplit(in[2], out[port][2], ports, numSamples)`. The mode is a strategy inside:
   - filter-based (Frequency)
   - gain-based (Blend, Dynamics, Pitch): one smoothed gain per port, applied as the port
     buffers are written
   - channel-based (Stereo)
3. **Port count follows the graph.** The executor tells the effect how many branches the split
   has when it builds the plan. It is not stored in a parameter, so the "graph is the only
   record of the count" rule holds. Crossover/split-note params beyond the count are hidden.
4. **Bypass = Copy.** A bypassed routing splitter sends everything to every branch: the split
   as it was before routing existed. Open question below.
5. **Branch alignment** (separate change, also helps Copy). The mixer pads each input by its
   branch's latency shortfall, worked out from `GetLatencySamples()` as the plan is built and
   added to the user's `delay_N`. This is what keeps a Frequency split flat with a latent
   plugin in one band, and it removes comb filtering from parallel splits generally.
6. **Branch sleep** (optional, later). A branch whose gain has been 0 and whose output has
   fallen silent (`SilenceSleepGate` logic) could skip processing. Until then a branch you're
   not routing to still uses CPU, the same as any split today.

## UI

- The Splitter panel gets Mode plus the selected mode's parameters, through the generic
  parameter renderer in both web and Nano Q. The branch-count control stays where it is.
- Each branch row is labelled with what it receives ("< 250 Hz", "250 Hz - 1.5 kHz",
  "below D3", "soft", "hard") so the routing can be read from the chain.
- Live activity: in gain-based modes, a dim/lit marker per branch from a telemetry value the
  splitter publishes, so the player can see which rig a note went to. Paint-only (see
  telemetry visuals).

## Phases

| Phase | Work | Done when |
| --- | --- | --- |
| 0 | Per-port gather, `SplitterEffect` with Mode = Copy only, `fromPort` in `PlannedEdge` | every existing preset is sample-identical at 44.1/48/96 kHz (`audio-ab`); executor and split tests pass |
| 1 | `dsp/Crossover.h` from the gate (gate unchanged), **Frequency** mode, branch labels | empty branches sum within 0.01 dB of flat at 2/3/4 branches; crossover automation glides with no zipper noise |
| 2 | **Blend** and **Dynamics** on shared per-port gains | crossfades click-free under automation; no audio-thread allocations |
| 3 | **Pitch** on `NoteTracker`, Undecided rules, activity marker | branch choice scored on demo riffs 01-03; bends across the split never switch mid-note |
| 4 | Mixer branch alignment | a latent plugin in one band keeps the Frequency sum flat |
| 5 | **Stereo** (L/R, M/S) with per-port layout | stereo-input tests; mono input falls back to Copy |

## Open questions

1. **Bypass.** Copy to all branches (proposed), or everything into branch 1 only? Copy keeps
   "bypassed = what a split did before"; branch 1 keeps the volume from jumping when a
   Frequency split is bypassed.
2. **Pitch default.** Is *Both* the right default for Undecided, or should a two-branch pitch
   split default to *Last*?
3. **Blend in Phase 2.** It's the cheapest mode and the one expression-pedal users will look
   for, but it wasn't on the original list.
