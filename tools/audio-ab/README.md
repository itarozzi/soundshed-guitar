# Audio A/B between app versions

`audio-ab.mjs` answers one question: **does anything sound different between two versions
of the app?** It renders every effect at its defaults, and a set of whole signal chains
through the app at its default settings, for both versions. Then it compares the renders and
says which effects and chains changed, how (level, tone, waveform, tail, latency) and whether
a player would hear it.

Its `perf` command answers a second one: **how does DSP CPU compare between versions?** See
[Timing DSP between versions](#timing-dsp-between-versions-perf).

```powershell
node tools/audio-ab/audio-ab.mjs 1.5.0              # the public release vs your working tree
node tools/audio-ab/audio-ab.mjs HEAD               # what your uncommitted changes do to the sound
node tools/audio-ab/audio-ab.mjs 1.5.0 main         # any two revisions
node tools/audio-ab/audio-ab.mjs 1.5.0 --rate 44100 --signals di,di_hot
node tools/audio-ab/audio-ab.mjs perf 1.5.0 1.6.0 HEAD   # µs per block, against 1.5.0
node tools/audio-ab/audio-ab.mjs --help
```

A revision is anything `git rev-parse` accepts, or `WORKTREE` (the default target) for this
checkout as it stands, uncommitted changes included. The console summary lists everything
that changed. `report.html` holds the detail, with base, target and difference players for
each changed case, and `report.md` and `compare.json` sit beside it.

## How it works

For each revision the driver:

1. **Checks it out** as a detached git worktree under the work directory. `WORKTREE` uses
   this checkout directly.
2. **Copies in this checkout's harness**: `core/tests/AudioSnapshot.cpp`, `AudioPerf.cpp`,
   `AudioSnapshotChains.h`, `AudioSnapshotSupport.h` and `AudioSnapshot.cmake`. If that revision's
   `core/tests/CMakeLists.txt` does not include `AudioSnapshot.cmake`, the driver adds the
   include. Both sides therefore run the same harness, so a difference can only come from
   the DSP.
3. **Builds `AudioSnapshot` in Release**, which is what ships and what `/fp:fast` and
   `-ffast-math` apply to. The build tree is the revision's own and lives under the work
   directory. A revision that pins the same dependencies as this checkout reuses
   `core/build/_deps`; otherwise it fetches its own, as the NAM core, Eigen and the resampler
   have moved between releases. `npm` is replaced by a no-op because the harness doesn't
   need the UI bundle.
4. **Renders** with the stimuli and test resources from *this* checkout, so both sides get
   identical input.

Renders of a committed revision are cached. The cache key covers the commit, the harness,
`snapshot.json` (bar its `perfCases`), the inputs and the options, so comparing against the same base again only
re-renders the working tree. The first run for a revision is the slow one: 1.5.0 took about
six minutes (two to configure and fetch, three to build, 40 s to render). After that, builds
are incremental and a full render takes about 30 s.

### What is rendered

**Effects, each alone at its defaults.** Every registered effect type gets its own
`SignalGraphExecutor`. Its parameters are the ones a node the user has just added gets: every
registered default, then the effect's nominated factory preset, else its first factory preset.
This mirrors `PluginControllerSignalPath.cpp`. The NAM types also get the interface calibration
the controller injects at default settings: 12 dBu, enabled. Effects that need a file get the
test resources named in `snapshot.json` under `effectResources`.

**Whole chains through the app.** Each chain in `snapshot.json` is built as a preset and sent
to a real `PluginController` with a `loadPreset` message. The controller runs standalone with
a fresh, empty profile, which makes every setting the app's default: mono input on channel 1,
limiter off, NAM calibration on at 12 dBu, the global chain present but switched off. The
render goes through the controller's own audio callback, so it covers the input stage, the
mixer, the global chain and the master too. Each render is primed with a second of silence,
so the preset's fade-in has finished before the stimulus starts.

Renders run with denormals flushed to zero, as the app's audio callback does
(`juce::ScopedNoDenormals`). Every case gets a fresh engine. The first stimulus is rendered
twice; if the two renders differ, the case is marked not deterministic and is judged on
level, tone and tail only.

| Stimulus  | What it is                                                                  |
| --------- | --------------------------------------------------------------------------- |
| `di`      | 6 s of the demo DI guitar (`core/ui/demo/DI_Guitar_L.mp3`, from 12 s, faded in and out) + 2 s tail |
| `di_hot`  | the same, 12 dB hotter, to push drives and amps                              |
| `sweep`   | 4 s exponential sine sweep, 20 Hz to 20 kHz at −12 dBFS, + 1 s tail          |
| `impulse` | a −6 dBFS impulse at 10 ms, + 1.5 s tail                                     |
| `silence` | 1.5 s of digital silence: self-noise, hum, DC                                |
| `noise`   | 3 s of white noise at −20 dBFS rms, + 1 s tail                               |

The default set is `di, sweep, impulse, silence`, taken from `snapshot.json`.

### Reading the verdicts

| Verdict     | Meaning                                                                    |
| ----------- | -------------------------------------------------------------------------- |
| `identical` | bit-identical                                                              |
| `rounding`  | plain null below −120 dB: float noise                                      |
| `inaudible` | measurably different, but below every threshold below                      |
| `slight`    | may be audible side by side                                                |
| `audible`   | a player would hear it                                                     |
| `broken`    | non-finite samples, output fell silent, or a render is missing             |
| `new` / `removed` | the effect or chain exists on one side only                          |

The measures, taken per channel, with the worst channel reported (`analysis.mjs`):

- **Level**: rms change over the render. Slight at 0.1 dB, audible at 0.5 dB.
- **Tone**: the biggest change in any third-octave band within 40 dB of the loudest band,
  beyond the overall level change, so a pure gain change counts as level only. Slight at
  0.3 dB, audible at 1 dB.
- **Character**: the gain-matched null, which is what is left after aligning the target to
  the base and matching its gain. Slight above −40 dB, audible above −20 dB. A bad null with
  unchanged level and tone means the waveform itself changed: a new nonlinearity, a
  different modulation, a timing change.
- **Tail**: rms after the stimulus ends. For `silence`, that is the self-noise. Slight at
  0.5 dB, audible at 1.5 dB. A tail that appears out of digital silence is always flagged:
  audible above −70 dBFS, slight below it.
- **Latency**: the lag that best aligns the two renders, noted only when the aligned
  waveforms match (a gain-matched null below −20 dB), alongside any change in an effect's
  reported latency. This is a note, never a verdict, but a pure latency change is at least
  `inaudible`, never `rounding`.

Nothing quieter than −100 dBFS raises a flag, and a change that is itself quieter than
−70 dBFS (a residual, a band, a tail) is at most `slight`. Override any threshold with
`--threshold name=value`; the names are in `DEFAULT_THRESHOLDS` in `analysis.mjs`.

The report also lists **definition changes** between the two registries: renamed or retired
effects, and changes to the parameters a new node gets, their ranges, units, steps, choices
and tapers. A retired type is compared against the type that now answers to its alias; for
example, `amp_nam` is compared against `amp_nam_optimized`.

Exit status is 1 when a subject is `broken`, or at least as bad as `--fail-on`
(`broken`, the default, or `audible`, `slight`, `never`). Status 2 means the tool itself
failed.

## Adding a chain

Add an entry to `chains` in `snapshot.json`:

```json
{
  "id": "my-rig",
  "name": "My rig",
  "description": "Shown in the report.",
  "globalChain": { "gate": true, "eq": true },
  "nodes": [
    { "type": "overdrive" },
    { "type": "amp_nam_optimized", "resource": "nam-drive" },
    { "type": "cab_ir", "resource": "ir", "params": { "mix": 0.8 } },
    { "type": "reverb_room", "enabled": false }
  ]
}
```

Name node types by alias (`core/protocol/effect-aliases.json`) so that one definition serves
every revision. If a revision lacks a type, that chain is reported as `new` or `removed` for
it. A node gets fresh-node parameters, then its own `params` on top.
`"freshParams": false` gives it only its own `params`, which is how the UI's New Preset
template sends them. Nodes run in series unless the chain gives `edges`; see `wet-dry`, which
uses a splitter and a mixer node with `toPort` for each mixer input. Resources are named in
`resources`, with paths relative to `core/tests/testdata/assets`.

Adding or changing a chain invalidates the render cache for every revision.

## Changing the analysis

The verdict rules live in `analysis.mjs` and are pinned by synthetic cases in
`analysis.test.mjs`: `node --test tools/audio-ab/analysis.test.mjs`. To re-judge renders you
already have without building or rendering anything, run
`node tools/audio-ab/audio-ab.mjs report <renderDirA> <renderDirB>`.

## Timing DSP between versions (`perf`)

```powershell
node tools/audio-ab/audio-ab.mjs perf 1.5.0 1.6.0 HEAD      # ratios against the first revision
node tools/audio-ab/audio-ab.mjs perf HEAD WORKTREE         # what your uncommitted changes cost
node tools/audio-ab/audio-ab.mjs perf 1.6.0 HEAD --rate 44100 --stereo --filter nam
```

`perf` checks out, copies the harness into and builds each revision as above, but its target
is `core/tests/AudioPerf`. Then it runs the revisions' builds one after another, for
`--rounds` rounds (default 4). Each round starts with the next revision along, so a machine
that warms up or gets busy part-way through affects every revision alike. Each case keeps its
best round, because interference only ever adds time. The console prints the table, and
`perf.md` and `perf.json` are written to `reports/perf-…`.

On a Ryzen 7 8845HS laptop on 2026-10-17, with the builds already done, this took 20 seconds.
The effects run faster than real time, so 10 s of audio takes well under a second to time:

```text
DSP CPU  µs per 64-sample block at 48000 Hz, mono input
  the mean of each case's best of 4 rounds of 10 s; the deadline is 1333 µs; ratios are against 1.5.0, below 1 is faster

                                       1.5.0          1.6.0           HEAD
  NAM, standard model (JCM800)          65.9   54.8 (0.83×)   20.8 (0.32×)
  NAM, A2 model (MESA clean)            53.3   51.4 (0.96×)   17.8 (0.33×)
  IR Cab (421 1960)                     13.2   11.4 (0.87×)   4.88 (0.37×)
  Heavy American at defaults            9.26   32.6 (3.52×)   5.37 (0.58×)
  Heavy American, 4 stages, gain 0.8    15.5   43.7 (2.82×)   6.41 (0.41×)
```

A revision's first `perf` build compiles only `AudioPerf.cpp` if its tree already has the
core built, from `render` or an earlier `perf` run. A fresh revision takes as long as its
first render does.

### What is timed

Each case in `perfCases` in `snapshot.json` is built as a chain is: node types by alias,
fresh-node parameters, `params` on top, test resources, and `edges` if given. It then runs on
its own `SignalGraphExecutor`, the way the effects pass runs one effect. A NAM node gets the
controller's 12 dBu interface calibration, unless the case sets `calibrationInputLevel`. The
input is the six seconds of demo DI that the `di` stimulus uses, looped. After a second of
warm-up, `--seconds` of audio (default 10) is timed, one `Process` call at a time.

- **Mono input by default**, as the app's default input is. On 1.6.0 and later, the executor
  gets the Mono input layout, as the mixer gives it. 1.5.0 has no layout and runs mono when it
  sees L == R. Without the layout, 1.6.0 and later run NAM and the amp in stereo at twice the
  cost, which the app never does for a mono input. `--stereo` feeds another part of the DI to
  the right channel and sets the Stereo layout. On 1.5.0, the different right channel is what
  makes it run stereo.
- The timing thread is pinned to one logical core (`--core`, default 4; `-1` for none) at
  high priority, with denormals flushed as in the app's audio callback. Work that an effect
  hands to helper threads counts only as the time `Process` spends waiting for it.
- `--rate` and `--block` work as they do for renders. At a rate other than the model's own,
  a NAM figure includes whatever resampling that revision does. 1.6.0 and later resample, and
  report the resampler's latency.

### Reading the table

Each figure is the mean µs per block from the case's best round. The ratio is against the
first revision named, and below 1 is faster. The block deadline (1333 µs at 48 kHz/64) is
printed alongside. `perf.md` adds the median, p99, max, share of the deadline, latency, output
rms and round spread for every case on every revision.

- **Output rms** checks that the same work is timed. Equal rms across revisions means the
  effect does the same thing. A different rms means the effect itself changed, as the Heavy
  American did between 1.5.0 and 1.6.0: a new amp, 4x oversampled and about 5x quieter.
- `passthrough!` means the output equalled the input on every timed block: nothing was
  processed, perhaps because a model failed to load. `silent!` means the output was silent.
- **Round spread** is how far the second-best round ran above the best one. When it is small,
  the best round is not a fluke. One slow round among close ones does not matter, because the
  best round discards it. Above 5% is reported as noisy: run again with the machine idle and
  plugged in, or with more `--rounds`.
- `error` means the case could not be built on that revision, for example because the effect
  type is not in it.

Figures are only comparable within one run on one machine. Compare revisions in the same
`perf` call, not against an old report. The summary rules live in `perf.mjs` and are pinned
by `perf.test.mjs`: `node --test tools/audio-ab/perf.test.mjs`.

### Adding a case

Add an entry to `perfCases`, written as a chain is:

```json
{
  "id": "amp-cab",
  "name": "NAM into the IR Cab",
  "nodes": [
    { "type": "amp_nam_optimized", "resource": "nam-drive" },
    { "type": "cab_ir", "resource": "ir" }
  ]
}
```

`--filter` picks cases whose id or name contains the text. Changing `perfCases` does not
invalidate the render cache. To time one build directly, without the driver:

```powershell
& <work-dir>\build\<sha>\tests\Release\AudioPerf.exe --definition tools\audio-ab\snapshot.json `
    --assets core\tests\testdata\assets --di core\ui\demo\DI_Guitar_L.mp3 --rounds 3 --filter nam
```

## Keeping the harness buildable against old revisions

The harness is compiled against every revision it compares, so it can only use APIs those
revisions have. Everything it calls has been stable since 1.5.0: `SignalGraphExecutor`,
`EffectRegistry`, `PluginController`'s `Initialize`/`Prepare`/`HandleUIMessage`/`ProcessAudio`,
`PresetStorage::SerializeToJson` and `IPluginHost`. Where a newer field or method is useful,
reach it through a `requires` check in a template, as `FreshNodeParams` does for
`EffectPresetDefinition::isDefault`. The check compiles away against a revision that lacks
it. If a base revision fails to build, the driver says so and shows the compiler errors.

## Files and disk

Everything lives in the work directory: `../<repo>-audio-ab` by default, or `--work-dir`, or
`SSG_AUDIO_AB_DIR`.

| Directory     | Holds                                                               |
| ------------- | ------------------------------------------------------------------- |
| `src/<sha>`   | one git worktree per revision                                       |
| `build/<sha>` | its Release build tree (`build/worktree` for this checkout), 0.7–1.2 GB each |
| `renders/`    | float WAVs, `manifest.json`, `effects.json`: about 190 MB per revision at 48 kHz |
| `reports/`    | `report.html`, `report.md`, `compare.json`, and the base, target and difference audio of every changed case, so a report plays what it measured even after the renders are replaced (1.5.0 against today: about 200 MB). `perf-…` folders hold `perf.md` and `perf.json` |
| `logs/`       | configure, build, render and perf logs; `<sha>-perf.log` holds AudioPerf's raw output for every round |

`node tools/audio-ab/audio-ab.mjs list` shows what is cached. `clean` removes the worktrees
and builds, and `clean --all` removes the renders and reports as well.

## What it does not cover

- Plugin mode. Chains run standalone. A DAW host sets its own input configuration.
- The WASM host and hosted third-party plugins. There is no test module or plugin, so the
  WASM host renders as a passthrough.
- Composites and blends from the user's library, and factory presets that need resources
  outside `core/tests/testdata/assets`.
- UI-side defaults beyond what `snapshot.json` spells out. The New Preset chain copies
  the engine's `BuildNewPreset` (`controller/PluginControllerPresetEdits.cpp`) by hand, so
  keep it in step.
- Controller policy in the effects pass. That pass mirrors the controller's defaults (the
  12 dBu NAM calibration) rather than running the controller. The chains pass runs the real
  controller, so a policy change shows up there.
- For `perf`: the controller's own cost (input stage, mixer, global chain, master), and other
  compilers. It times what this machine builds: MSVC on Windows.
