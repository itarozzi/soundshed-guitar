# High-Level Code Review

**Date:** 19 September 2026
**Scope:** Application architecture, complexity, unused code, design boundaries, build reproducibility, and maintainability.

## Executive Summary

The app is functionally well-tested, but architectural complexity is concentrated in the orchestration, UI state, and real-time synchronization layers. The most important issue is a likely audio-dropout path; the remaining findings are primarily maintainability and reproducibility risks.

## Findings

### P1 — Riff operations can block the real-time audio path

`PluginController::ProcessAudio()` uses `try_lock(mDSPMutex)` and returns false—causing the caller to output silence—whenever another thread owns the mutex.

However, several riff operations perform work proportional to the length of a recording while holding that same mutex:

- `HandleSaveRiffTakeRequest()` copies both captured audio vectors.
- `HandleTrimCapturedRiffRequest()` allocates and copies two new vectors, then copies the resulting capture again.
- `HandlePreviewCapturedRiffRequest()` copies the complete capture.

These operations can therefore create audible gaps, particularly for long captures.

Relevant locations:

- `core/src/PluginController.cpp:302`
- `core/src/controller/PluginControllerRiffs.cpp:346`
- `core/src/controller/PluginControllerRiffs.cpp:537`
- `core/src/controller/PluginControllerRiffs.cpp:986`

**Recommendation:** Move riff capture into its own service and publish completed takes as immutable or shared buffers. Swap a pointer under a short lock, then trim, encode, save, and preview outside the DSP mutex. Add a concurrency regression test that processes audio while saving or trimming a long take.

**Status:** Implemented on 19 September 2026. Capture state is now published through shared snapshots, length-dependent work runs outside the DSP mutex, and `MixerInstanceLockTests` covers concurrent audio processing during a two-million-frame trim.

### P1 — The UI dependency graph is cyclic and built around unrestricted global mutation

The accepted cycle baseline currently contains:

- A 9-module `layoutDesigner + signalPath` cycle.
- A 31-module `navigation + presets + toneSharingPanel` cycle.

Examples include:

- `navigation.ts` importing tone sharing while tone-sharing modules import navigation.
- `layoutDesigner.ts` importing signal-path rendering while `signalPath.ts` imports the designer.
- Preset archive code importing tone-sharing features while tone-sharing asset code imports the preset facade.

This is amplified by the mutable `uiState` singleton. At review time, 137 modules imported state-related modules and there were more than 1,100 direct `uiState` property references, including widespread direct assignment.

Relevant locations:

- `core/ui/scripts/cycles-baseline.json`
- `core/ui/ts/state.ts:168`
- `core/ui/ts/navigation.ts:8`
- `core/ui/ts/toneSharingPanel/aiSearch.ts:7`
- `core/ui/ts/layoutDesigner.ts:23`
- `core/ui/ts/signalPath.ts:27`
- `core/ui/ts/presets/archive.ts:10`
- `core/ui/ts/toneSharingPanel/assets.ts:6`

**Recommendation:**

- Give each feature an owned state slice with explicit commands and selectors.
- Move cross-feature workflows into a composition or coordinator module.
- Inject navigation and archive operations as callbacks instead of importing peer features.
- Require the cycle baseline to shrink on relevant changes rather than merely preventing new feature groups.

**Status:** The two cross-feature import cycles were removed on 19 September 2026, and persisted navigation view state is now feature-owned. The four cycles left inside features (two in `presets/`, the signal-path params panel, and tone sharing's browse feed) have since been broken too, by registered redraw hooks (`requestPresetUIRender`, `requestNodeParamsPanel`, `requestBrowseReload`), an injected loader for preset history, and leaf modules for preset filtering and toolbar state. `check-cycles` now compares module by module against an empty baseline, so any new cycle fails, inside a feature or between two.

`uiState` writes are now pinned per field by `scripts/check-state-writes.js`, which runs in `npm run verify` and CI and fails when a module starts writing a field it is not pinned for. Three slices have a single owner with commands. `appSettingsStore.ts` owns app settings, which had 23 writer modules; its `updateAppSetting` records a value and sends it to the engine in one call, and three modules that previously sent without recording now do both. `presetLibraryStore.ts` owns the preset list, cache, filtered view, active and loading ids, and active scene, which had up to 13 writer modules per field. `mixerStore.ts` owns the Multi-Rig mixer, which had 6. Still open: the other fields keep their current writers (84 field–module write pairs across 53 fields, pinned), including preset folders, favourites and ratings, the global chain, UI settings, and composite edit state. Reads still go directly to `uiState`.

### P2 — `PluginController` and `MultiPresetMixer` remain god objects

Splitting `PluginController` across translation units has improved navigation, but it has not established ownership boundaries. At review time, its header exposed approximately 138 handlers and 91 state fields through one class.

`MultiPresetMixer` was approximately 3,233 physical lines with around 104 state fields. It owns preset lifetime, crossfading, tail retirement, worker threads, global chains, node editing, diagnostics, buffering, tuner buffering, and pitch detection.

Relevant locations:

- `core/src/PluginController.h:362`
- `core/src/PluginController.h:838`
- `core/src/dsp/MultiPresetMixer.h:36`
- `core/src/dsp/MultiPresetMixer.h:481`
- `core/src/dsp/MultiPresetMixer.cpp:1982`

**Recommendation:** Extract cohesive services such as:

- `PresetVoicePool` or `PresetSwapManager`
- `GlobalChainEngine`
- `TunerEngine`
- `MixerTelemetry`
- Feature-specific controller services that register their own message handlers

The root controller should compose these services rather than expose every feature's private operations.

**Status:** Tuner capture, analysis, callback dispatch, and worker lifetime have been extracted into `TunerEngine`. Level measurement, diagnostics snapshots, enablement, and oversized-block counts now belong to `MixerTelemetry`. Global-chain normalization and control edits now live in `GlobalChainEditor`, with `MultiPresetMixer` retaining its public API. Preset lifetime is now `PresetVoicePool` (with `PresetInstance` as its own unit): installation, staged swap, replace and add commits, the tail spill and retiree caps, ramp advance, and the audio thread's end-of-block collect with its read-scope handshake. Global-chain executor ownership, rebuild, staging, and swap are `GlobalChainEngine`. Retirement is `DspReaper`, shared by both. `MultiPresetMixer.cpp` is down to 1,766 lines and its header to 564; what remains is the per-block signal flow, the scalar settings, and node-edit routing. There is one intended behaviour change: a staged global-chain swap now seeds this instance's node-type config defaults (NAM quality), as an in-place rebuild always did.

On the controller side, services can now register their own UI messages in a `MessageHandlerRegistry`, which `MessageDispatcher` tries first, inside the same JSON-error guard. `PracticeToolService` (12 messages) and `TunerService` (3) have moved to it, removing 15 `Handle*` forwarding methods and `MessageDispatchPracticeTool.cpp`; 123 handlers remain on the controller, down from 138. Still open: the metronome, the signal test, and the preset, resource and signal-path areas depend on shared controller state (the active preset, settings saves, state broadcasts), so moving them means injecting that state first.

### P2 — Cross-language contracts have multiple manual sources of truth

`MessageDispatcher.cpp` describes itself as the canonical message source, but the UI separately maintains a string-keyed handler table, while the outbound bridge accepts `unknown` payloads.

At review time, there were 157 native dispatch names. Thirteen were not mentioned anywhere in the current TypeScript. Some are documented compatibility aliases, but others appear to be obsolete surface area:

- `getPerformanceStats`
- `getSignalDiagnostics`
- `importToneSharingPack`
- `openAudioPreferences`
- `removeLocalLibraryResource`
- `removePreset`
- `setGlobalChain`
- `setLimiterEnabled`
- `setNodeEnabled`
- `setNodeParam`
- `setTunerEnabled`
- `setTunerReference`
- `splitSignalPathEdge`

Effect identity is similarly duplicated between `EffectGuids.h`, `effectGuids.ts`, and additional fallback alias metadata in `presetV2.ts`.

Relevant locations:

- `core/src/MessageDispatcher.cpp:1`
- `core/ui/ts/messages.ts:35`
- `core/ui/ts/bridge.ts:17`
- `core/src/dsp/EffectGuids.h:1`
- `core/ui/ts/effectGuids.ts:1`
- `core/ui/ts/presetV2.ts:319`

**Recommendation:** Use one machine-readable protocol and effect manifest to generate:

- TypeScript discriminated unions and validators.
- C++ message identifiers and validation helpers.
- Effect GUID and alias constants.
- Protocol documentation or automated parity tests.

**Status:** Partly done on 19 September 2026. `core/protocol/ui-messages.json` lists every message type in both directions. `tools/check-protocol.mjs` runs in `npm run verify` and both structural CI workflows. It reads what each side sends, routes and handles, and fails when the two sides disagree with each other or with the manifest. It also fails on an undocumented type, beyond a pinned backlog of 131 that may only shrink, and on any difference between `EffectGuids.h` and `effectGuids.ts`.

The audit of the 13 native-only names removed ten: `importToneSharingPack`, `openAudioPreferences` (with the JUCE dialog hook behind it), `removeLocalLibraryResource` (which skipped the in-use check), `removePreset`, `setGlobalChain`, `setLimiterEnabled`, `setNodeEnabled`, `setNodeParam`, `setTunerEnabled` and `setTunerReference`. Three are kept and documented: `getPerformanceStats` and `getSignalDiagnostics` as diagnostic pulls the lock tests use, and `splitSignalPathEdge` as an engine capability the UI does not expose yet.

The new check also found drift, now fixed. Five UI handlers served types the engine no longer sends. The `riffLibraryState` reply was ignored. A failed pack-archive delete was never reported. And two sends built JSON by hand instead of going through the bridge.

Still open: generated TypeScript unions and C++ identifiers, payload schemas, and effect-alias parity. The C++ aliases are registered in code a static check cannot read reliably, and the legacy `amp_nam` resolves to the optimized NAM amp in the engine but to the plain one's GUID in the UI.

### P2 — The retired auto-level feature still exists throughout the stack

The UI explicitly says mixer-wide auto-level is retired, and current templates contain no corresponding controls. The native handler always forces the feature off.

Nevertheless, auto-level remains in preset schemas, UI state, messages, mixer state, tests, and an active DSP branch.

Relevant locations:

- `core/ui/ts/controls.ts:931`
- `core/src/controller/PluginControllerGlobalChain.cpp:327`
- `core/src/presets/PresetTypes.h:153`
- `core/src/dsp/MultiPresetMixer.cpp:2556`

**Recommendation:** Keep legacy fields only in deserialization or migration code, where they can be read and discarded. Remove the runtime flags, handlers, inert UI functions, and unreachable DSP processing.

**Status:** Done on 19 September 2026. Gone: the mixer's peak auto input and auto output stages; the flags on the mixer, preset `GlobalSettings` and `GlobalSignalChainConfig`; the `setAutoLevel` message and its `autoLevelChanged` reply; and the UI's inert toggles. Legacy keys are still stripped where they are read (the app-settings migration, NAM node params, archive sanitising), and a stored global chain that still carries them is read with them ignored. `DSPPerformanceTests` lost its auto-level case; the limiter's ceiling tests remain.

### P2 — Several fetched dependencies are not reproducible

`core/CMakeLists.txt` fetches three dependencies from `main` and one from `master`. A clean build can therefore change without a repository commit.

Relevant locations:

- `core/CMakeLists.txt:150`
- `core/CMakeLists.txt:158`
- `core/CMakeLists.txt:167`
- `core/CMakeLists.txt:259`

**Recommendation:** Pin each dependency to an immutable release tag or commit SHA and update them deliberately, ideally through automated dependency-update pull requests.

**Status:** Done on 19 September 2026, except automated update pull requests. `signalsmith_linear`, `signalsmith_stretch`, `stftPitchShift` and `minimp3` are pinned to the commits both build trees were already using. `tools/check-dependency-pins.mjs` fails CI on any branch-name pin, and its `--upstream` flag reports pins with newer commits or tags. There is no update-PR automation: Dependabot does not read CMake FetchContent.

### P3 — Complexity gates currently preserve debt rather than drive it down

The checks pass, but their accepted baselines contain:

- 42 C++ files over the 800-line budget.
- 21 UI files over the 800-line budget.
- Oversized files such as `MultiPresetMixer.cpp`, `PluginControllerResources.cpp`, `resourceBrowser.ts`, and `layoutDesigner.ts`.
- Stale ceilings that allow already-improved files to grow back before CI fails.

C++ effects also lean heavily on header implementations: 46 effect headers versus two `.cpp` files, all pulled through `BuiltinEffects.h`. This increases header coupling and recompilation cost.

Relevant locations:

- `tools/cpp-file-size-allowlist.json`
- `core/ui/scripts/file-size-allowlist.json`
- `core/src/dsp/effects/BuiltinEffects.h:3`

**Recommendation:**

- Tighten allowlists whenever a file improves.
- Require touched oversized files to shrink or extract a coherent unit.
- Move non-template effect implementations and registration into `.cpp` files.

## Recommended Order of Work

1. Remove riff-buffer work from `mDSPMutex`.
2. Break the two UI cycle groups and introduce feature-owned state.
3. Extract tuner, telemetry, global-chain, and preset-lifetime responsibilities from `MultiPresetMixer`.
4. Replace manual protocol and GUID duplication with generated contracts.
5. Delete the retired auto-level path and audit the 13 native-only message names.
6. Pin dependencies and progressively tighten size baselines.

## Verification Performed

- UI verification passed: typecheck, lint, 38 test files, and 401 tests.
- The native Debug build succeeded.
- All 55 non-benchmark native tests passed.
- The build emitted PCH-definition and numeric-conversion warnings around `NamResamplerResetTests`; these should be cleaned up so new warnings remain visible.
- The review was read-only; no application source changes were made as part of it.

## Review Caveat

A concurrent `BlendSupport` extraction appeared in the worktree during the review. It was treated as in-progress user work and excluded from the conclusions above. The recorded build and test results may not include the final moments of that concurrent edit.
