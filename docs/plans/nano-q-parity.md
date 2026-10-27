# Nano Q: the road to parity with the web UI

Status: **plan, 2026-10-27.** Audit taken at `b47bca5c` (Nano Q resizable, with the web UI's
full layout). Follows [nano-q.md](nano-q.md), whose Progress table records what is done.

Nano Q now draws both of the web UI's layouts (compact and full), so the question is no longer
"what does a small-screen player need" (the scope [native-ui.md](native-ui.md) set for Nano) but
"what would stop someone using Nano Q in place of Soundshed Guitar". This plan lists what is
missing, sorts it into phases, and says where each piece of logic should live.

## Where things stand

| | Count |
|---|---|
| UI→engine message types the engine routes | 191 (`core/protocol/ui-messages.json`) |
| ... that `uiclient/UiCommands` sends | 58 |
| Engine→UI message types | 126 |
| ... that `UiClient` parses into `ClientState` | 52 |

Nano Q covers the Play surface: the chain, the effect editor's generic controls, model and IR
slots, the resource chooser (library, folder, Tone3000), presets (search, favourites, recents),
setlist stepping, scenes, the global chain, tuner, metronome, demo clips, chain undo and A/B,
Tones browse and install, a settings subset, both layouts and zoom. What it lacks, by area:

- **Input.** No keyboard handling at all (`nanoq/src/ui` has no key handlers): no Ctrl+Z, no
  Space to bypass or tap, no Delete, no Esc, no arrow keys. Knobs have no double-click reset,
  typed value or wheel setting. Chain cards cannot be dragged.
- **Effect editor.** Special sections: EQ curve and spectrum, Graphic EQ bands, cab response
  (Export as IR, Match an IR), IR alignment, the IR list between IR Cabinet slots, Gate Learn,
  spatial panner, Signal Analyzer, mixer-node inputs, Pickup Match curve, blend-node controls,
  hosted-plugin list and Open Plugin, DSP status tiles, channel mode, device artwork, custom
  layouts and the layout switch, deleting an effect preset.
- **Presets.** Folder editing, tags, ratings, the save/edit dialog's details, import/export,
  preview, setlist editing and the pads view, Multi-Rig.
- **Resources.** Favourites, creator and architecture filters, tags, pack grouping, details,
  edit, delete with usage, Tone3000 previews, Import All, Tone3000 favourites, the remembered
  ◀▶ list per slot.
- **Live use.** MIDI learn and mapping, keyboard mappings, the MIDI log, MIDI setlist output,
  input levelling profiles and training, Pickup Match (global), the in-app audio and MIDI
  device page (Nano Q shows clap-wrapper's own window), signal path test, the metronome's pan,
  grouping, click kit and accent grid.
- **App.** Settings sections (DSP levels, Tone3000 key and mode, library export, updates,
  density, wheel, signal-flow animation, DSP Performance page, Features), update check, Help,
  deep links, shared-sync catch-up, the output DSP status flyout, feature flags.
- **Creation tools.** Riff capture and library, looper, Jam, Tones publishing and My Content,
  AI Tone Builder, blend editor and manager, composites, custom effect designer, layout
  designer.

## How each piece of work is split

Every phase below has the same three layers, done in this order:

1. **Engine**, when the web UI keeps state or edits a persisted document in TypeScript. The
   engine gets a command, and the web UI moves onto it in the same change, deleting its
   TypeScript (the drift policy from native-ui.md). Examples: preset folder edits, setlist
   edits, archive export, EQ responses.
2. **`core/src/uiclient`**: the sender in `UiCommands`, the parser into `ClientState`, and any
   presentation logic (filters, layout, formatting) as JUCE-free C++ with unit tests. Keeping it
   in core keeps it testable without a UI. Where the web UI keeps its own TypeScript
   copy of presentation logic (as with `ChainHistory` today), the two share test vectors.
3. **Nano Q**, in `nanoq/src/ui`, checked against the web UI at the same size (DevDriver
   `shot` against CDP `Page.captureScreenshot`, as in nano-q.md).

`tools/check-protocol.mjs` scans `uiclient` and `juce/source/{nativeui,editor}` but not
`nanoq/src`; add it in phase 1 so a message Nano Q sends outside `UiCommands` is caught.

### Two shared-state traps to fix first

- **UI settings.** `UiCommands::PatchNativeUiSettings` writes only the `native` part of
  `uiSettings`. Web-only keys that should be shared (`fxFavorites`, `fxRecents`,
  `signalPathWrap`, the effect-layout rules, the per-slot resource navigation lists) cannot be
  written from a native UI. Either move each into an engine-owned app setting (preferred: the
  engine already owns favourites and recents for presets) or give `UiCommands` a patch for
  named shared keys.
- **Feature flags.** The web UI gates a dozen features on `features.<id>.enabled`. Nano Q
  ignores them. Read them through `ClientState::appSettings` and honour the same defaults, so
  a feature off in Soundshed Guitar is off in Nano Q.

## Progress

| Phase | State |
|---|---|
| 1 | **Done** (2026-10-27, d5aa16b1..56c52111). The keyboard (`ui/KeyRouter.h`, `ui/Keys.cpp`: Ctrl+Z, Ctrl+Shift+Z and Ctrl+Y, Space to bypass the card clicked last or tap the tempo, Delete, F2, Esc; dialogs and drop-downs are routed too, and Esc closes one from inside its text box); a typed value from a double click on a knob's value; the wheel following `ui.mouseWheelAdjustsControls`; dragging cards by the web UI's rules (`uiclient/ChainDropRules`, `UiCommands::MoveNodeToEdge`), across lines, into branches, and the bypass flick; the chooser's Favourites and Recent (`uiclient/FxMarks`, through `UiCommands::PatchUiSettings`, the fix for the UI settings trap) and its star; the feature switches (`uiclient/FeatureFlags`: Tones, Tone 3000, riffs, factory presets); the Channels choice; deleting your own effect presets; the plugin-host block (list, favourites, Open Plugin, Browse, loading state: inert until core can host plugins without JUCE); the blend block (name, model count, Blend Mode override); the drag bar between chain and editor (`native.chainAreaHeight`); the preset trail in uiclient (`PresetTrail`, ten kept as in the web UI); the effect's artwork in the full layout's editor. Windows standalone: a sizing frame and maximise box (clap-wrapper dropped them: QPlug answers `can_resize` before the view exists) and a caption coloured from the theme (`WindowChrome.cpp`). The dev driver gained `key` and `type`. **Moved on:** the Graphic EQ and mixer-input panels go with phase 4's special sections; the blend knobs and matched-model line with phase 4 (the engine should report the model playing); icon-only chain cards, and the NAM badge for a model whose row lacks `architectureVersion` (the engine should report it), to phase 6; hosting plugins without JUCE is a phase of its own. Not checked live: the blend block (no blends in the test profile) |

## Phases

Sizes are rough, for one engineer who knows the codebase: S under a week, M one to two weeks,
L three or more.

### Phase 1: editing at full speed (M)

The things a user hits in the first minute.

- **Keyboard.** Elements' key events into `Shell`: Ctrl/Cmd+Z, Ctrl+Shift+Z and Ctrl+Y for
  `ChainHistory`; Space bypasses the selected node when the chain has focus, otherwise taps
  tempo; Delete removes the selected node (or collapses a split); Esc closes the top sheet,
  menu or dialog; arrow keys on tabs, the chooser and lists; F2 renames a scene. All
  suppressed in a text field.
- **Knob gestures.** Double-click resets to the default; double-click the value to type it;
  the wheel when the shared `ui.mouseWheelAdjustsControls` is on; Shift for fine moves.
- **Drag in the chain.** Drag a card to reorder, across wrapped lines and into branches.
  `ChainDropRules` in uiclient (port of `signalPathDropTargets.ts` and
  `signalPath/chainRules.ts`, shared vectors), then pointer drag in `ChainStrip`. Vertical
  flick to bypass on touch.
- **Effect chooser favourites and recents** (after the UI settings trap above), and Replace
  mode marking the current effect.
- **Effect editor leftovers that need no new engine work:** Graphic EQ bands with typed gain and
  frequency; the mixer node's per-input level, pan, delay and mute; Pitch Shift's range and
  snap (check `ParamFormat` covers it); delete an effect preset (`deleteEffectPreset`);
  channel mode (`setSignalPathNodeChannelMode`, behind the shared setting); device artwork
  (equipment art and Tone3000 capture art over the gradient from
  `effect-presentation.json`); the NAM badge for a model whose library row lacks
  `architectureVersion`.
- **Hosted plugin nodes:** the searchable plugin list, Open Plugin, the load spinner
  (`hostedPluginResourceLoadCompleted`, never parsed today, so a pending load never clears),
  `nodeResourceBrowseCancelled`.
- **Blend nodes, play side only:** parse `state.blendLibrary`; Blend knobs or sweep, the
  matched model, Blend Mode override (`updateSignalPathNodeConfig blendModeOverride`). The
  model choice mirrors `blendUtils.ts selectBlendMix`; better, have the engine report the
  selected model in node telemetry so neither UI computes it.
- **The splitter between chain and editor** in the full layout.
- **Preset back and forward** moves into uiclient (Nano Q's `StepPresetTrail` in
  `WideFrame.cpp` is the third copy of `presets/history.ts`).

Done when: a chain can be built, reordered, edited and undone without the mouse leaving
Nano Q or the keyboard being ignored, and the web UI's Delete, Space and undo shortcuts all
work the same way.

### Phase 2: presets, folders and setlists (L)

The library is where Nano Q is furthest behind for someone who keeps more than a handful of
presets.

- **Engine-owned folder edits.** `presets/folders.ts`, `folderControls.ts` and `folderDrag.ts`
  edit the whole tree in TypeScript and send it with `setPresetFolders`. Replace with engine
  commands (create, rename, move, delete a folder; add or remove a preset), as scenes and new
  presets were moved in 2026-09. Web UI onto them in the same change.
- **Engine-owned setlist edits** likewise (`presets/setlists.ts:118-251`: create, rename,
  bank, assign a slot, move, remove, delete), replacing whole-document `setSetlists`.
- **Ratings** (`setPresetRating`), and **tags** as a shared data file (the vocabulary is in
  `presetTags.ts` and again in `uiclient/ToneSharing.h`).
- **Nano Q's preset chooser** as the web UI's: Recents (4), Favourites, All, the folder tree
  with counts, nesting by drag, search and tag chips, ratings, folder context menu. The
  Presets page drops its older look.
- **Save and edit dialog:** folder, name, category, description, tags; Advanced with the
  input peak ("Apply Current").
- **Archive export moves into the engine.** The web UI builds `.soundshed.preset(s)` and
  library zips with JSZip (`presets/archive.ts`, `settings/libraryExport.ts`,
  `presets/folderArchive.ts`); the engine already parses them. Add writing, move the web UI
  onto it, then Nano Q gets Export preset, Export folder, Import, and library export.
- **Tone Sharing installs in the web UI move onto `installPresetArchives`** and
  `deleteInstalledPresetArchive`, the clearest drift-policy breach left (the web UI still
  unzips in TypeScript, `presets/archive.ts:945`, `toneSharingPanel/installedPacks.ts`).
  **Done 2026-10-27** (`toneSharingPanel/install.ts`): the web UI fetches the Tone3000 models
  an archive names, then sends the archives whole (`allowMissingRemote`, `fetchedResources`).
  A pack or preset dropped as a file still imports in TypeScript (`importPresetArchive`).
- **Preview a preset** (`previewPreset` and restore), used by Tones too.
- **Setlist editing and the pads view** (4/6/8 pads, banks, hold to assign, drop onto a pad).
- **File drop**, if Elements' hosts deliver it (spike first; the Win32 and Cocoa hosts are
  the ones that matter): a pack, a preset, a `.nam` or IR onto the chain.

Done when: every preset-library operation in the web UI's chooser and save dialog can be done
in Nano Q, and the web UI no longer edits folders, setlists or installs in TypeScript.

### Phase 3: the resource library (M)

- **`ResourceIndex` in uiclient**, as native-ui.md planned and never built: dedup
  (`resourceDedup.ts`), filters and facets (category, A1/A2, creator, tags, favourites),
  sort, search, pack grouping (`libraryPacks.ts`), and the previous/next list per context
  (`resourceBrowser.ts:2011-2100`). Nano Q's own filter and sort in `ResourceBrowser.cpp`
  are replaced by it.
- **Resource favourites** (`setResourceFavorite`), **details**, **edit** name, category and
  tags (`updateLibraryResource`), **delete** with the usage check (`queryResourceUsage`,
  `deleteLibraryResource`).
- **Tone3000:** previews (`previewRemoteResource`, `cancelPreviewResource`, rather than
  importing to audition), Import All, Tone3000 favourites with the user's own key, "open this
  model's tone".
- **The remembered ◀▶ list per slot** (library, folder or Tone3000 result set) in a shared
  setting, so stepping a slot in one product steps the same list in the other.
- **The IR Cabinet's IR list** between its slots.
- **Resource usage and cleanup** move into the engine (`settings/resourceUsage.ts` and
  `libraryCleanup.ts` compute in TypeScript what the engine's `queryResourceUsage` could).

Done when: Nano Q's chooser has the web UI's Library, Folder and Tone 3000 tabs with every
filter and action, and both products step a slot through the same list.

### Phase 4: visualisations and analysis (L)

Needs a plot widget for Elements (curves, a log-frequency axis, draggable handles, a
spectrum fill) used by every item below, and an eye on UI render cost (see the UI
performance review: 20 Hz redraws were the expensive part in the web UI).

- **Engine EQ responses.** `GetFrequencyResponse` for the parametric and graphic EQs (only
  Simple Cab and Pickup Match implement it today). The web UI moves its EQ curves onto
  `getEffectResponse` and `eqPlot.ts:288-385` (fixed at 44.1 kHz) is deleted: native-ui.md
  phase 5, still open.
- **uiclient feeds:** `setSpectrumWatch`/`sldS`, `setLoudnessWatch`/`sldL`, `sldA`,
  `spatialPosition`, `getEffectResponse`/`effectResponse` (coalesced as `effectResponse.ts`
  does), `gateLearn`/`gateLearnStatus`, `analyzeIrAlignment`, `matchSimpleCabToIr`,
  `exportEffectAsIr`.
- **Nano Q:** the parametric EQ curve with handles over the live spectrum, in the effect
  editor and the Global EQ dialog; Simple Cabinet response, Export as IR, Match an IR; IR
  alignment (waveforms, offset strip, Auto Align, nudge, invert); Gate Learn, in the node and
  the global gate's settings flyout; the spatial panner; the Signal Analyzer node; Pickup Match
  curves; DSP status tiles in the effect header and the output flyout; the cables' signal-flow
  animation.

Done when: every effect with a special section in the web UI has it in Nano Q, and no EQ maths
is left in TypeScript.

### Phase 5: the live rig (L)

- **MIDI and automation in uiclient:** parse `automation` (slots into `ClientState`) and
  `midiLearnCapture`, `midiLog`; send `getAutomation`, `setAutomationSlot`,
  `removeAutomationSlot`, `setAutomationValue`, `armMidiLearn`, `cancelMidiLearn`,
  `setMidiLogEnabled`. Move slot address resolution and labels from `midiMapping.ts` into
  uiclient.
- **Keyboard mappings** are run by the web UI (`automationPanel.ts:727-792`). Move running
  them into the engine (the host passes key events) or into a uiclient `KeyboardAutomation`;
  either way one implementation.
- **Nano Q:** right-click and long-press on any mappable control (MIDI Learn, for this preset,
  Cancel, Clear); the MIDI & Automation dialog (Mappings, Keyboard, MIDI Log; setlist program
  change and SysEx name options).
- **Multi-Rig:** parse the mixer's per-slot mix, pan, mute, solo and the composite preset
  list; tabs over the chain, the inline mixer, add a preset to the mix, save and load
  Multi-Rigs.
- **The audio and MIDI device page** in the app, on `audioDevice` (uiclient already handles
  it; the removed JUCE Nano had one, `juce/source/nativeui` before 2026-10-27, worth reading), replacing clap-wrapper's Alt+Space window in the standalone;
  Run Signal Path Test.
- **Input levelling:** profiles, training and the trim maths live in
  `settings/inputCalibration.ts`; move them into the engine, then the flyout and the training
  dialog in Nano Q. Global Pickup Match with My Guitars.
- **Metronome remainder:** pan, grouping, click kit, the accent grid (`setMetronome` fields
  uiclient does not send yet).
- **Touch:** long press everywhere the web UI has a context menu (iOS already has a long
  press in the simulator); the on-screen keyboard for text fields (nano-q-ios.md).

Done when: a MIDI foot controller can be mapped, a Multi-Rig built and played, and the
standalone set up, all without opening Soundshed Guitar.

### Phase 6: settings, app shell and Tones (M)

- **Settings** to match the web UI's sections: General (preset switch tails, output ceiling,
  limiter, mono output unity, NAM auto calibration, interface level), Tone3000 (relay or own
  key, health check), Library (export), Software Updates (version from `appInfo`, check on
  startup), UI (density, wheel, signal flow), DSP Performance (load, latency, multi-core,
  fast NAM, quality, oversampling, AA filter, signal diagnostics, per-node list), Features
  (flags), Help.
- **Update check** through the shared HTTP layer (the web UI's `updateCheck.ts` is TS-only;
  move it into uiclient and have the web UI keep or drop its copy).
- **Shared-sync catch-up** (`getSharedSyncState`/`sharedSyncState`), so an editor that was
  closed sees what changed.
- **Tones:** preview a shared preset before installing (phase 2's `previewPreset`),
  favourites, ratings and votes, the share link and pack view, deep links
  (`navigateToToneSharingDeepLink` and the `soundshed://` scheme).
- **Zoom range** to the web UI's 75 to 200% (Nano Q's is 70 to 150%), if the plugin state
  allows.

### Phase 7: creation tools (scope to decide)

These are large, mostly behind feature flags that are off by default, and some cannot be
ported as they stand. Recommendation per item:

| Feature | Recommendation | Why |
|---|---|---|
| Custom effect layouts (rendering) | **Port** | A user who picked a layout in Soundshed Guitar sees it in Nano Q. Needs the layout library in uiclient, image knobs and filmstrips in Artist |
| Riff capture and library | **Port** | Engine-driven already (all `riff*` messages); needs a waveform widget, which the looper shares |
| Looper | **Port after riffs** | Engine-driven (14 commands, 5 replies); flag off by default |
| Tones publishing, My Content, packs | Port later | All HTTP; ToneService grows publish and profile calls; archive writing comes from phase 2 |
| Blend editor and manager | Keep in Soundshed Guitar for now | Authoring tool; big UI |
| Composite editor, custom effect designer, layout designer | Keep in Soundshed Guitar | Authoring tools, flags off by default |
| AI Tone Builder | Keep in Soundshed Guitar | Flag off; the whole flow is TypeScript against a service; a port means moving it to core first |
| Jam (YouTube, Scales) | Out | Both are web content (YouTube player, an iframe); Nano Q has no web view |
| Practice Tool | Port only if Jam is wanted without the web parts | Engine-driven (12 commands) |

"Keep in Soundshed Guitar" means Nano Q shows the results (a blend plays, a composite runs, a
layout renders) but authoring stays in the web UI, and Nano Q says so where the web UI would
offer the tool.

## Order and why

1. **Phase 1** first: cheap, and the gaps are the ones a user notices at once.
2. **Phase 2** next: it carries the most engine work that also improves Soundshed Guitar
   (folders, setlists, archive writing, tone sharing installs), and it unblocks phase 6's Tones
   work.
3. **Phases 3 and 4** can run side by side: they touch different files (resource chooser vs
   effect editor) and different engine areas.
4. **Phase 5** after 1, which gives the context-menu and keyboard plumbing MIDI learn needs.
5. **Phase 6** any time after 2.
6. **Phase 7** once its scope is decided.

## Risks

- **Platforms.** The JUCE Nano, removed on 2026-10-27, was the planned native app for
  Android and for Linux boards with small screens. Nano Q has no Android build (Elements has
  no Android host) and its Linux build is untested, so Android stays on the WebView app until
  one of those is done.
- **Elements gaps.** File drop, keyboard focus, accessibility and an on-screen keyboard are
  host-level; each may need a patch to Elements (PRs upstream are fine, as with Artist's
  shadow fix). Spike file drop and focus at the start of phases 1 and 2.
- **Drift during the work.** The web UI keeps moving. Re-run the audit (the message counts
  above and `check-protocol.mjs`) at the start of each phase.
- **Engine moves break the web UI.** Each engine-owned edit changes the web UI in the same
  change; the web verify and smoke test have to pass at every phase, not only Nano Q's.
- **Render cost.** Phase 4's live curves and the signal-flow animation are the heaviest
  drawing Nano Q will do; measure CPU per the UI render cost notes before calling them done.
