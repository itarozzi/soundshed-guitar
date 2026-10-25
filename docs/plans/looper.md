# Looper

Status: **plan, revision 3** (2026-10-22). **Phases 1, 2, 3 and 6 built** (2026-10-25):
see [Implementation status](#implementation-status). The decisions marked **(decide)** were taken as
recommended.

## Implementation status

**Built (2026-10-25):**
- **Phase 1, the engine.** `core/src/dsp/looper/LooperEngine.h` (`LooperEngine.cpp` for the block,
  the taps and playback; `LooperEngineActions.cpp` for presses, layers, undo and transport) and
  `core/src/dsp/TransportClock.h`. In `MultiPresetMixer`: the DI tap after the input fold, before
  calibration; `FinishOutputBlock`, the one end of the block's three exits; the sleep gate held
  awake while the looper needs the chains. `LooperEngineTests` and `MixerLooperTests`.
- **Phase 6, riff capture on the looper's recorder.** `core/src/controller/RiffRecorder.{h,cpp}` is a
  second `LooperEngine` with playback off, fed the raw input ahead of the demo clip.
  `RiffCaptureRuntime`, `ProcessRiffCaptureBlock` and `RiffCaptureNotices.h` are gone; a finished
  take is a `RiffTake` (`controller/RiffTake.h`). The riff messages, the library index and the
  take files are unchanged, so existing riff libraries load as they are: there is nothing to
  migrate until Phase 4 adds `loops/` and the `looper-projects` document beside them.
  `RiffRecorderTests` checks takes sample for sample.

- **Phase 2, the service and control.** `core/src/controller/LooperService.{h,cpp}` and
  `LooperServiceMessages.cpp`: buffers (shared, freed once no snapshot refers to them and every
  offer has surely been applied, `LooperEngine::BlocksProcessed`), the spare, the memory budget
  (redo dropped first), the looper.* settings, `looperState`/`looperLayers`/`looperWaveforms`, and
  `looperCommand`/`setLooper`/`setLooperLayer`/`removeLooperLayer`/`getLooperState`. The metronome
  publishes `TransportClock`, restarts at a sample offset (`RequestReset(offset)`), locks its tempo
  under a Bars loop, and mutes the click through a take (`SetClickSuppressed`); its click moved to
  `MetronomeServiceRender.cpp`. `IPluginHost::GetDeviceLatencySamples()` (the standalone reports
  input + output). Registry triggers `looper.recPlayDub/stop/undo/redo/clear` and `looper.level`.
  `LooperServiceTests`.

- **Phase 3, the UI.** `core/ui/ts/looper.ts` and `core/ui/ts/looper/` (state, strip, layers,
  waveform, refresh), `messages/looperHandlers.ts`, the senders in `bridge.ts`, `css/looper.css`, the
  Looper feature flag and the footer's Loop button (docs/user-interface.md, "Looper strip").
  `tests/looperStrip.test.ts`. Checked in the running app: a free loop, an overdub held across
  three wraps (three layers), undo, and the flag switching the engine off and freeing its memory.
  **Left out:** the performance-pads looper row (`performancePads.ts` is at its size pin; the strip
  stays over the Play panel, so the transport is never a menu away).

- **Phase 4, projects and the riff library.** `controller/LooperServiceProjects.cpp`,
  `LooperServiceExport.cpp`, `LooperProjectFiles.{h,cpp}` and `LooperWorker.h`; `util::Encode32BitFloatWav`.
  The engine gained `ReplaceAudio` (a layer's audio swapped, with an optional 8 ms crossfade, only
  for the layer whose serial it names) and `LoadLoop` (a whole loop in one command). A first layer
  is copied out of the capture buffer when it finishes. Saved loops are listed in the strip's
  Loops menu (open, delete, recover), not in the riff library panel. `LooperProjectTests`.

**Not built yet:** Phase 5 (re-tone, and adding a riff as a layer, which needs its render).

**Phase 2 limits:** a Bars take started from MIDI with the metronome off counts in silently (the
guidance click is switched on the message thread, for a UI press). The looper level a footswitch
sets is not saved.

**Where the build differs from the text below:**
- **Riff capture is a second engine, not a session of the mixer's.** Its DI is the raw input, and
  a riff take must never touch a loop, nor a loop a riff take.
- **Two engine settings beyond the table:** `countInEveryTake` (riff capture counts in before free
  and armed takes; an armed take listens from the downbeat) and `playbackEnabled` (off for riffs: the
  take closes Stopped, with no ring-out and no seam).
- **Riff takes changed in two ways a player can notice:** an armed take starts 10 ms before the
  note, and the arm threshold rises above a hum measured as it waits (fixed -60 dBFS before).
  `barAlignOffsetSamples` counts to the take's first sample, pre-roll included.
- **Undo is a history of actions.** Recording a layer and removing one are both steps: Undo takes
  back the last of them, so after a remove it brings the layer back, and otherwise it takes the
  newest layer away. Redo replays. A new layer clears redo.
- **The spare may be longer than the loop.** Until the length is known the service keeps a
  capture-sized spare, so an overdub can open the moment a free first layer closes.
- **No separate `SeamCrossfade.h`.** The first layer's seam is smoothed by the ring-out (which now
  always ends on a 5 ms fade, so playing on across the close leaves no step) and by fading the DI
  played just after the close onto the start. The 8 ms swap crossfade belongs to Phase 5.
- **Smaller choices:** a layer shorter than 50 ms is dropped; an overdub pressed within 50 ms of
  the record wrap opens on it; while the arm floor is being measured a peak above -24 dBFS starts
  the take at once; Stop during a Bars first layer cancels it (it cannot close at the wrong length).

History:
- Rev 1 (2026-10-13): one summed loop with a feedback control.
- Rev 2 (2026-10-13): added Armed and Bars recording.
- Rev 3 (2026-10-22): the looper is rebuilt on the riff library and recorder. A loop is a
  stack of separate **layers**, each holding the DI and the effected sound. It is saved
  as a **looper project**.

## What it is

A live phrase looper.

- **Record and layer.** You record a phrase, it plays back in a loop, and you add layers
  on top.
- **Each layer is its own track.** Every layer can be levelled, panned, muted, soloed,
  removed or exported. You can change the preset it was played through later.
- **Undo** removes the newest layer.
- **Control.** The looper is driven from the UI, the keyboard, or a MIDI footswitch.
- **Tone changes.** Switching presets, scenes or setlist entries changes the tone of
  what you play *now*. The layers already recorded keep the tone they were recorded
  with.
- **Saving.** A loop is saved as a **looper project** in the riff library. Any layer can
  be exported as a riff, and any riff can be brought into a loop as a layer.

A take can start on a press, or **Armed**: the looper waits and starts recording when
you start playing. A take can end on a press, or in **Bars** mode it runs for a set
number of bars at the metronome's tempo and time signature.

## The design in brief

- **Two recordings per layer, made together.** For each layer the looper records two
  things at once:
  - the **DI**: what came in from the guitar;
  - the **wet** sound: what the chains made of it, as heard.
- **Playback is cheap.** Playback is a sum of the wet recordings, with a gain and a
  balance each. No layer runs a chain, so ten layers cost what one does: a stereo
  multiply-add per layer per sample.
- **Changing a layer's tone is offline.** It is a re-amp: the layer's DI is rendered
  through the chosen preset on a detached graph, off the audio thread. This is the same
  machinery `PresetLevelMeter` uses. The new wet recording replaces the old one when it
  is ready, and the live audio never pays for it.
- **The looper is a mixer stage, not an effect node.** `LooperEngine` sits in
  `MultiPresetMixer` between the global post-chain and the master gain:
  - It taps the DI after the input is folded to mono, and the wet sound before it adds
    its own playback.
  - A preset swap, a scene change or a chain rebuild never touches a layer.
  - Master gain, output mute, the limiter and the output meters cover the loop.
  - The metronome click and the practice-tool backing track are never recorded.
- **The riff library is its home.**
  - Looper projects are saved in the riff library's folder, and listed next to the
    riffs.
  - Riff capture is re-implemented on the looper's recorder: a riff take is a one-layer
    recording with loop playback switched off.
  - The riff library's own format does not change.

## Riff capture: extend it or re-implement it

Riff capture today (`PluginControllerRiffs.cpp`, `PluginController.h:1259-1295`):

- **One take**, recorded from the **raw** host input: before the demo clip is mixed in,
  and before the input is folded or calibrated.
- **Storage:** stereo float vectors, swapped in under `mDSPMutex` and written by
  `ProcessRiffCaptureBlock` on the audio thread.
- **No playback** while it records.
- **Saved** as 16-bit stereo WAVs under `<library>/takes/<riffId>/`, indexed in the
  `riff-library` storage document.
- **Notices.** It has kept JSON off the audio thread since 014cf31b: notices are raised
  there and sent by `PublishRiffCaptureNotices`.

The looper needs things that recorder was never shaped for:

- a DI tap and a wet tap recorded together;
- many layers;
- playback running while the next layer records;
- buffers handed to the audio thread and taken back without it ever releasing memory.

Extending `RiffCaptureRuntime` that far is a rewrite anyway. So:

- **The recorder is re-implemented.** `LooperEngine` is the one recorder. Riff capture
  becomes a looper session with one layer and playback off. Its modal, its count-in and
  click guidance, its arm and its trim stay as they are to the user.
  `RiffCaptureRuntime`, `ProcessRiffCaptureBlock` and their state leave
  `PluginController`.
- **The riff library is extended.** It gains looper projects. It also gains two ways
  between the two: "export layer as riff" and "add riff as layer". Existing riffs and
  takes load unchanged.
- **One difference to keep in mind.** Riff capture records the raw input before the demo
  clip. The looper's DI is taken after the clip, so the demo can be looped and tested
  with no guitar. When riff capture moves onto the recorder, it keeps its own
  behaviour: the demo clip is not recorded into a riff.

## Layers

### What a layer holds

| Field | What it is |
| ----- | ---------- |
| DI | One channel in mono input mode (the selected input), two in stereo input mode. Float. Before input calibration (see below) |
| Wet | Stereo float. The global post-chain output, moved back by the chain latency so it lines up with the DI |
| Length | The loop length. Every layer of a loop has the same length |
| Recorded span | `[start, end)` within the loop. A punched-in layer is zero outside it, with 5 ms ramps at the edges |
| Calibration gain | The input calibration gain in force while it was recorded |
| Chain latency | `GetTotalLatencySamples()` when it was recorded |
| Tone | The preset id and scene, and a **snapshot** of the preset JSON as it was applied. The snapshot means a later re-amp reproduces the tone even after the preset is edited or deleted. `toneChanged` is set if the preset or scene changed during the take |
| Mix | Level, balance, mute and solo. These are atomics, and they are not part of the audio |
| Wet source | `live` (recorded as played) or `reamp` (rendered later, with the snapshot it used) |

### Where the two taps are

- **The DI is tapped inside `ProcessStereoBlock`**: after the mono fold and channel
  select, and **before** the input calibration gain.
  - **Why before calibration.** Riffs are stored raw and replayed through the input
    (`DemoPreviewService::MixIntoInput`), where calibration is applied again. A DI taken
    after calibration would be calibrated twice when it is exported as a riff.
  - **Re-amping.** A re-amp applies the layer's stored calibration gain, so it hears
    exactly what the chain heard.
  - **What is included.** The tap is after the demo clip and the signal-test tone, which
    are both mixed into the input.
- **The wet sound is tapped after the global post-chain**, before the looper adds its
  playback and before master gain.
  - **Alignment.** The chain delays the wet sound, so it is written `chain latency`
    samples earlier in the layer. A note's DI and its wet sound then sit at the same
    loop position.
  - **Latency changes mid-take.** A preset swap can change the latency during a take.
    That shift is not corrected; the layer keeps the latency it started with, and
    `toneChanged` is set.

### Layers and the loop cycle

- **The first layer sets the loop length.** It is either closed by a press (free mode)
  or by the bar count (Bars mode).
- **An overdub layer is at most one cycle long.** Staying in Overdub across the loop's
  wrap closes the current layer and opens the next one on the same sample. Continuous
  overdubbing therefore stacks one layer per pass, the way a pedal does. Each pass can
  still be undone or removed on its own.
- **There is no feedback or decay control.** A summing looper fades old layers to make
  room for new ones. Here every layer has its own level, so older layers can be faded
  by hand.

### Tails across the wrap

A note played near the end of the loop rings on (a reverb, a delay, a sustained chord)
past the end, and should be heard at the start of the next pass.

- **Layers that butt together.** If the next layer is recording by then, the tail is in
  the live output, and the next layer records it. Nothing more is needed.
- **A layer followed by playback only.** After the layer closes, the looper keeps
  writing the wet output onto the *start* of the closing layer, adding to what is there,
  wrapped. This is the **ring-out**. It stops at the first of these:
  - the wet output falls below -70 dBFS;
  - the DI rises above the arm threshold, meaning the player has started playing
    something that is not being recorded;
  - another layer starts;
  - 5 s pass.
- **The DI is never rung out.** A DI tail is just silence.
- **Re-amped layers** don't need the ring-out. The render wraps its own tails (see
  Re-tone).

### Mixing

- **What is summed.** The looper adds each audible layer's wet sound, times its level,
  with a stereo **balance**: unity at centre, and turning towards one side only lowers
  the other.
- **Why balance, not the mixer's pan.** `MultiPresetMixer::ComputePanGains` is
  equal-power, which puts a centred source 3 dB down (see the centre-pan makeup,
  3bba6794). A layer's wet sound is already stereo and was heard at full level, so it
  must come back at full level.
- **Solo** follows the usual rule: if any layer is soloed, only soloed layers sound.
- **Changes are ramped.** Level, balance, mute and solo changes ramp over 10 ms
  (`LinearRamp.h`).

### Undo, redo and removing a layer

- **Undo** removes the newest layer. It fades out over 10 ms and goes on a redo stack.
  **Redo** brings it back.
- **The redo stack** is cleared when a new layer is recorded, or when the memory budget
  needs the space.
- **Any layer can be removed** from the layers panel, not just the newest. It fades out
  and goes on the same stack, so undo brings it back.
- **No audio is copied** by any of these. They change which slots are audible, and
  ownership moves on the message thread (see Realtime design).

## Re-tone: changing a layer's preset later

You pick a preset (and scene) for a layer, or "re-amp all with the current rig". The
service then renders the layer's DI through it **offline**:

1. **Build the graph.** It resolves the preset like `measurePresetLevel` does
   (`PluginControllerPresetLevel.cpp:150`: `PreparePresetForEngine`, missing-resource
   check). It then builds a detached graph with `MultiPresetMixer::BuildDetachedExecutor`
   on one low-priority worker. It never touches the live mixer or `mDSPMutex`.
2. **Apply the calibration.** It applies the layer's stored calibration gain to the DI.
3. **Render the loop twice around and keep the second pass.** The first pass warms up
   the amp state and fills the reverbs and delays. The second pass then includes the
   tails that wrap across the loop point, so the result loops seamlessly.
4. **Compensate the latency.** The render's latency is taken off, the same way the live
   tap does.
5. **Swap it in.** The new wet sound replaces the old one at a block boundary, with an
   8 ms crossfade. The original live recording is kept in the session folder, so "revert
   to recorded tone" is always possible.

Details:

- **Global chains (decide).** The live wet sound went through the global pre-chain and
  post-chain (EQ, Doubler). `measurePresetLevel` passes an empty
  `GlobalSignalChainConfig{}`, so a re-amp built the same way would leave them out.
  **Recommended:** include the *current* global chains in the render, built detached in
  the same way, so a re-toned layer sits like a live one. The layer stores which global
  chain it used.
- **Refused:** presets with hosted plugins (as `measurePresetLevel` refuses them, because
  a second copy of a user's plugin is not safe to run off its editor) and presets with
  missing resources. The UI says why.
- **WASM nodes:** need checking. They run in their own runtime and may not build
  detached.
- **Cost.** About two loop lengths of audio per layer, rendered on one worker.
  - NAM runs many times faster than real time on desktop, so a 16 s loop takes a few
    seconds.
  - On Android it takes longer, and the progress bar matters more.
  - Jobs queue one at a time, and the UI shows progress per layer.
  - The layer keeps playing its old wet sound until the new one lands.
- **A layer that plays through a live chain in real time** is not planned. It would cost
  a chain per layer, which is exactly what this design avoids.

## Recording modes

These apply to every layer. On the first layer they also set the loop length.

| Setting | Values | What it decides |
| ------- | ------ | --------------- |
| **Start** (`looper.start`) | `press` \| `armed` | Recording begins on the press, or on the first note played after it |
| **Length** (`looper.length`) | `free` \| `bars` | First layer only: a second press closes the loop, or it closes itself after `looper.bars` bars |

| | **Length: free** | **Length: bars** |
| - | - | - |
| **Start: press** | Classic looper | Press, optional count-in, exactly N bars |
| **Start: armed** | Waits for the first note, closes on the next press | Waits for the first note, records N bars |

Overdub layers start on the press, or are armed and start on the first note. They end on
a press, or at the wrap if Overdub is held.

### Armed

- **What it listens to.** It listens to the **DI tap**, not the wet output. Amp hiss, and
  the tail of a reverb or delay from before the press, would set off a detector on the
  wet sound.
- **Threshold.** The looper measures the input floor for the first 200 ms after arming.
  It then triggers on a peak above whichever is higher: 12 dB over that floor, or
  `looper.armThresholdDb` (default -50 dBFS). Riff capture's fixed 0.001 (-60 dBFS) can
  sit inside a single coil's hum. The UI says so if arming takes more than a few seconds.
- **Pre-roll.** The take begins 10 ms before the trigger, so the pick attack is not cut.
  While Armed, the engine records into the capture buffer as a ring. The trigger only
  sets where the layer's head is, so nothing is copied (see Realtime design).
- **Overriding it.** A rec press while Armed starts the take immediately. Stop cancels
  the arm.

### Bars

- **The length.** The first layer is exactly `looper.bars` bars at the metronome's tempo
  and time signature (`MetronomeService::BarSeconds()`), rounded to whole samples. The
  tempo is taken when the press happens.
- **Too long.** A bar count that would exceed `looper.maxSeconds` is refused, and the
  strip shows the most that fits.
- **The tempo is locked** while a Bars loop or a Bars project is loaded. The broadcast's
  `editable` flag shows it locked, and Clear or closing the project unlocks it. Changing
  the tempo under a fixed-length loop would set the click and the loop drifting apart.
- **After the first layer** closes, the looper goes to Play, or to Overdub
  (`looper.afterRecord`).
- **Press start.** The press restarts the metronome's bar. It then clicks
  `looper.countInBars` bars (0, 1 or 2, default 1), even with the metronome off, using
  the guidance click riff capture uses. Recording starts on the next downbeat.
  - `looper.clickWhileRecording` (default on) keeps the click through the take.
  - The decision to stop the guidance click is made on the audio thread, through an
    atomic, never by releasing guidance state there.
- **Armed start.**
  - With the metronome off, the first note is bar 1.
  - With it running, the layer snaps to the bar line nearest the first note on the
    latency-corrected grid. That can be up to half a bar back, which the ring already
    holds.

### Staying in time with the click

- **Sample-accurate restart.** `MetronomeService::RequestReset()` gains a sample offset:
  `RequestReset(int sampleOffsetInBlock)`. A count-in then starts on the press's sample,
  and a re-phase can land in the middle of a block. The metronome renders after the
  mixer in the same block, so a request made in the mixer lands in that block.
- **Re-phase at every wrap.** At every wrap of a Bars loop, the looper re-phases the
  metronome to bar 1. This takes out the rounding drift between the metronome's
  fractional beat lengths and a loop that is a whole number of samples.
- **`dsp/TransportClock.h`** holds the samples since the bar line and the samples per
  bar. The metronome writes it and the looper reads it, for the armed snap. Both are on
  the audio thread, so relaxed atomics are enough.

### Latency alignment

The player hears the click one output latency after it is rendered. Their note reaches
the looper's DI tap one input latency later, so a count-in take's downbeat arrives
`D = output + input` samples after the bar line the engine rendered.

- **The wet tap** is already moved back by the chain latency (see Where the two taps
  are). `D` therefore leaves the chain out, unlike rev 2.
- **Bars + press:** the record window is moved `D` later. Playback phase stays on the
  bar line.
- **Bars + armed, snapping to a running click:** it snaps to the grid moved by `D`.
- **Where `D` comes from:**
  - The device latencies, which the standalone already reads
    (`StandaloneAudioSettings.cpp:324-325`). They need exposing through
    `IPluginHost::GetDeviceLatencySamples()`, which returns 0 in plugins.
  - `looper.alignOffsetMs` (default 0, ±50 ms), for drivers that leave the converters
    out of the latency they report.
  - A loopback-cable test (see Tests) measures what is left.

## Signal flow

`ProcessStereoBlock` has three exits that write the block and return:
- the tuner's muted mode;
- the silence sleep gate;
- the normal path, which ends at the limiter.

A loop has to keep playing through all three, so the plan pulls the end of the block into
one function they all call:

```
DI tap:   after the input fold / channel select, before calibration
          → mLooper.NoteInput(diL, diR, numSamples, calibrationGain)   // also feeds the arm detector

FinishOutputBlock(outputs, numSamples):
    mLooper.Process(outL, outR, numSamples, recordEnabled = !tunerMuted)
        // writes outL/R (the chain's output, or zeros) to the recording layer's wet,
        // then adds the audible layers' playback
    master gain / output mute
    Output telemetry tap
    clip limiter
```

- **The sleep gate** measures the chains before the looper (`NoteProcessed`), so
  playback alone never keeps the chains awake. The gate is held awake while the looper
  is Armed, counting in, recording, or ringing out a tail. Otherwise the first note
  after a silence could reach chains that are still asleep.
- **The tuner's muted mode** keeps the loop advancing, so it is still in time when the
  tuner closes, but silent. A layer recording at the time punches out. **(decide)**
  Recommended: muted for v1, with "audible while tuning" as a later setting.
- **File size.** `MultiPresetMixer.cpp` is 1932 lines against a pin of 2250, so there is
  room. `FinishOutputBlock` should still make it shorter, because the three exits each
  repeat the output stage today.

## Realtime design

The engine is JUCE-free, and on the audio thread it never allocates, frees, takes a
lock or releases a `shared_ptr`.

- **Slots.** The engine has a fixed table of `kMaxLayers` (16) slots. Each slot holds a
  raw pointer to the layer's audio and its mix atomics.
- **Who owns the audio.** `LooperService` owns every buffer.
- **Generations.** The engine writes an acknowledged generation after each block. The
  service frees a buffer only once the engine has acknowledged a generation in which
  nothing points at it. The same rule covers removed layers, replaced wet sounds and a
  cleared loop.
- **Commands** from the message thread go through an `SpscRingBuffer<LooperCommand>`.
  Commands from MIDI and DAW automation are applied directly, because those already run
  on the audio thread under `mDSPMutex`.
- **The capture buffer.** There is one buffer of `maxSeconds`, holding DI and wet, while
  the looper is enabled. The first layer records into it, because its length is not
  known in advance in free mode. While Armed it is also the pre-roll ring.
  - The layer is a **circular window** `[head, head + length)` of the buffer, so a
    pre-rolled or snapped start needs no copy.
  - When the first layer closes, the service copies it into an exact-length layer
    buffer off the audio thread. It then swaps the slot's pointer, an O(1) command.
    The capture buffer is free again for the next loop.
- **The spare.** Once the length is known, the service always keeps **one spare layer
  buffer** of exactly that length, zeroed. A new overdub layer takes the spare, and the
  service allocates the next one in `OnIdle`; 35 MB takes roughly 10-20 ms to allocate
  and zero. A layer opened while there is no spare (two layers inside one idle tick)
  is refused, and the UI shows a notice. The wrap that opens the next layer in
  continuous overdub is always at least one loop length after the last spare was taken.
- **State out.** The engine publishes its state, the trigger, the count-in beat and the
  bar it is on as atomics. It publishes per-layer peak buckets (256 per layer, relaxed
  atomics) for the waveforms. `OnIdle` sends them. Nothing is sent from the audio
  thread.
- **Commands apply at block start.** The worst case is 1.3 ms at this machine's
  64-sample buffer, and 21 ms at a 1024-sample buffer.

### Memory

DI is one channel in mono input mode. Float, 48 kHz.

| Loop length | One layer (mono DI + stereo wet) | 8 layers | Capture buffer at `maxSeconds` = 60 |
| ----------- | -------------------------------- | -------- | ----------------------------------- |
| 8 s (4 bars at 120) | 4.6 MB | 37 MB | 35 MB |
| 16 s | 9.2 MB | 74 MB | 35 MB |
| 60 s | 35 MB | 276 MB | 35 MB |

A stereo DI adds a third. **(decide)** Recommended:
- `maxSeconds` 60 by default (30, 60, 120 or 300);
- 16 layers;
- a memory budget of 512 MB (`looper.memoryBudgetMb`), past which a new layer is refused
  and the redo stack is dropped first.

## Looper projects

A project is a folder in the riff library:

```
<riff library>/loops/<projectId>/
    project.json
    layers/<layerId>.di.wav        32-bit float, 1 or 2 channels, recorded span only
    layers/<layerId>.wet.wav       32-bit float, stereo, recorded span only
    layers/<layerId>.recorded.wav  the original live wet, kept after a re-amp (for revert)
```

- **`project.json`** holds:
  - `version`, `id`, `title`, `created`, `modified`, `sampleRate`, `lengthSamples`;
  - `tempoBpm`, `timeSigNum`, `timeSigDen` and `bars` (Bars projects);
  - per layer: `id`, `title`, `order`, `startSample`, `endSample`, `diChannels`,
    `calibrationGainDb`, `latencySamples`, `level`, `balance`, `muted`, `solo`,
    `wetSource`, `tone {presetId, presetName, sceneId, snapshot}`, `toneChanged`,
    `globalChain` (when re-amped), and `riffId` (when it came from a riff).
- **The index.** A `looper-projects` document in the storage DB, next to
  `riff-library`, lists `{id, title, path, modified, lengthSec, layerCount, tempoBpm}`.
  It is a separate document, so the riff index's schema does not change. **(decide)**
  Recommended: separate.
- **WAV format.** Project WAVs are 32-bit float, so wet sounds keep their headroom and
  nothing is dithered. `util::DecodePcmWav` already reads IEEE float
  (`Wav.cpp:105,149`). `util/Wav.h` needs an `Encode32BitFloatWav` next to
  `Encode16BitWav`.
- **The session folder.** Each layer is written in the background to
  `<riff library>/loops/.session/` as soon as it closes, on a low-priority worker,
  reading the layer's immutable buffer with no lock.
  - **Save** writes `project.json` and moves the folder.
  - **Crash recovery.** A crash loses at most the layer in progress, and the next launch
    offers to recover the session. **(decide)** Recommended.
- **Load.**
  1. The files are decoded and resampled to the current rate (`ConvertToSampleRate`) on
     a worker.
  2. The layer buffers are built, then committed into the slots as one command.
  3. A Bars project sets and locks the metronome's tempo and time signature.
  4. A project recorded at another rate plays resampled. Re-amping it renders its DI at
     the current rate.
- **Unsaved changes** prompt before Clear, loading another project, or quitting.

### Riffs ↔ layers

- **Export layer as riff.** The layer's DI (its recorded span) is written through the
  existing take path: 16-bit stereo, a mono DI written to both channels, under
  `<library>/takes/<riffId>/`. It goes in as a new riff or a new take of an existing
  one, with `tempoBpm`, `bars`, the time signature and `presetId` filled in from the
  layer. Because riffs are DI and replay through the rig, this is the DI, never the wet
  sound.
- **Export the wet sound** to a WAV file the user chooses (float or 24-bit). It is not
  added to the riff library.
- **Export the mix:** all audible layers rendered to one WAV, with each layer's level
  and balance. Later.
- **Add riff as layer.**
  1. The riff take is decoded and resampled. A take matching the loop's tempo and bars
     fits as it is. Otherwise the UI offers to pad or trim it to the loop length, or to
     start a new project at the riff's length.
  2. It is re-amped through the current rig (or the riff's `presetId`) to make its wet
     sound.
  3. It becomes a layer with `riffId` set.

## LooperService (core/src/controller/LooperService.{h,cpp})

Built like `PracticeToolService`: it registers its own messages through
`RegisterMessageHandlers`. It owns:
- the buffers, the spare and the generations;
- the command queue;
- the re-amp worker and the session writer;
- project save, load and indexing;
- the state feed;
- the settings.

It will not fit in one file under the 800-line limit. Split it the way the practice tool
is split:
- `LooperService.cpp`: lifecycle, buffers, commands;
- `LooperServiceProjects.cpp`: save, load, index, session, riff export and import;
- `LooperServiceReamp.cpp`: the render worker;
- `LooperServiceMessages.cpp`: the messages.

**Settings** are app settings. The loop audio is never stored in app settings or in DAW
host state.

| Key | Default |
| --- | ------- |
| `looper.enabled` | false (follows the feature flag) |
| `looper.maxSeconds` | 60 |
| `looper.memoryBudgetMb` | 512 |
| `looper.level` | 1.0 (all layers) |
| `looper.start` | `press` |
| `looper.length` | `free` |
| `looper.bars` | 4 |
| `looper.countInBars` | 1 |
| `looper.clickWhileRecording` | true |
| `looper.afterRecord` | `play` |
| `looper.armThresholdDb` | -50 |
| `looper.alignOffsetMs` | 0 |

### Messages

These go in `core/protocol/ui-messages.json`, and in `docs/user-interface.md`'s tables.

| Direction | Type | Payload |
| --------- | ---- | ------- |
| UI → engine | `looperCommand` | `{command: "recPlayDub" \| "record" \| "play" \| "overdub" \| "stop" \| "undo" \| "redo" \| "clear"}` |
| UI → engine | `setLooper` | settings fields |
| UI → engine | `setLooperLayer` | `{layerId, level?, balance?, muted?, solo?, title?}` |
| UI → engine | `removeLooperLayer` | `{layerId}` |
| UI → engine | `retoneLooperLayer` | `{layerIds[] \| "all", presetId?, sceneId?, revert?}` |
| UI → engine | `exportLooperLayer` | `{layerId, as: "riff" \| "wetWav", riffId?, path?}` |
| UI → engine | `addRiffAsLooperLayer` | `{riffId, takeId, fit?: "pad" \| "trim" \| "newProject"}` |
| UI → engine | `saveLooperProject` / `loadLooperProject` / `deleteLooperProject` / `getLooperProjects` | `{projectId?, title?}` |
| engine → UI | `looperState` | `{state, positionSec, lengthSec, bars?, tempoBpm?, canUndo, canRedo, countInBeatsLeft?, barIndex?, armWaitingSec?, projectId?, dirty, memoryMb, notice?}` |
| engine → UI | `looperLayers` | `{layers: [{id, title, order, span, level, balance, muted, solo, tone, wetSource, toneChanged, retone?: {progress}}]}`, sent on change only |
| engine → UI | `looperWaveforms` | `{layerId → {peaksL[], peaksR[]}, generation}`, sent when a layer closes or changes, about 4 times a second while recording |
| engine → UI | `looperProjects` | the index |

### Footswitch, keyboard and DAW

These are registry entries in `AutomationSlotTable::InitializeRegistry`, so they appear
in MIDI learn and the keyboard map:
- `looper.recPlayDub`, `looper.stop`, `looper.undo`, `looper.redo` and `looper.clear`
  (triggers);
- `looper.level` (continuous).

Each `apply` only enqueues or sets an atomic. They are **not** appended to
`kDefaultSlots`, because that list fixes the VST3 parameter order.

The single-switch gestures (hold for undo, double-tap to stop) need switch-release events,
which trigger slots don't get today, so they are left for later.

## UI (WebView)

- **The looper strip.** A footer button toggles a strip docked above the footer. It
  stays open over the Play panel and the pads, because a performance action must never
  be a menu away (`navigation.ts:287-295`). It holds:
  - **Transport:** the state button (Rec, Play or Dub), Stop, Undo and Redo, Clear.
  - **A summary waveform** with a playhead and bar lines in Bars mode
    (`core/ui/ts/waveform/render.ts`).
  - The time, the bar ("bar 3 of 4") and the layer count.
  - **Modes:** Press | Armed, and Free | Bars with a bar stepper.
  - **A tempo chip** that opens the metronome, shown locked while a Bars loop exists.
  - **Project:** the title, an unsaved marker, Save and Open.
  - The states have to be readable from across a room: Armed pulses with "Play to
    start", and the count-in shows the beats left in large type.
- **The layers panel** opens from the strip. One row per layer, newest at the top:
  - a colour and a mini waveform;
  - the name;
  - a **tone chip** with the preset name. Tapping it opens the preset picker to re-amp,
    and it shows the progress and a "recorded tone" revert;
  - level, balance, mute and solo;
  - export (as a riff, or as a wet WAV) and remove.
  - "Re-amp all with the current rig" sits at the top. A `toneChanged` layer shows a
    marker.
- **The riff library** (Jam → Riff Library) gets a **Loops** list next to Saved Riffs:
  open, rename, delete, recover session. Each riff gains "Add to loop".
- **Riff capture's modal** stays, and records through the looper (Phase 6).
- **Performance pads:** a looper row with Rec/Play/Dub, Stop, and Undo.
- **Compact layout:** the strip collapses to the state button, Stop and Undo, plus a
  progress bar. The layers panel becomes a full-height sheet.
- **Feature flag:** `Features.Looper`, in the power group, off by default until it has
  been played live.
- **Files:** new UI code goes in `core/ui/ts/looper/`.

**Nano** is out of scope. Its natural home is a TransportBar sheet, and that is a scope
decision of its own.

## Plugin (DAW) builds

**(decide)** Recommended: **standalone only**, like the metronome. The reasons:
- A DAW records anyway.
- There is no host PPQ (`IPluginHost.h`).
- The memory would be paid per instance.

The engine is host-agnostic, so this stays a gating decision and does not affect the
design.

## Phases

1. **Engine.**
   - `LooperEngine.h`: slots, the capture ring, the DI and wet taps with latency
     alignment, layers per cycle, the ring-out, mixing with balance, Armed, Bars,
     CountIn.
   - `TransportClock.h` and `SeamCrossfade.h`.
   - `FinishOutputBlock` and the DI tap in the mixer.
   - Unit tests. The engine is driven by tests only, with the clock and latency injected.
2. **Service and control.**
   - `LooperService`: ownership, spares, generations, the command queue, settings, the
     state, layers and waveform feeds.
   - The metronome link: `RequestReset(offset)`, the clock, the guidance stop flag, the
     tempo lock.
   - `GetDeviceLatencySamples`.
   - The registry entries and the protocol manifest.
3. **WebView UI.** The strip, the layers panel (mix, remove, undo), the pads and the
   compact layout, behind the flag. Live checks.
4. **Projects and the riff library.** The session writer, save and load, the index,
   `Encode32BitFloatWav`, export as riff or wet WAV, add a riff as a layer, the Loops
   list, crash recovery.
5. **Re-tone.** The detached render with global chains and the two-pass wrap, the job
   queue and its progress, the swap and revert, refusing hosted plugins.
6. **Riff capture on the looper's recorder.**
   - Port the modal's record path onto a one-layer session.
   - Delete `RiffCaptureRuntime` and `ProcessRiffCaptureBlock`.
   - Check that the takes it writes are the same as today's. The engine's DI comes in
     through `NoteInput`, so a riff session can feed it from `ProcessAudioLocked` before
     the demo clip is mixed in. That is today's tap.
   - Check that the riff tests still pass.
7. **Later.** Each of these is its own decision:
   - quantised free mode;
   - set tempo from loop;
   - export the mix;
   - single-switch gestures;
   - plugin builds;
   - Nano;
   - reverse and half speed;
   - per-layer trim and offset editing.

## Tests

**Engine (`LooperEngineTests.cpp`).**
- Every state transition, including presses in impossible states (they are ignored).
- DI and wet taps:
  - A known impulse through a graph with known latency lands at the same loop position
    in both.
  - The DI is before calibration.
  - A mono input mode gives a one-channel DI.
- Layers per cycle: holding Overdub across three wraps gives three layers, joined
  sample-accurately.
- Punch ramps.
- Ring-out: it stops on decay, on DI input, on a new layer, and after 5 s.
- Mixing:
  - Balance is unity at centre.
  - Solo and mute.
  - Level ramps.
  - Ten layers cost under a stated budget per block.
- Undo and redo; removing any layer.
- Armed: trigger position, pre-roll inside a block, floor-relative threshold over hum,
  a long armed wait that wraps the ring.
- Bars:
  - It closes on the exact sample for 4/4, 3/4, 6/8 and 7/8, at 40, 120 and 240 BPM, at
    44.1, 48 and 96 kHz.
  - The count-in is exact.
  - The record window moves by `D`.
  - The armed snap goes both ways.
  - The metronome stays in phase over 30 minutes of wraps.
- **Realtime safety:**
  - No allocation or free on the audio thread for any command (`AudioThreadAllocations.h`).
  - A buffer is never freed before its generation is acknowledged; this is checked under
    a stress thread.
- No NaN or Inf (`FiniteCheck.h`), and the clang-cl build too.

**Mixer (`MultiPresetMixerTests`).**
- Layers survive a preset swap, a scene change and a global-chain rebuild, bit-identical.
- Playback through a sleeping gate.
- The gate is held awake while Armed or recording.
- The tuner's muted mode.
- A mono output fold.
- Master, mute and the limiter apply to the loop.
- The metronome and the practice tool are never recorded.

**Projects.**
- Save, then load, round-trips bit-exact at the same rate.
- Load at another rate.
- Session recovery after a simulated crash, and the index is kept consistent.
- An exported riff take replays through `DemoPreviewService` at the same level as the
  live layer. This checks that calibration is not applied twice.
- Riff → layer → riff round-trips.
- Non-ASCII project paths (`PathToUtf8`).

**Re-tone.**
- A re-amp of a layer through its own snapshot, with the same global chains, matches the
  live wet sound to within the chain's non-determinism. A NAM preset should null deeply.
- The two-pass render loops with no step at the seam.
- Hosted-plugin presets are refused.

**UI:** vitest for the strip, the layers panel, the flag gating and the compact collapse.

**Live, over CDP** (`tools/agent-ui-debug`):
1. Loop the demo clip in free, Armed and Bars modes.
2. Layer three passes, rebalance them, remove the middle one, then switch presets while
   the loop runs. Listen for clicks.
3. Re-tone a layer and check there is no glitch at the swap.
4. Save, clear, load, and compare the output with `sldS`.
5. Sample the per-callback DSP time with 8 layers playing, against the 1333 µs deadline.
6. Loopback cable on the Yamaha Steinberg interface: record the click in a Bars + press
   take, and measure what is left of `D`.

## Docs to update when it lands

- `docs/signal-chain.md`: the block order and the taps.
- `docs/features.md`: the looper, and the riff library's Loops.
- `docs/user-interface.md`: the messages, the strip, the layers panel and the pads.
- `docs/data-models.md`: `project.json` and the `looper-projects` index.
- `docs/agent-quickstart.md`: the services table.
- `changelog.md`.

## Decisions

| # | Decision | Status / recommendation |
| - | -------- | ----------------------- |
| 1 | What a loop records | **Decided (rev 3):** DI and wet, per layer |
| 2 | Loop audible while the tuner mutes | Muted, but keeps time |
| 3 | Capture length, layer cap, memory budget | 60 s; 16 layers; 512 MB |
| 4 | UI home | Docked strip, a layers panel, Loops in the riff library, a pads row |
| 5 | Plugin builds | Standalone only |
| 6 | Armed and Bars in v1 | **Decided (rev 2):** yes |
| 7 | Metronome tempo while a Bars loop or project is loaded | Locked |
| 8 | Bars + armed with the click running | Snap to the nearest bar line |
| 9 | After a Bars first layer | Play; Overdub as a setting |
| 10 | Arm threshold | 12 dB over the measured floor, never below -50 dBFS |
| 11 | Riff capture | **Decided (rev 3):** re-implement the recorder, extend the library |
| 12 | Holding Overdub across the wrap | A new layer per pass |
| 13 | Where the DI is tapped | After the input fold, before calibration; calibration stored per layer |
| 14 | Re-tone and the global chains | Include the current global chains in the render |
| 15 | Project index | Its own `looper-projects` document |
| 16 | Session autosave and crash recovery | Yes |
