# Changelog

## Unreleased

### Effects & DSP
* Added a conventional **Wah** effect alongside the Auto-Wah. Its Pedal Position is made to be mapped to an expression pedal, a MIDI CC or DAW automation, and is smoothed so a MIDI pedal sweeps without zipper noise. Heel and toe frequency, Q and how it narrows across the sweep, pot taper, toe gain, low end, treble, inductor-style saturation and an optical-style response lag cover most wah designs, and 34 factory presets voice popular, boutique and artist signature pedals — the Cry Baby GCB-95, Vox V847 and Clyde McCoy, Morley, Weeping Demon, Fulltone Clyde Deluxe, Real McCoy RMC3 and Xotic XW-1, plus signature wahs for Steve Vai, Zakk Wylde, Eddie Van Halen, Slash, Kirk Hammett, Dimebag Darrell, Jerry Cantrell, Buddy Guy, John Petrucci, Joe Bonamassa, Mark Tremonti, George Lynch and Joe Satriani. Auto-Engage switches the wah off once the pedal rests at the heel, for expression pedals without a toe switch.
* **Pitch Shift** gained a Snap to Semitone switch and a Range Min and Max. With Snap off the pitch glides freely, and with it on the pitch moves in whole semitones. The range limits the Semitones knob, and it is also what an expression pedal, MIDI CC or DAW automation sweeps, so heel to toe can cover exactly 0 to +7 semitones, for example. Previously a mapped pedal always swept the full -12 to +12.
* Added a new **3D Spatial** effect that places your signal as a point source anywhere around you — left/right, front/behind, above/below and near/far — with a draggable radar and elevation display in the effect panel. Includes seven motion modes (Orbit, Arc, Figure 8, Spiral, Drift, Pendulum), optional tempo sync, a Doppler mode, a Speakers listening mode for playback that isn't on headphones, and 11 factory presets. Designed for headphones; on speakers the left/right and distance cues still work.
* Added configurable NAM processing quality under Settings → DSP Performance: oversampling (Off, 2x–32x) and anti-alias filter phase, applied to every NAM amp, NAM FX and NAM Blend node. In a DAW these belong to the individual plugin instance and are saved with the project, so two instances can run at different quality tiers.
* Offline bounces, freezes and exports now automatically render NAM at higher quality (full model size, at least 2x oversampling) without changing the settings you play with live.
* Standard NAM models (and LSTM models) now use the NAM core's fast tanh approximation, which cut their CPU cost by about 9% in our benchmark. The difference in output sits about 52 dB below the signal, well under a typical capture's own difference from the real amp. A2 models don't use tanh and are unchanged.
* Fixed NAM amps putting out invalid audio just after a model loaded or a NAM quality setting changed, when the model's sample rate differs from the session's (a 48 kHz model in a 44.1 kHz session, say) and the audio buffer is small. The new NAM resampler was starting up from uninitialised memory. With a Linear anti-alias filter, at buffers up to about 128 samples, the amp could stay broken; with Minimum Phase, at 64 samples or less, it usually dropped out for about a tenth of a second instead.
* macOS, Linux and Android: a NAM amp whose model's sample rate differs from the session's, with the Minimum Phase anti-alias filter, now recovers from an invalid sample from the model or from the effect before it, at the cost of a click. It used to put out invalid audio until the amp was reloaded, because optimised builds had silently removed the NAM resampler's checks for invalid values.
* Fixed a NAM amp putting out invalid audio, or silence, until it was reloaded, after a single invalid sample from the model or from the effect before it. This happened on every platform with oversampling on and the Minimum Phase anti-alias filter (the default) when the session runs at the model's sample rate or a power of two times it (a 48 kHz model at 48, 96 or 192 kHz, say): that path had no check for invalid values at all. The amp now recovers at the cost of a click.
* Fixed a NAM amp replaying a moment of old audio when the host restarted its audio, with the Minimum Phase anti-alias filter, when the model's sample rate differs from the session's and either oversampling is off or the two rates are not a power of two apart (a 44.1 kHz model in a 48 kHz session, say).
* Fixed hosted plugins being fed stale audio when the host delivers a block smaller than the declared maximum (e.g. ASIO buffer sizes below 256). The plugin now sees exactly the frames the host provided instead of processing the previous block's tail.
* Hosted plugins are now explicitly resumed after `prepareToPlay`, so plugins that gate processing on `isSuspended()` no longer load silently without affecting the audio.
* Fixed clicks and stale output from hosts that deliver larger audio blocks than they declare — oversized blocks are now split in the mixer and IR Reverb.
* IR Reverb now caches resampled impulse responses and ignores redundant quality changes, so changing settings no longer causes audio dropouts.
* IR normalization gain is now sample-rate compensated, so IR levels stay consistent whatever rate the host runs at.
* Graphic EQ's Profile dropdown has been replaced by the effect Presets dropdown. The profiles are now flat band layouts listed under Factory, differing only in how many bands they have and where they sit, so choosing one no longer imposes a voicing. A newly added Graphic EQ starts flat, and your own curves are saved alongside them as effect presets.
* Graphic EQ gained a Reset button that returns every band to 0 dB without changing the selected profile.
* **Synth Voice** follows your playing more closely: it reaches a new note up to twice as fast, tracks notes down to 45 Hz (low F# on an eight-string), and uses a small fraction of the CPU it did, so it no longer crackles at small buffers or high sample rates.
* The **Auto Arpeggiator**'s pitch trigger now switches at the pitch you set, where it used to fire a semitone or two below it, and works at 88.2 kHz and above.
* The tuner gives steadier readings, reads notes above about 1 kHz in the right octave, and works at 176.4 and 192 kHz.
* **Overdrive, Distortion and Fuzz** now model fifteen classic pedals behind a Model switch: the TS-808, Klon Centaur, Bluesbreaker, Timmy, Fulldrive and LPB-1 boost; the RAT, DS-1, Distortion+ and Metal Zone; and the Fuzz Face, Big Muff, Tone Bender, Maestro Fuzz-Tone and Super-Fuzz. Swap the clipping diodes, tighten the bass, starve a fuzz's bias, or EQ the distortion like a Metal Zone. They sound smoother at high gain, clean up when you play softly, and switching one on no longer jumps the volume. Your existing presets keep their loudness.
* Fixed long convolution reverb IRs crackling at small buffer sizes. A multi-second IR now spreads its work evenly across audio callbacks instead of doing it in one burst. With a 4.7 s plate at 48 kHz and a 64-sample buffer, the slowest callback went from 142% of the available time to 54%.
* Fixed filters misbehaving at low or unusual sample rates: the NAM amp's presence control at 8 kHz, the simple cab at 11.025 kHz, and the built-in amp at fractional rates. These fixed-frequency filters are now kept below Nyquist.
* macOS and Linux: turning NAM auto input calibration off no longer leaves NAM amps playing at the wrong level. The Parametric EQ, Graphic EQ and Flanger also recover from an invalid sample in their input, instead of producing bad output until the effect is rebuilt. Optimised builds had been silently removing the checks for invalid values.

### Presets & Workflow
* Effects now have a Presets dropdown in their header, opening a small flyout with a single list of factory presets followed by your own saved settings. Save the current settings of any effect under a name and load them onto any other preset.
* Added back/forward buttons beside the preset selector to step through recently loaded presets (up to 10), for quick A/B comparison between tones.
* Added **undo/redo and A/B** for the signal chain, in the footer (#43). Undo and redo cover nodes, wiring, parameters, bypass and node settings, and a knob drag counts as one step. Use Ctrl/Cmd+Z to undo, and Ctrl/Cmd+Shift+Z or Ctrl/Cmd+Y to redo. A and B hold two independent versions of the chain to compare, and the ⇄ button copies the live chain to the other slot. A change that keeps the chain's shape is applied without reloading the preset, so the sound isn't interrupted. Loading another preset or scene resets both slots to that preset.
* Presets can be dragged onto a setlist pad to assign them, either from the library popover or from the loaded preset's name in the toolbar. If the pad already holds a different preset, you're asked before it's replaced.
* Fixed replacing an effect with one from a different category, such as a delay with a reverb, which was being rejected without any message.
* Hosted plugin state is now kept through chain edits, scene switches, Multi-Rig slots, plugin swaps and standalone restarts (#23).
* Preset switching is now genuinely gapless: the outgoing preset crossfades into the incoming one over ~21 ms instead of being cut dead, and switching cost dropped from ~32 ms to ~11 ms through a NAM model cache and building signal graphs off the audio lock.
* Delay and reverb tails now carry over a preset or scene switch. The old preset's input fades away and stays disconnected through its final release. Tail state survives silent gaps between repeats for the selected duration: two bars at the current tempo by default, configurable from Off to four bars under Settings → General → Preset Switching. Rapid switches release older tails early and cap the number of retiring chains.
* Fixed Multi-Rig scene changes rebuilding under the audio lock, excessive retiring chains during rapid scene changes, and a shutdown deadlock involving retired hosted-plugin editors.
* Selecting a setlist slot (from the pads, a footswitch or MIDI) now reliably swaps the active preset rather than stacking another one on top of it, and the swap is performed once by the engine instead of twice.
* Fixed unsaved chain edits, such as a newly added wah, silently disappearing when the preset being edited was selected again. A setlist pad, mapped key, footswitch or MIDI message for the preset that's already playing now only moves the setlist cursor. Loading a Practice Tool project whose preset is already loaded leaves that preset alone. Choosing the loaded preset again in the library asks before discarding your changes.
* Fixed the Add Effect list jumping to the top-left corner of the window when the signal chain redrew while it was open. It now stays under its + button, or closes if that insertion point no longer exists.
* The Multi-Rig Mixer is now a core feature and on by default: every preset card has an Add to Mixer button, the signal path grows a Mix tab when two or more presets are running, and the preset library has a Multi-Rig tab. It can be switched off under Settings → Feature Toggles.
* The Mix tab's Master Out knob is now the Multi-Rig's own level: a dB gain on the mix of presets, applied before the global output stage, independent of the OUT knob, and saved and recalled with the Multi-Rig. Loading a single preset resets it to 0 dB. The per-mix Limiter switch has moved to Settings → General → Advanced DSP Level Targets, beside the Output Protection Ceiling it aims at — it always applied to the whole output rather than to one mix, and it now persists between sessions instead of being reset by the next preset load. Save/Update and Delete are icon buttons stacked beside the knob. Previously the knob mirrored the global output as a linear percentage that could read 222% and snapped back whenever the global chain was rebuilt.
* Multi-Rig mixes can be saved and recalled as library entries of their own. Each card lists the presets in the mix, can be deleted in place, and is found by the library search box; loading one asks before discarding unsaved preset edits, and updating one keeps its original creation date.
* Preset tags are now shared between the save and publish dialogs, and `bass` was added to the tag list (credit: diego).
* Scene selection is now restored correctly when reopening the plugin editor.

### Library & Resources
* NAM models and IR files can be dragged from your file manager straight onto the app to import them, or dropped onto a NAM or IR Cab effect to load them into that node.
* The resource browser now remembers a separate folder location and navigation list for each effect role, so browsing IR cabs doesn't lose your place in the NAM models.
* Next/previous resource stepping now also walks Tone3000 search results, downloading and importing each model only when you reach it.
* A Tone3000 tone's models are listed alphabetically in the resource browser and the Settings Tone3000 browser, with numbers in their natural order ("Gain 2" before "Gain 10"). Next/previous stepping and whole-tone imports follow the same order.
* Tone3000 tone artwork is shown in the effect visualisation in place of the generic category image, and stored with the imported resource. Existing imports are backfilled as tone listings are fetched.
* Fixed a crash when accessing files whose names contain non-ASCII characters such as an en dash.
* Fixed loading of resources referenced by their normalized path.
* Double-clicking a result in the Resource Library or Folder tab now selects it and closes the browser.
* Fixed composite effects that take both an amp model and a cab IR, such as Supercharged Neural Amp. Choosing one used to overwrite the other, which then showed as a missing resource.
* Tone Sharing search now runs on the server, so it searches all community presets rather than only the pages already loaded. A new tag filter narrows the results, and both apply as you page through them.

### Practice & Jam
* Added the **Practice Tool** to the Jam panel: a backing-track player for learning songs by ear. Open a WAV, AIFF or MP3 file, or drag one in, and change its speed (0.25x–2x) and pitch (±12 semitones) independently, along with volume and balance. Mark named loop sections on the stereo waveform, with song-section name suggestions and an undo for deleted loops. A four-band Backing Track EQ shapes the track without touching your guitar. Projects save the track, its loops, the fader settings and optionally the current preset, so you can pick up where you left off. The track is mixed in after your signal chain, and speed and pitch are processed on a background thread so your guitar signal is never held up. It can be hidden under Settings → Feature Toggles (credit: AriKuorikoski, #39).
* Riff previews now loop seamlessly. The engine loops the clip itself with a crossfade at the join, instead of the UI requesting it again each time. Trim markers and repeat settings can be changed while the preview plays.
* The standalone **metronome** has been rebuilt. It adds time signatures from 2/4 to 13/4, 3/8 to 12/8, and grouped 5/8 and 7/8, plus a per-beat accent pattern you edit by clicking the beats (accent, medium, normal or silent). You can also add subdivisions from eighths to 32nds, including triplets. It has sampled click kits and a live beat display, and the Space bar and the footer's TAP button tap the tempo.
* The Jam player has a favourite button, so you can favourite the track that's playing without leaving the player (credit: belowm, #48).

### Effect Layouts
* The layout button on an effect now opens a picker where you choose between the standard controls and any custom layout, and remember that choice as a rule scoped to every use of the effect, to amps/FX matching a keyword, or to a single preset. A master *Use Effect Layouts* switch turns the whole system off without discarding your rules, and layouts can be created, edited and named from the same popover. (Requires the Effect Layout Editor power feature.)

### MIDI & Automation
* Keyboard shortcuts mapped to automation slots now work whether or not the MIDI & Automation panel is open. Previously they only fired while that panel was visible, which made them unusable in practice.
* The spacebar is never captured by keyboard mappings and always passes through to the host, so DAW transport keeps working while the plugin window has focus.
* Added MIDI/automation slots for direct scene selection (Scene 1–4), so a footswitch can jump straight to a scene. Works with the plugin window closed.
* Fixed setlist presets, bank changes and scene switches from a MIDI footswitch or DAW automation doing nothing in a plugin whose window was closed. They were held until the window was next opened, and now take effect straight away.
* Existing DAW automation lanes no longer rebind to a different parameter after upgrading — the plugin's parameter layout is now append-only, so adding the new scene slots leaves earlier custom slots where they were.
* The plugin now declares that it accepts MIDI input, so hosts offer MIDI routing to it (credit: diego).
* MIDI channels are now consistently displayed as 1–16 (or "any") throughout the UI.
* Right-click a control to **MIDI Learn** it. This works on the IN and OUT knobs, every effect parameter in standard and custom layouts, and the setlist pads, bank arrows and editor slots. A bar shows while it's listening (Cancel or Escape stops it), and a message confirms the mapping it captured. The same menu has Clear Mapping, which removes the MIDI assignment but keeps the slot's keyboard and DAW automation.
* **MIDI Learn for this preset** maps a control only while that preset is active, so one expression pedal can drive a wah in one preset and a volume in another. It's offered on effect parameters and the IN and OUT knobs, but not on setlist pads or bank controls, which pick what plays rather than shape how it sounds. Within that preset it overrides any global mapping on the same MIDI control, and controls the preset doesn't map still follow their global mapping. Per-preset mappings are listed under Preset Mappings in the MIDI & Automation panel, and each preset can have up to 16. They respond to MIDI only, aren't included when a preset is exported or shared, and are removed when the preset is deleted.
* MIDI and DAW automation of effect parameters now covers each parameter's full range. Previously a CC swept a ±24 dB knob between only 0 and 1 dB, so existing automation of effect parameters on custom slots will now play back across the full range.
* Wide frequency controls turn on a musical, logarithmic scale: the Ring Modulator's Frequency, and the Low Cut and High Cut on Digital Delay, IR Cab and Advanced Reverb. Each stretch of the knob covers the same number of octaves, and MIDI and DAW automation follow the same curve, so an expression pedal sweeps them evenly. Automation already recorded on those cuts plays back along the new curve.
* The loaded preset's name is sent to the MIDI controller as SysEx, which Soundshed Go controllers show on their display and other devices ignore. In a plugin, the name is sent while the editor window is open. You can turn this off in the MIDI & Automation panel's Mappings tab. To support it, the plugin now has a MIDI output in every format, and the standalone app lists MIDI output devices.
* Automation slot values are now saved with the DAW project, and restoring them no longer triggers their actions.

### Settings & Storage
* Settings, presets and library data are now kept in a single SQLite store, which multiple plugin instances and the standalone app can read and write at the same time without stepping on each other. Existing files are imported once on first launch and left in place untouched.
* UI storage files (setlists, automation, preset folders/favourites/ratings) are now written atomically, so a crash or power loss mid-write can no longer truncate them.
* All other files the app writes in full are now written atomically too, including presets, library indexes, layouts and extracted factory models. Plugin instances that start together on a fresh profile can no longer read a model while another instance is still extracting it. A factory model that an older version left incomplete is repaired on the next launch.
* Clarified which settings belong where: NAM quality and editor size/zoom now belong to a plugin instance and are saved with the DAW project, shared preferences still sync between instances, and standalone-only settings (last preset, metronome, input mode, global FX) are no longer overwritten by a plugin instance.
* Standalone app: audio and MIDI device settings now live in Settings → Audio Device and MIDI Devices instead of a separate system dialog, so they work on Android as well as desktop. You can pick the driver, devices, input and output channels, sample rate and buffer size, play a test tone, open the driver's control panel, and turn MIDI inputs and the MIDI output on or off. The section also shows a live input meter, the latency the driver reports and a count of audio dropouts. The input mute is now remembered for each input/output pair, and a new pair is muted automatically only when it looks like a built-in microphone playing through speakers. Before this, Android started every launch with the input muted. Device changes are saved immediately rather than when the app exits.
* The plugin editor window now reopens at the size you left it at. Each instance remembers its own size with the DAW project, so it survives closing and reopening the editor as well as reopening the project. Where the window sits on screen stays the host's to remember. A brand-new instance now opens at a size based on your display — up to 1600x1100 and never more than 80% of the screen — instead of a fixed 1200x900, which came up small on high-resolution or scaled displays.

### Performance
* A preset or scene change now sends the UI a small state update instead of the entire app state including the resource library, which was around fifty times larger.
* DSP performance and signal-level telemetry are suppressed while the UI is hidden, and the signal-level messages themselves are far more compact.
* The audio thread does much less work in each block. It no longer allocates memory, works out the chain's processing order only when the chain changes, and updates level meters only as often as they're displayed. A light chain went from 6.65 µs to 2.03 µs per 64-sample block. The Reverb and Parametric EQ also cost less per sample.

### UI & Workflow
* The Global EQ, Parametric EQ and Graphic EQ now show a live spectrum of the incoming audio behind their curves, lined up with the frequency handles, so you can see which frequencies your tone is using while you EQ it. It shows what arrives at the EQ, before any cut or boost, including while the EQ is bypassed. It only runs while an EQ curve is on screen.
* The control bar has an OUT meter beside the OUT knob, matching the IN meter on the other side, so you can see what is leaving the app as well as what is arriving. It reads the mix after master gain and auto-level, which means it follows the OUT knob and drops to silence when the output is muted. Both meters gained a held-peak readout in dB above the ladder — useful resolution the eight segments cannot show, and the only way to see how far past full scale a hot output actually goes.
* Added a **compact layout** for small windows. The signal chain and the effect controls become two tabs, each using the full height, and picking a node switches to its controls. In short, wide windows the navigation moves to a rail down the left edge and the effect header fits on one line, so the knobs stay visible down to the 640x400 minimum window size. Layout Density (Settings → General → UI) is set to Auto, which picks a layout from the window size; choose Compact or Full to override it.
* Effect artwork now fills its panel. Signal chain nodes without their own thumbnail show the same artwork, with the effect's icon on top. In narrow windows the artwork shrinks to a thumbnail beside the effect name, so it no longer pushes the controls down.
* Fixed an empty notification that flashed on every state update and cleared any message already on screen.
* The Help panel has been rewritten, and the startup screen shows only the app logo. NAM FX, Digital Delay, Doubler and the Advanced and Ambient reverbs have their own icons, and the footer uses icons for Setlist, Tuner and the other tools. Knob rings shade along their sweep in the dark and classic themes, and interface text now uses UK spelling throughout.
* Adding an effect to the signal chain now selects it, so its controls and visualisation come up straight away, whether it came from the + button, a drag from the FX Library or a tone group dropped on the chain. In the compact layout it also switches to the effect's controls, as picking a node does.

### Platform & Reliability
* Linux: fixed signal-chain node reordering and drag-and-drop from the FX Library panel, and improved leftward reordering (#27).
* Linux: the standalone app now explains what to install when the WebKit WebView is missing instead of failing to start (#21).
* macOS: the app is no longer sandboxed, so imported file resources are still available after a restart.
* Fixed the effect dropdown being positioned incorrectly at non-default UI zoom levels (#33).
* Fixed text encoding for the plugin UI so non-ASCII characters display correctly (#13).
* Hosted plugin loading now resolves bundle paths consistently across VST3, Audio Unit, LV2 and CLAP on every platform, so picking a file from inside a plugin bundle loads the plugin you intended.
* Improved signal chain drag/drop compatibility checks, and fixed a node reordering case that could break the graph.
* Fixed a crash when the UI sent a message with an unexpected value type. The message is now logged and ignored.
* In a DAW, opening a project no longer marks it as modified. Hosts that snapshot plugin state for undo no longer record the plugin's restore as a new undo step, which used to wipe the host's redo. Signal chain undo now works with hosted plugins and keeps each plugin's current state, although undoing the deletion of a hosted plugin still can't bring its state back.
* Fixed a crash that could happen when a DAW asked for the plugin's state from a background thread, as some hosts do when saving, while a preset was loading or a mixer slot was being removed. The state is now always put together on the plugin's own main thread. If that thread is busy for more than a second, the host gets the most recent state the plugin already had ready rather than being kept waiting.
* Fixed the same kind of crash when a DAW restored the plugin's state, or picked a setlist entry as a program, from a background thread, as AU, AAX and LV2 hosts can when opening a project. Both now happen on the plugin's main thread. If that thread is busy for more than a second, the host is not kept waiting: the restore follows as soon as the thread is free, in the order the host asked, and a save in the meantime keeps the state being restored.
* Fixed a crash that could happen when a DAW read the plugin's parameters, or MIDI was coming in, while the plugin was reloading its automation mappings: when a project opened, when another instance changed the mappings, or at startup. DAW parameters are now read from copies of their own, and the mappings are swapped in without the audio thread ever seeing them half-built.
* CLAP: parameter values a host sets while audio is stopped now take effect straight away, and latency changes are only reported when the host allows it. Both issues were found with clap-validator.
* Windows: the CLAP plugin's UI no longer appears oversized and clipped on a scaled desktop, such as one set to 150% (#38).
* Windows: fixed the installer's component checkboxes not toggling correctly. The installer also won't continue until at least one component is selected (#41).
* Windows: the app's WebView2 data now lives in one folder, %LOCALAPPDATA%\Soundshed Guitar\WebView2, instead of a new temporary folder on every launch. Old folders are cleaned up in the background, and uninstalling removes both.

## 1.5.0 (July 25, 2026)

### Performance, Presets & Settings
* Added a dedicated Setlist performance pads view in the Play tab, with configurable 4, 6, or 8-pad layouts.
* Update setlist management with named banks and per-pad preset assignments for faster live preset switching.
* Standalone app global effects now reliably retain their state, and settings synchronize across multiple plugin instances.

### DSP & MIDI
* Added a Graphic EQ effect with 5- and 10-band Bass, Guitar, and General Purpose profiles.
* Improved MIDI automation for effect bypasses, including reliable Note On/Note Off toggle behavior and effect-type bypass automation.
* Fixed the selected input audio channel being restored incorrectly at startup.

### Library & Tone Sharing
* Resource folder browsing now remains responsive with large folders.
* Tone Sharing now displays download counts and handles expired sign-in sessions more gracefully.

### UI & Workflow
* Reworked the signal-chain interaction with a more compact layout, clearer parallel-routing controls, and node bypass buttons.
* Added collapsible signal-chain and app control areas.
* Improved responsive layouts, including the control bar and footer, and reduced the minimum window size to 640 x 400.
* Added DSP stats to individual effect visualization
* Refined light and classic themes, knob styling, UI scaling, and overall visual consistency.

### Platform Reliability
* macOS fixed app entitlements
* Improved macOS hosted-plugin loading error reporting.
* Improved Linux WebKit view discovery for more reliable standalone startup.

## 1.4.0 (July 03, 2026)

### MIDI & Automation
* Added full MIDI parameter mapping and automation workflows.
* Added direct MIDI setlist bank selection, plus bank up/down behavior fixes.
* UI now updates parameter values immediately when controls change via MIDI.

### DSP & Audio
* Revised NAM calibration support for chained NAMs
* Reviewed NAM calibration handling for stereo vs. mono signal paths and added a post high-pass DC blocker (5 Hz) to match the reference gateway.
* Added low-latency mode for IR Cab and IR Reverb, with low latency now used by default for IR Reverb.
* IR Reverb output balancing now uses L2-normalized gain for more consistent loudness.
* Fixed EQ artifacting when adjusting EQ parameters.
* Improved mono ping-pong delay behavior to preserve a correct mono main path.
* Fixed blend effect behavior and improved live model blend preview.

### Audio Import & Formats
* IR Cab loading, riff import, and demo audio now support AIFF/AIFC and MP3 files in addition to WAV, via a new shared multi-format audio decoder.

### Signal Analyzer
* Added a new Signal Analyzer utility effect for real-time signal level diagnostics.
* Added LUFS loudness measurements to the Signal Analyzer.
* Added bark band perceptual analysis to the Signal Analyzer.
* Signal Analyzer now supports mono/stereo input modes.

### Presets, Library & Resources
* Added a new folder browser with favorites and preview support for local resources.
* Added tagging and filter-by-tag support to the resource and folder browser, plus general UI polish.
* Added an architecture filter and improved navigation for NAM models in the resource browser.
* Added prev/next selectors for stepping through resources without leaving the browser.
* Added a calibration indicator to show when a NAM model includes calibration data.
* Preset import now creates the destination folder when needed.
* Fixed "Save New Preset" flow where a newly created preset could be empty.
* Added library resource delete support.
* Fixed Tone3000 BYOK loading/authorization flow and added favourites support in BYOK mode.
* Improved startup and browsing performance by optimizing resource loading and deferring heavy tone/jam loads at startup.

### Jam, UI & UX
* Jam tab is now enabled with substantial layout and interaction improvements.
* Fixed Jam video playback integration issues including CORS handling, scrolling, touch drag, and WebKit docking behavior.
* Migrated UI icons to SVG assets for cleaner and more consistent cross-platform rendering.
* Expanded and consolidated theme/layout work (including light/classic refinements) and improved general UI consistency.
* Reworked UI assembly/components architecture for a more maintainable and flexible interface foundation.

### Platform & Packaging
* Windows installer now allows choosing a custom install location.

### Stability & Internal Improvements
* Reduced unnecessary UI updates caused by DSP performance stats.
* Fixed live DSP stats updates.

## 1.3.0 (June 16, 2026)

### Neural Amp Modeler (NAM)
* Reworked NAM processing to more closely match the reference implementation. Resampling performance fixes.
* Input leveling from model metadata is now applied for more consistent output levels across model (for first nam in chain)
* Unified and simplified auto-gain calibration.

### IR Cab
* Added **pan** or **L/R split** controls to the IR Cab, enabling true stereo cabinet placement.
* Simplified and corrected IR level normalization

### Signal Chain & Performance
* **Faster preset switching**: presets now load on a background thread, eliminating audio interruptions when changing presets.
* Signal chain is safely paused during preset loading to prevent partial-state processing artifacts.
* New **parallel execution pipeline** for the effects chain with adaptive workload balancing, improving CPU efficiency on multi-core systems.
* Mono/stereo mode is now correctly recalled between sessions.

### EQ
* Low and high shelf filters now expose a **Q / resonance** control for more precise tonal shaping.

### Mixer / Splitter
* Fixed delay option on the Mixer/Splitter node. iF your preset suddenly have a delay it's becuase it wasn't working before, check the mixer node.

### UI / UX
* Overhauled **Tone Sharing and Browser** experience.
* Preset folder rename and delete workflow improvements.
* **Toast notifications**: status messages now appear as floating overlays rather than being embedded in the footer bar.
* Improved inline knob editor keyboard input behavior.
* Light mode styling improvements.
* Add NAM/IR metadata details in resource browser
* Add Favourites toggle for local resources

### Plugin Host
* Prevented a crash that could occur when the plugin attempted to host itself.
* Improved LV2 plugin list handling and support for bundle-hierarchy plugin types.
* Plugin list UI updated with cleaner item selection and keyboard navigation support.
* When running inside a DAW as a plugin, input channel and mono/stereo mode are now correctly delegated to the host.

### Tone 3000 / Sharing
* Improved Tone3000 integration; the library tab (used for advanced/experimental blends) is now **disabled by default** in Settings to avoid confusion.
* Shared preset titles now prefer the original published name.
* Improved resource handling and UI for shared/imported presets.

### Fixes
* Fixed preset pack uninstall.
* Fixed resource naming during import/export publish.
* Preset-level data no longer inadvertently inherits global signal chain settings.
* DSP diagnostic message rate is now debounced to reduce UI thread overhead.

---

## 1.2.0 (June 05, 2026)
* Support for NAM A2 architecture, including option to adjust NAM processing quality (Settings > DSP Performance). 
* Updated Tone 3000 integration, default to A2 nam models
* Mono/Stereo path DSP optimizations
* Combined performance work drops single-thread/core DSP usage from 30% to 5-10% when using mono path with NAM A2 in "slim" (min) quality mode.
* Clap plugin available on all platforms

## 1.1.0 (May 23, 2026)

---

### 🎧 Core DSP & Audio Engine Updates

*   **Full Stereo Signal Path** (`79e039b`): Implemented a full stereo processing signal path, allowing for rich, immersive stereo audio across the entire effects chain.
*   **Upgraded Resampling & Sample Rate Consistency** (`8078a07`, `1022820`, `4a0fa6c`):
    *   Enhanced **Convolution Reverb** (formerly IR Reverb) resampling with windowed sinc interpolation and proper normalization.
    *   Improved resampling pipelines and sample rate consistency across both Convolution Reverb and Neural Amp Modeler (NAM) effects.
*   **Reverb & NAM Optimizations** (`dba4f66`, `43274d0`, `c2cbbd1`):
    *   Optimized reverb DSP for sound quality and lower CPU overhead.
    *   Renamed **IR Reverb** to **Convolution Reverb** for a cleaner and more professional user interface.
    *   Added a mix wet/dry parameter to the Neural Amp Modeler (NAM) effect.
*   **Performance Tuning** (`fe2ff17`, `684c9c5`):
    *   Limited the frame-rate (FPS) of DSP performance metrics updates to reduce UI thread load and overhead.
    *   Enabled the DSP diagnostics signal meter by default to ensure real-time visual feedback is always available.
*   **Input Calibration & Level Management** (`9445b6a`, `85c83a6`, `21be4a1`, `fa0f5b9`, `c9550cc`):
    *   Simplified and streamlined the audio levels and calibration setup across the entire application and signal chain.
    *   Implemented named input level calibrations and target training.
    *   Disabled calibration controls during active training sessions and added a clean deletion mechanism for calibrated profiles.
    *   Removed legacy, redundant calibration code and duplicate UI components.
*   **Hybrid Transpose & Pitch Corrections** (`3c6712e`, `52701f6`, `2b26d6e`, `be89472`):
    *   Added first-phase support for a Hybrid Transpose mechanism.
    *   Resolved an issue where global pitch transpose interfered with preset-specific signal chain transpose parameters.

---

### 🔌 WASM & Hosted Plugin Integrations

*   **WASM Effects Infrastructure** (`e9d3584`, `338202b`, `84891b8`, `8a92aed`):
    *   Implemented the first-draft **WASM effect host**, allowing custom-compiled WASM effects to run directly in the engine.
    *   Added parameter publishing capabilities for WASM effects.
    *   Laid out the API and UI blueprint for generating custom effects.
    *   Added thumbnail/visual support for loaded WASM modules.
*   **Linux LV2 Support** (`dd30e29`, `f43a570`, `5d7b3d0`):
    *   Added native LV2 plugin support on Linux environments.
    *   Fixed multi-plugin instantiation bugs allowing users to run more than one plugin concurrently.
*   **Hosted Plugin State Capture & Diagnostics** (`be5393b`, `af48bbf`, `aea8bf9`, `9dccf4b`):
    *   Implemented a JSON-based debug state capture mechanism for hosted plugins to simplify diagnostic reporting.
    *   Ensured plugin state updates are correctly validated and tracked across platform boundaries.
    *   Made hosted plugin IDs intentional and consistent across platforms (Windows, macOS, Linux).

---

### 🎨 User Interface & Experience (UI/UX)

*   **UI Modernization & Custom Layouts** (`49a571a`, `2c38fad`, `57c577a`, `8ae0491`, `5234a23`):
    *   Polished visual assets and updated modern icons for all effects.
    *   Improved custom effect and visualization layouts, with specific fixes for visual alignment.
    *   Updated primary design aesthetics, control bar, and footer styling.
*   **Interface Zooming** (`3ea39eb`): Implemented native UI zoom controls to support scaling on displays of varying resolutions.
*   **Demo Audio Render Actions** (`3b0b386`, `1022820`):
    *   Added an action context menu to the Demo Audio player.
    *   Allowed direct rendering of demo tracks at a selectable sample rate (kHz) and saved user preferences for future sessions.
*   **Jam & Practice Enhancements** (`67e983b`, `8a9f584`):
    *   Added a **Scales Tab** to the Jam menu to assist with practice and improvisation.
    *   Implemented Jam query caching to make searches instant.
*   **Integration Feature Flags** (`dcda73f`): Introduced runtime application feature flags, enabling users to toggle active external integrations and experimental features on/off.
*   **Preset & Sharing Refinements** (`aa7fa1b`, `5be14df`, `00561b6`):
    *   Added a quick-close button for the Preset view.
    *   Fixed preset z-index stacking to prevent UI overlapping.
    *   Ensured the "Save As" command successfully assigns a unique preset ID.
    *   Improved the handling and parsing of tone sharing web links.
*   **Session Security** (`c9678c4`): Added auto-refresh support for Tone300 sessions to maintain connectivity.

---

### ⚙️ Build System & Platform Support

*   **Windows x86 (32-bit) Support** (`47ffeee`, `f9fbfe2`, `1ee7d96`, `d73754e`):
    *   Added full build and packaging pipeline support for 32-bit Windows systems.
    *   Added comprehensive diagnostic tooling to troubleshoot and validate 32-bit compiler architectures.
*   **Intel IPP DSP Optimizations** (`c92c7ec`, `5cc444b`, `82fe49b`, `65f75fd`, `249f472`):
    *   Integrated and modularized Intel Integrated Performance Primitives (IPP) config inside CMake helpers.
    *   Configured architecture-specific detection for x86 and x64 builds, falling back gracefully to optimized alternates.
*   **macOS & Linux Fixes** (`f009595`, `0cdfd71`, `0e9b583`):
    *   Resolved macOS microphone and input device permission consent bugs.
    *   Compiled native arm64 and x64 executables for Linux environments, standardizing the resulting filenames.
    *   Suppressed excessive and noisy console output in Linux runtime environments.
*   **Codebase Refactoring & Cleanup** (`fd27eee`, `1314cb4`, `52d221c`, `4d77e4b`, `644a190`):
    *   Conducted sweeping TypeScript cleans and architectural refactoring.
    *   Tidied up JUCE plugin wrapper files and unified DSP bypass/passthrough paths.
    *   Added logging adjustments for a "no-debug" clean launch.


## 1.0.3 (March 27, 2026)
- Fixed macOS standalone microphone permissions. In the standalone app, use Settings > Audio Preferences to set inputs.
- Fixed macOS plugin window sizing and remembered standalone window state preferences.
- Temporarily removed the Jam tab on macOS due to a WKWebView YouTube referrer issue.
- Reorganized Settings > DSP performance.
- Added keyboard focus and direct value entry for knobs, plus mouse wheel editing.
