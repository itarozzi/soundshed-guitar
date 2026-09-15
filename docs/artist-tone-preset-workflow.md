# Agent guide: research and create artist-inspired presets

Use this workflow when a user asks for artist tones in the **running Soundshed Guitar app**, with Tone3000 or local NAM/IR resources and a review folder. This is a content-authoring task; it normally requires no application code changes, rebuild, or commit.

Read [agent-quickstart.md](agent-quickstart.md) and [the live-app control guide](../tools/agent-ui-debug/README.md) first. The contracts below were checked against the repository on 2026-09-09. Recheck their source if the app has changed.

## 1. Establish scope and protect existing work

- Use artists requested by the user. For an open-ended request, choose a varied, bounded batch: eight artists with two scenes each worked well. On “do more,” add new artists to the existing review folder without replacing its members.
- Keep research notes, intended preset JSON, before-state, and verification results in a task output directory. Use stable new preset IDs within the run so retries cannot create duplicates. Check for existing IDs before saving.
- Snapshot the current preset, active scene, dirty state, mixer slots, and folder tree. Preserve unsaved edits before loading anything else. An active multi-rig, archive session, or another agent restarting the app needs coordination before mutation.
- Treat the user’s request as authorization to research, download needed resources, create presets, and organize the requested folder. It does not authorize public publishing, purchases, deleting unrelated presets, overwriting user edits, or committing source changes.
- Do not use the cleanup instruction from the UI testing guide to delete these presets: these are requested deliverables, not throwaway tests. Leave the app available for review.

## 2. Connect to the actual app

Soundshed’s native window hosts WebView2. A normal Chrome/Edge tab cannot reach its backend. Use the repository’s CDP helper or an equivalent small Node client against the app’s own WebView2 remote-debugging endpoint.

1. Reuse the running app if it has remote debugging enabled. Locate only Soundshed’s process and its WebView2 command line; avoid dumping unrelated process details.
2. Read `http://127.0.0.1:<port>/json/list`. Select a **page** titled `Soundshed Guitar` with a `https://juce.backend/` URL. Do not assume the first target is correct.
3. If no usable app exists, launch the existing Standalone executable with an unused local port:

   ```powershell
   $env:WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS='--remote-debugging-port=9355'
   Start-Process -FilePath 'C:/path/to/Soundshed Guitar.exe' -WindowStyle Hidden
   ```

   Use a visible window only when the user needs it for interaction. Follow the environment’s approval rules for launching and accessing the user profile. Do not start another instance over an existing unsaved session.
4. Get a fresh websocket URL after a restart. If the target is `about:blank`, wait for startup and inspect again. A relative import error there is a connection/page-state problem, not proof that an application module is missing. Only recover navigation to the app URL after confirming this is the Soundshed WebView and the current build serves that URL.
5. A custom CDP client should await `Runtime.evaluate` with `awaitPromise: true`, use `returnByValue: true`, reject on `exceptionDetails`, impose timeouts, and close the websocket when done. Place substantial JavaScript in files to avoid PowerShell quoting errors. A hidden WebView may throttle JavaScript timers; use wall-clock deadlines rather than assuming a 100 ms polling timer actually runs every 100 ms.

Example invocation using a freshly discovered websocket:

```powershell
node tools/agent-ui-debug/cdp-tool.mjs $artistWs --eval "document.title"
```

If the connection repeatedly disappears, coordinate with the user or concurrent build work. Continue research and offline authoring while waiting; do not repeatedly kill or relaunch their application.

## 3. Inspect runtime resources and effect definitions

Evaluate inside the app page:

```javascript
const { uiState } = await import('./dist/state.js');
const { EffectTypeRegistry } = await import('./dist/presetV2.js');
const inventory = {
  resources: uiState.resourceLibrary,
  effects: EffectTypeRegistry.getAll(),
  folders: uiState.presetFolders,
  activePreset: uiState.activePresetSnapshot,
  sceneId: uiState.activePresetSceneId,
  dirty: uiState.presetDirty,
  mixer: uiState.mixer,
  archiveSession: uiState.presetArchiveSession,
};
```

The current build serves modules under `/dist/`, **not `/js/`**. Inspect `document.scripts` if uncertain. Save the selected inventory locally; printing the full library can produce hundreds of thousands of tokens. Never include `appSettings`, session tokens, or authentication headers in an inventory dump.

For each candidate, examine `id`, `name`, `category`, `fileMissing`, `description`, and `metadata`. Keep provenance such as `toneId`, `modelId`, creator and source URL. Prefer installed resources with valid files. Do not infer validity from a recognizable name: stale factory archive aliases can point to missing files.

Use the runtime registry’s canonical effect UUID and parameter definitions. Initialize defaults, then validate every override against its actual key and range. Examples of easy mistakes:

| Effect | Parameter detail |
| --- | --- |
| Noise Gate | `thresholdDb`, not `threshold` |
| VCA Compressor | `threshold`, `attack`, `release`, `makeup` |
| Digital Delay | `time` in milliseconds; `mix` and `feedback` are fractions |
| Chorus | `depth` is milliseconds, not a 0–1 amount |
| Room Reverb | `decay` is normalized, not seconds |
| NAM | `inputGain` and `outputGain` are dB; tone controls are dB |
| Input / Output | `gainDb` |

## 4. Research dated rigs and make explicit substitutions

Start at [Pedalboard Archive](https://pedalboardarchive.com/). Read the individual dated board, its routing/evidence notes, and its cited original sources when more detail is needed. If opening a page fails, try a search restricted to its exact artist/path; do not treat an error page as evidence.

Record a short evidence table for each artist:

| Item | Record |
| --- | --- |
| Context | Artist, year, tour/session, source URL |
| Gear and routing | Documented effects, loop/front-of-amp placement, serial vs alternate paths |
| Evidence limit | Source-described order, reconstructed order, missing settings |
| Soundshed mapping | Exact capture, related capture, built-in approximation, or omission |
| Performance | Pickup suggestion, guitar-volume response, tuning, manual controls |

Separate documented facts from authored choices. “Used a TS10” does not document delay time, drive level, pickup choice or every pedal being on together. A touring inventory is not a song patch. Do not claim an exact replica or measured artist settings.

Useful lessons from the first batches:

- Hendrix’s Woodstock rig excludes Octavia; do not add it merely because it is associated with him elsewhere.
- Hetfield’s clean and dirty processors are alternatives. Represent them as scenes, not serial amps.
- Morello’s board belongs in the amp loop. Putting effects after a full amp NAM but before the cabinet only approximates that loop: the NAM already includes power-amp response.
- Summers’ documented Walking on the Moon modulation is Electric Mistress flanging, not just generic chorus.
- A manual wah or Whammy performance cannot be recreated by silently substituting an auto-wah or a fixed octave. For a wah, use the Wah effect with the nearest factory voicing and leave Pedal Position for the player to map, or fix it only for a documented cocked-wah tone. Label fixed settings and omitted performance controls.

## 5. Search and import Tone3000 through the app

Use the app’s authenticated client; never extract credentials or bypass its session flow. Relevant modules are `tone3000.js`, `tone3000Api.js`, and `tone3000Shared.js`.

```javascript
const auth = await import('./dist/tone3000.js');
const api = await import('./dist/tone3000Api.js');
const shared = await import('./dist/tone3000Shared.js');
const params = new URLSearchParams({
  query: 'Hiwatt DR103', page: '1', page_size: '5', sort: 'best-match',
});
const response = await auth.tone3000AuthenticatedFetch(
  api.buildTone3000SearchUrl(params)
);
if (!response.ok) throw new Error(`Tone search: ${response.status}`);
const tones = api.extractTone3000Tones(await response.json());
// Select an inspected tone, then inspect its model names and descriptions.
const models = await shared.fetchTone3000Models(selectedTone);
```

Do not take the first fuzzy search result: “Dover Drive” returned unrelated Dover amplifiers; “SansAmp” returned bass pedals. Prefer the correct equipment type, gain setting, capture scope and calibration over keyword similarity. Inspect pagination when a pack exceeds the helper’s first 100 models; use `buildTone3000ModelsUrl(toneId, page, pageSize)` with the authenticated client for later pages.

Import only the chosen models. Some current search results expose `format: 'ir'` instead of `platform`. The import helper determines resource type from `platform`, so normalize it deliberately:

```javascript
const format = selectedTone.platform ?? selectedTone.format;
if (!['nam', 'ir'].includes(format)) throw new Error('Inspect unsupported format');
const result = await shared.importTone3000Models(
  { ...selectedTone, platform: format }, selectedModels
);
```

Then wait until every returned resource ID appears under the correct `uiState.resourceLibrary` type with `fileMissing === false`. A returned ID is not by itself a persistence or load check. On an interrupted call, inspect which IDs already exist before retrying imports.

### Capture and cabinet compatibility

- **Pedal NAM:** use Neural FX (NAM), then an appropriate amp and cabinet chain.
- **Amp-only NAM:** use Neural Amp (NAM), then a cabinet IR.
- **Full rig / amp-cab NAM:** generally omit the additional cabinet.
- **Preamp-only NAM:** explicitly account for power-amp/cabinet response or document the approximation.
- IRs can be cabinets, rooms or reverbs. Check content type and intended use, not only `.wav`.
- Do not classify solely from a model suffix such as `CAB`: title, gear metadata, description and actual model metadata can resolve misleading names. The Two-Rock SSS “BAL DI” pack, for example, describes an external IR despite a model name ending in `CAB`.
- Keep the user’s interface calibration intact. `useCalibration` controls NAM metadata calibration; it does not replace correct input setup or justify arbitrary global gain changes.

## 6. Author scenes, validate, then save

Construct Preset V2 with a stable new `id`, `version: 2`, `name`, `category`, `description`, `tags`, and `scenes`. Each scene has an ID, title and `graph`. Nodes need unique IDs, an effect `type`, a label, parameters, and any resources. Boundary nodes use `__input__` and `__output__` and types `input` and `output`.

```javascript
const resource = { resourceType: 'nam', resourceId: inspectedModelId };
const node = {
  id: crypto.randomUUID(), type: inspectedEffect.type,
  category: inspectedEffect.category, label: 'Descriptive model label',
  params: validatedParameters, resources: [resource],
};
// Serial example: Input -> pedal -> amp -> cab -> delay -> Output.
const edges = nodes.slice(1).map((node, i) => ({
  from: nodes[i].id, to: node.id,
}));
```

Verify unique node IDs, valid endpoints, a connected input-to-output path, no unintended orphan nodes, no cycles, valid parameter ranges, and all required resources. Build only intended branches; separate scenes are often clearer than complex switching graphs.

Save through the native bridge, not direct database writes:

```javascript
const { postMessage } = await import('./dist/bridge.js');
postMessage({
  type: 'savePreset', saveMode: 'save-as', requireNewPresetId: true,
  presetId: preset.id, name: preset.name, category: preset.category,
  description: preset.description, sceneId: preset.scenes[0].id,
  includeGlobalSignalChain: false, preset,
});
```

Wait for completion and fetch it back with
`requestPresetFromBackend(preset.id)` from `./dist/presets/fetch.js`.
For a deliberate update to a preset created in the current task, keep its ID and use `saveMode: 'update'`. Never re-run a creation script blindly after an uncertain outcome.

**Saving is not loading.** The current backend can re-key the running mixer slot on save without applying the supplied graph. To audition or test, fetch the persisted preset and send the entire object:

```javascript
const { requestPresetFromBackend } = await import('./dist/presets/fetch.js');
const saved = await requestPresetFromBackend(preset.id);
postMessage({
  type: 'loadPreset', presetId: saved.id, preset: saved,
  sceneId: saved.scenes[0].id,
});
```

An ID-only `loadPreset` message is insufficient for this handler. Wait until the backend-reported active preset and scene match, loading has finished, and missing-resource status has arrived. A save notification, selected card, or optimistic client snapshot is not DSP verification.

## 7. Merge into the review folder

Get fresh folders with `getPresetFolders`. Clone the current tree. Locate the intended folder by ID (or case-insensitive name at the correct parent); create it only when absent. Append the new preset IDs with deduplication, preserving existing children and every unrelated folder.

```javascript
postMessage({ type: 'setPresetFolders', folders: mergedTree, activeFolderId: reviewFolder.id });
postMessage({ type: 'getPresetFolders' });
```

The setter writes storage but does **not** echo the new tree. Wait for the subsequent `presetFolders` response before checking membership. In the SQLite document store the envelope ID is `preset-folders`, even though source APIs call it `preset-folders.json`.

## 8. Validate the persisted result and audio

1. Fetch every saved preset from the backend and compare scene titles, IDs, graph topology, parameters and resource references to the intended JSON. Normalize serialization defaults: `enabled: true` may be omitted. The UI can add a convenience top-level `graph`; compare canonical scene graphs.
2. Load every scene using the freshly fetched full preset. Verify the active preset/scene and no missing resources or new load errors.
3. Use the app’s signal test when the user’s monitoring setup is appropriate:

   ```javascript
   uiState.signalTest = null; // avoid reading a previous result
   postMessage({ type: 'runSignalPathTest', frequency: 220, duration: 0.4 });
   ```

   Wait for a new `signalPathTestResult` / `uiState.signalTest` with a bounded timeout. Check finite input/output values, sample rate, `passed`, and error logs. **The current implementation injects a full-scale sine**, so it can be audible/loud: use suitably lowered physical monitoring or an authorized offline alternative. Do not quietly change the user's global audio settings to compensate.
4. A sine test proves that audio reaches the output. It does not prove tonal similarity, absence of clipping/transients, perceptual loudness matching, or good behavior with real guitar dynamics. Audition a consistent dry guitar clip and inspect peaks when available; describe the actual verification performed. Check elapsed time too, but do not attribute a slow session to one capture without DSP timing evidence. Standard captures can reduce processing cost versus complex variants; a pass alone does not certify real-time performance.
5. Reduce gross level differences with per-node NAM output gain or the preset output trim. Preserve the intended gain staging into the amp. Re-save changed presets, fetch again, and repeat affected checks. Do not change global input calibration or output level to balance a preset pack.
6. For independent storage verification when needed, open `%APPDATA%/Soundshed Guitar/data/v1/soundshed.db` **read-only** with SQLite URI `mode=ro`. Current table: `items(type, id, json, updated_at)`. Read only the created IDs and folder document. Never copy a live database without WAL-safe handling or modify it behind the running app.
7. Refresh cache entries through backend fetches after updates. If a page reload is needed, preserve unsaved work first. Recheck the final active graph; loading an old `uiState.presets` object can undo the intended *runtime* trim while the database still contains the correct preset.

## 9. Leave reviewable deliverables

- Keep all requested presets in the review folder and show it in the **preset chooser**. The TONES navigation page opens community sharing, not the local preset-folder view.
- Leave a representative preset/scene loaded unless the user prefers the previous session restored. Confirm the visible folder contains the full intended list.
- Supply a short guide per artist: dated source, selected models/IRs with provenance, scene chains, pickup suggestions, approximations, and actual verification limits.
- Include a concise completion message with new and total counts. For “do more,” distinguish the new batch from the existing collection.
- If a portable archive is requested, use the app's preset collection export and check its resource payload/references. A plain JSON list referencing local resource IDs is not a self-contained preset pack. Respect resource licensing and publishing restrictions.
- Do not commit, publish, delete the user’s presets, or close the app as an automatic cleanup step.

## Source map

| Contract | Source |
| --- | --- |
| CDP control | `tools/agent-ui-debug/cdp-tool.mjs` |
| Runtime state | `core/ui/ts/state.ts` |
| Effect registry | `core/ui/ts/presetV2.ts` |
| Preset and graph types | `core/ui/ts/types.ts`, `core/src/presets/PresetTypes.h` |
| Authenticated Tone3000 requests | `core/ui/ts/tone3000.ts`, `tone3000Api.ts` |
| Model import | `core/ui/ts/tone3000Shared.ts` |
| Native bridge | `core/ui/ts/bridge.ts` |
| Authoritative preset fetch | `core/ui/ts/presets/fetch.ts` |
| Save/load/folders | `core/src/controller/PluginControllerPresets.cpp` |
| Signal-test input and pass criteria | `core/src/controller/SignalTestService.cpp` |
| UI storage document IDs | `core/src/controller/PluginControllerStorage.cpp` |
| SQLite schema | `core/src/storage/JsonStore.cpp` |
| Archive export | `core/ui/ts/presets/archive.ts` |
