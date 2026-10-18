# DSP effects review, 2026-10-18

A full pass over `core/src/dsp/effects/` and the DSP helpers they share, looking at
correctness, completeness and audio-thread cost. Seven read-only reviewers each took one
family:

- dynamics
- drive and amp
- modulation
- delay and reverb
- pitch and synth
- EQ, cab and pickup
- NAM, composite and WASM

Every finding was re-read end to end before it was reported. The rule was the bug hunt's
(docs/bug-hunt-2026-10-17-review.md): a real defect with a plausible symptom, or a
meaningful audio-thread cost with a clear cheaper alternative. Latent defects behind impossible
inputs and cosmetic items were left out.

Nothing rated high, meaning audible or a crash in normal use. Most of what was found is one
theme: a control moved while playing, from the panel, MIDI or DAW automation, steps the
signal instead of gliding. Since `SetParam` runs on the audio thread for MIDI and automation,
nothing upstream smooths it.

## Fixed

Each fix has a regression test that fails on the code before it.

| Effect | Defect | Test |
|---|---|---|
| Synth Voice | Pitch clamped at 2 kHz *after* the octave shift: at Octave +2 every note from about C5 up played the same B6 (Octave +1 from about B5, Voice 2 at +12 too). Now clamped at 0.45 × the sample rate, which PolyBLEP stays clean up to. | `SynthSawEffectTests`, A5 at Octave +2 plays 3520 Hz (was 2000) |
| Synth Voice | A jump wider than a fifth reset both oscillators' phase at full level: a step of up to full scale on Sine and Triangle. The phase now resets only while the voice is silent. | `SynthSawEffectTests`, largest step 0.057 of peak (was 0.55) |
| Digital Delay | Time, Spread, Division and the tempo moved the read head in one step: a crackle on every knob step, and all through a DAW tempo ramp with Sync on, since the executor pushes `bpm` every block it moves. Time and Spread now glide (40 ms, capped at the tape delays' speed limit), and snap until audio has run so a loaded preset doesn't sweep. The ducking follower's coefficients were per-sample constants tuned at 48 kHz, so it ran 4× faster at 192 kHz; they are now set in ms. The `%` per read and the `sin` per sample with modulation off are gone. | `EffectModeTests`, Time changes (0.46 and 0.15 of peak → 0.06) |
| Noise Gate | Gate → Swell mid-note dropped a ringing note to the floor (−80 dB) in one sample: Swell's position was never moved in Gate mode, and its pick detector started with no history, so it read the ringing note as a new pick. Swell now starts at the gate's gain, and the detector warms up for its history window. | `EffectModeTests`, −0.00 dB after the switch (was −54.8) |
| Mixer (every split's merge) | Level, pan, master and mute stepped once per block. A delay change jumped the read head. A muted input skipped its delay line, so unmuting replayed up to 60 ms from before the mute. Now level, pan, master and mute glide over 10 ms, a delay change crossfades, and the executor runs muted inputs so the line stays current. | `ParamGlideTests` (0.48 / 0.85 of peak and a 0.135 replay error → 0.03 / 0.03 / 0) |
| Gain | Applied as a block-constant step. Now a 10 ms glide, landing at once before audio starts. | `ParamGlideTests` (0.94 → 0.03) |
| NAM resampler | The latency it reported came from the resampler's own formula, which counts only the upsampler's pre-roll, so it was short whenever host and model rates differ: 27 against 33 at 44.1 kHz, 70 against 120 at 192 kHz, 91 against 210 with linear phase. The NAM amp's dry path at Mix < 1 is aligned by it (a comb, first notch near 3.7 kHz at 44.1 kHz), as are a mixed-rate blend's lanes and host delay compensation. `NamOversamplingProcessor::Prepare` now measures the round trip with a smooth pulse (the first-moment shift is the low-frequency group delay) and reports that. | `NamResamplerLatencyTests`: a 30 Hz phase measurement agrees within 0.5 samples in all 7 cases (6 failed before) |
| Composite (custom effects) | The tempo never reached inside: composites didn't declare `requiresTempo`, and `SetParam` dropped `bpm`. A synced delay, tremolo or chorus inside a custom effect always ran at 120 bpm. | `SignalGraphExecutorTests`, Tempo delivery inside a composite (120 → 132) |
| Chorus, Flanger | Delay and Depth moved the read heads in one step. They now glide (30 ms one-pole). | `ParamGlideTests` (0.57 / 0.31 → 0.03) |
| Doubler | Time moved an integer read head in one step. Now a fractional, glided head (30 ms, speed-capped). | `ParamGlideTests` (0.18 → 0.03) |
| Octave | Into and out of its transparent bypass was a hard switch: Mix 0 → 0.01 jumped the audio 80 ms in time. It now crossfades over 10 ms, as Pitch Shift does. | `ParamGlideTests` (1.63 → 0.03) |
| NAM amp | Input and Output gain (calibration included) stepped once per block, so an expression pedal on Output as a volume pedal zippered. Both now glide over 10 ms. The unity fast path is skipped while Output glides. | `ParamGlideTests` |

## Open

These are ranked most severe first. Line numbers are as of d1524dce.

### Medium

- **IR Cab: gains and switches step.** `mix`, `irBlend`, slot gain and pan, `outputGain`
  and auto comp are read per block. Polarity, L/R Split and mic emulation flip on one
  sample. `airMode` also zeroes the running air-filter state (`IRCabEffect.h:633-729`,
  `396-460`). Inverting IR B at Blend 0.5 is a click. Left for the session working on the
  IR Cab's blend compensation, which has this file open.
- **Speaker Drive ignores the nominal operating level.** The −18 dBFS threshold, its 6 dB
  limit and the excursion knee are fixed in sample units (`SpeakerDrive.h:102-157`), while the
  amp and drive pedals scale by `GetNominalOperatingLevelDbfs()`. At a −6 dBFS nominal level,
  Drive 1 is a constant 6 dB cut, and at −30 it barely engages. Changes the sound for anyone
  off the default, so it wants an A/B and a decision.
- **Simple Cab: Mic 2 on or off makes Mic 1 swoop.** Taps are measured from the nearer
  *active* mic (`SimpleCabVoicing.h:443-450`), and the reference slews at 0.05 samples per
  sample. Raising Mic Blend from 0 gives about a 0.84-semitone dip for 21-58 ms.
- **Spatial 3D's Doppler mode leaves the wet path late.** The wet delay adds the
  propagation time, but the dry path and `GetLatencySamples` count only the bias
  (`Spatial3DEffect.h:974-996`, `528-531`, `786-789`). At Mix < 1 that is a comb (4.4 ms at
  the default 1.5 m), which sweeps with distance. Switching Smooth → Doppler ramps about 210
  samples over one 64-sample sub-block.
- **Auto Arpeggiator: the pitch trigger switches with no fade.** Above or Below drops the
  output from dry to `dry × (1 − mix)` in one sample, and back (`AutoArpEffect.h:145-168`,
  `660-669`).
- **The experimental STFT and Hybrid transposes** (behind ExperimentalEffects):
  - They report `analysis − synthesis` latency; the real figure is 2 × synthesis.
  - The STFT dry path isn't delayed at all, and Hybrid's is aligned to the wrong figure.
  - A semitone or option change runs `RebuildPipeline` (allocation) or `Reset()` (a 5-20 ms
    gap) on the audio thread (`StftTransposeEffect.h:46-57`, `115`, `200-219`, `548`;
    `HybridTransposeEffect.h:134`, `190`, `619`).
  - The bypass at 0 st is a hard cut.

### Low

- **Tempo-synced rates are clamped to the free-rate range.** Flanger ≤ 5 Hz, Chorus ≤ 10,
  Phaser ≤ 8, Vibe 0.3-12, Spatial 3D ≤ 4. At 120 bpm, six of the Flanger's 15 divisions run
  at 5 Hz and drift off the grid, and the UI doesn't show `effectiveRate`.
- **NAM blend: calibration follows the selection, not what is playing.** In snap mode, the
  outgoing model plays at the incoming one's input and output gain for up to 0.28 s
  (`MultiModelNAMAmpEffect.h:294-301`, `1158-1199`). It also has no non-finite scrub, which
  the NAM amp got in b668b1e0.
- **NAM amp tone stack.** With all four tone controls at 0 the filters are skipped, and they
  resume with stale state: a thump on the next tone change (`OptimizedNAMAmpEffect.h:237-298`).
- **WASM effect.** The dry path isn't delayed by the module's reported latency
  (`WasmEffect.cpp:816-841`).
- **Mix and output steps elsewhere.** The Digital, Analog and Tape delays' Mix, IR Reverb's
  Mix and Output, and pre-delay and early-tap reads in the reverbs all step. Pedal sweeps
  zipper.
- **IR Reverb Quality.** A Quality change swaps in history-less convolvers, which cuts the
  tail (`IRReverbEffect.h:318-324`). IR Cab solved this in e80df677.
- **IR Cab rapid loads.** A second IR load or Quality commit inside the 30 ms fade drops the
  outgoing side halfway (`IRCabEffect.h:1138-1153`, `1672-1682`).
- **Non-finite input latches.** Digital Delay, the algorithmic and spring reverbs, and
  Ambient (without Shimmer) don't scrub their input, so one non-finite sample from upstream
  silences them until a reload. Analog and Tape do scrub theirs.
- **Synth Voice's gate is hard on and off.** At a Gate of about −30 dB it ticks at each note's
  start and end (`SynthSawEffect.h:227`).
- **Drive pedals with blocks under 16 samples.** The knob glide and redesign rate depend on the
  block size (`DrivePedal.h:102`, `275-283`).
- **One-off allocations in `SetParam`.** The first write of a new key allocates once in
  `MultiModelNAMAmpEffect.h:291`, `WasmEffect.cpp:883`, and a composite's
  `SetNodeParam` (`node->params[key]`).

### Performance (no audible defect)

- **`RealtimeConvolver`** runs complex FFTs on real input: a real FFT would roughly halve the
  IR Cab's and IR Reverb's convolution cost (3 s stereo reverb about 1.4% of a core).
- **Signal Analyzer.**
  - It computes the Hann window inside every Goertzel bin: 64 `cos()` per sample, about 3.3%
    of a core at 48 kHz/128, against 0.6% hoisted.
  - Its integrated loudness does 4096 `pow()` every 100 ms once the history fills.
  - Its spectrum resolution and smoothing depend on the host block size.
- **Wah** designs its filter every sample in Auto mode and while the pedal moves: four `pow`
  and a `tan`.
- **Integer division per read.** Spring and Ambient reverbs wrap reads with `%` (about 38 and
  52 per sample). Algorithmic Reverb replaced the same pattern with `WrapReadIndex`.
- **Per-sample transcendentals.**
  - Chorus, Flanger and Tremolo use `sin`/`tanh` per sample where a phasor with `FastSin01`
    would do.
  - `ClipCurve::Antialiased` recomputes `Apply(bias)` at 8× (`DriveStages.h:176`).
- **Control-rate restarts.** Spatial 3D and Ring Mod restart their control-rate countdown on
  every `Process` call, so tiny host blocks redesign every call.
- **Noise Reduction's hiss stage** runs fully with Hiss at 0.

## Checked and fine

- **Filters and oversamplers:**
  - the RBJ designs and their Nyquist clamps
  - the half-band IIR and amp FIR oversamplers, whose latency matches what they report
  - antiderivative antialiasing integrals
- **Convolution:** `RealtimeConvolver`'s zero-latency partitioning, and IR alignment and trim.
- **Glides and ramps:** the built-in amp's per-sample control glides, the drive pedals' model
  switches.
- **Dynamics:** the compressor's knee and makeup, the gate detector and hysteresis, Noise
  Reduction's hum bank.
- **Delay lines:** buffer sizing at 192 kHz in every delay line, and feedback bounds
  everywhere.
- **Pitch:**
  - Pitch Shift and Transpose: latency and dry alignment
  - SpliceTransposer and the pitch tracker, at every rate
  - Guitar to MIDI's note handling: no stuck notes
- **Legacy migrations:** drive, amp and wah.
- **Channel flags:** every `CanWiden` and `KeepsChannelsSeparate` claim.
- **Denormals:** the JUCE callback runs under `ScopedNoDenormals`.
- **Aliasing:** the executor never processes in place.
