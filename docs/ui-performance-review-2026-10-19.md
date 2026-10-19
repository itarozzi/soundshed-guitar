# UI CPU and memory review (2026-10-19)

Where the WebView2 UI spends CPU and memory, measured in the running Standalone, and
what to change. Items 1 and 2 of the recommended order were done the same day; see
"Done 2026-10-19" at the end for the before/after numbers. Item 5 was dropped after
measuring (see Memory finding 1).

## How it was measured

Release Standalone built from main on 2026-10-19 (14:47, before 5d623895), launched with
`WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--remote-debugging-port=9333`, window
2195 × 1164 CSS px at devicePixelRatio 1.75, a developer profile (440 NAM models, the
"Animate signal flow" setting on, Debug State Capture on). Audio was the `di-riff-01`
demo clip looped with the output muted (`setOutputMuted(true)` and
`previewDemoAudio` with `repeat: true` from `dist/bridge.js`). CPU is per process from
`Win32_Process` kernel+user time, plus CDP `Performance.getMetrics`, a devtools timeline
trace and a CPU profile for the renderer main thread. All figures are percent of one core.
A/B tests injected a `<style>` override into the running page, interleaved twice.
See `tools/agent-ui-debug/README.md` for driving the app over CDP.

## Baseline

| State | Renderer process | Renderer main thread | GPU process | Notes |
| --- | --- | --- | --- | --- |
| Visible, silent | 2.6% | 1.5% | 1.2% | 16 style recalcs/s, all from the selected-node peak meter |
| Visible, playing | 12–14% | 9.0% | 14–15% | ~53 main frames/s, 60 compositor frames/s, 2 render passes per frame |
| Minimised | 0% | 0% | 0% | engine feeds stop (`uiVisibility`); GPU memory falls ~100 MB |

The app process itself idles at ~9.5%, but 6.7 of that is the audio thread and 1.4 the
message thread: DSP, not UI.

Memory, visible:

| Process | Private |
| --- | --- |
| GPU process | 230–355 MB (drifts; ~220 MB minimised) |
| Renderer | 97–107 MB, of which the JS heap is only 9–13 MB |
| Browser | ~50 MB |
| Network, storage, crashpad utilities | ~25 MB |

So the UI costs ~400–500 MB, and almost none of it is JavaScript objects. The GPU
process (tiles and render surfaces for a 3841 × 2037 device-pixel window) and the
renderer's DOM, style and decoded images are the weight.

DOM: 8,948 elements (24k nodes), **73% not rendered**. The biggest hidden subtrees are
`#equipment-library-results` (the resource library, 3,272 elements, 172 SVGs, inside
Settings), `#riff-library-list` (837), `#preset-library-presets-panel` (462) and
`#automation-slots-list` (461). 451 inline SVGs in all, 75 stylesheets, 3,314 rules.

`uiState` by JSON size: `resourceLibrary` 810 KB, `layoutLibrary` 215 KB (custom
layout images), `presets` 66 KB, everything else small.

## CPU findings, ranked

### 1. The params panel's backdrop blur doubles GPU work while playing — measured

`css/signal-path/params-panel.css:18` gives `.node-params-panel` `backdrop-filter:
blur(10px)`. A node is selected by default, so the panel is always on screen, and the
peak meter inside it changes 20–30 times a second, so Chromium re-blurs the region
behind the panel every frame (the trace shows two render passes per frame).

A/B in the live app, CSS override injected, interleaved twice, 8 s each:

| Variant | GPU process | Renderer main |
| --- | --- | --- |
| As shipped | 14.4 / 14.9% | 9.0 / 8.9% |
| No backdrop blur on `.node-params-panel` | 7.1 / 7.9% | 9.4 / 9.8% |

**Fix:** drop the backdrop blur from the params panel (its background is transparent
there, so the blur is barely visible), or replace it with an opaque or pre-tinted
background. Half the GPU process's time while playing, for one CSS line. The same
reasoning applies to the per-row blurs in the DSP diagnostics list
(`css/base.css:1116`), `.composite-list-item` and `.layout-list-item`
(`css/advanced-panel.css:117/453`) and `.dsp-status-tile` (`css/dsp-status.css:24`):
blur only over things that do not change, and never per list row.

### 2. A state broadcast costs ~90 ms of UI JavaScript, most of it for hidden views — measured

Five `requestState` round trips: each `state` message is **890 KB** and takes
**85–138 ms** inside `IPlugReceiveData` with the window minimised (so no style, layout
or paint included). A state arrives after every preset load, node add/remove,
bypass, scene change and library edit, so each of those is a long task on top of the
engine's own 65–90 ms `BroadcastState` (measured at launch, 2026-10-08).

CPU profile, per state (inclusive):

| ms | What |
| --- | --- |
| 61 | garbage collection (parsing and cloning ~0.9 MB) |
| 40 | `refreshSettingsView` → `renderLibraryView` → `renderGroupedLibraryView` (`settings/libraryView.ts:85/166`): re-renders the hidden resource library, including a forced layout (`clientWidth`, 10 ms) and `deduplicateResourcesByHashAndPath` (9 ms) |
| 34 | `postUiDebugSnapshot` — **only because Debug State Capture is on in this profile**; users with the flag off do not pay it |
| 24 | `renderPresetUI` (preset list, folder tree, details) |
| 22 | `onEffectCatalog` |
| 16 | `renderSignalPathBar` (and the survey found `refreshPerformancePads` calls it a second time, `performancePads.ts:927-933`) |
| 3 | `renderRiffLibraryPanel`, hidden |

**Fixes, in order of payoff:**
- **Take `resourceLibrary` (and `layoutLibrary`) out of `state`.** Send them as their own
  messages when they change, with a revision number. That removes ~90% of the bytes, most
  of the GC and the dedup pass, and the engine's serialisation of them on every
  broadcast. It also takes ~0.1–0.15 s off launch, where the first state is drawn.
- **Do not render hidden views.** `renderLibraryView`, the riff library, the automation
  list and the preset list popover should mark themselves dirty when hidden and render
  when shown. That is ~45 ms of the remaining cost and most of the hidden DOM.
- **Render each area only when its slice changed.** `stateHandlers.ts:46-239` rebuilds
  everything for any change. Compare a cheap signature (revision, graph hash, preset
  list length plus ids) per area and skip unchanged ones; coalesce to one render per
  animation frame. The chain must not be rebuilt twice per state.

### 3. Meter transitions keep the main thread at 60 frames/s — measured

`.vu-seg` has `transition: background 60ms linear` (`css/vu-meter.css:61`) and
`.default-effect-shell-meter` `transition: transform 0.12s, background 0.12s, opacity
0.12s` (`params-panel.css:349`). Telemetry flushes at ~26 Hz, but each class or style
change starts a transition, and background-colour transitions run on the main thread,
so the page renders a full lifecycle every display frame (53/s measured).

A/B: no transitions on those two selectors took the renderer main thread from 9.0% to
7.9% and style recalc from 2.3% to 1.9%; with the blur removed too, 7.1% main and
~8.9% GPU. (The override missed the shell meter, whose transition is on `::after`, so
that gain is the `.vu-seg` change alone.)

**Fix:** no colour transitions on meter segments (switch them instantly, or fade with
`opacity`, which the compositor can run). Keep the shell meter's `transform` transition
but drop `background`/`opacity` there, and give it `will-change: transform`.

### 4. The selected-node peak meter rewrites the DOM on every frame, even when silent — measured

`ts/signalPath/telemetry.ts:83-112` removes and re-adds `is-inactive`/`is-clipped`,
rewrites `rail.title` and sets `--meter-fill-scale` on every diagnostics frame without
comparing to the last value. With no audio it still mutates class and title ~17 times a
second, which is all 16 style recalcs/s of the idle page.

**Fix:** keep the last written state and write only on change; set the title on
`pointerenter`, not per frame.

### 5. From code reading (not measured here)

From the CPU survey; each is real code but did not show up in this session's
measurements, either because the view was closed or the feature was not in use:

- **DSP diagnostics list renders while Settings is closed** if its sub-tab was left on
  DSP Performance: `isDSPPerformanceTabVisible()` (`views.ts:735`) checks the sub-tab,
  not `#panel-settings.active`, and the choice persists. 20 Hz `innerHTML` rebuild with a
  topological sort per graph. Not happening in this profile (verified: no mutations), but a
  user who once opened that tab pays it for good. Cheap fix: also require the Settings
  panel to be active.
- **MIDI/automation rebuilds the whole params panel** on each
  `signalPathNodeParamUpdated` (`messages/signalPathHandlers.ts:123-136` →
  `paramsPanel/panel.ts:49-66`), tearing down the EQ curve, spectrum watcher and every
  knob. Update the one knob through `nodeParamKnobs`, batched per frame.
- **Analyzer spectrogram** redraws its whole history each frame with a new `hsl()`
  string per cell (`signalPath/telemetry.ts:387-478`), and can push the same bins twice.
  Scroll the image with `drawImage` and draw one column from a colour table.
- **Waveform canvases** reset `canvas.width/height` every draw (`waveform/render.ts:182`),
  which reallocates and forces layout; the practice tool runs a 60 fps loop to move the
  playhead and the riff preview loop has no visibility check. Resize on change only,
  cache the peaks offscreen, move the playhead with a CSS transform.
- **EQ curve and spatial panner** call `getComputedStyle` on every draw at 30/20 Hz
  (`eqCurve.ts:298-321`, `spatialPanner.ts:51-67`); cache theme colours as
  `waveform/render.ts:97-114` already does.
- **Console logging** of every non-telemetry message, twice, plus tuner logs per frame
  (`messages.ts:159-162`, `tunerHandlers.ts:23`, `tuner.ts:350-397`). Cheap without
  DevTools attached, but put the tuner, transport, riff progress and metronome beat on
  the skip list.

## Memory findings, ranked

### 1. The GPU process is the largest consumer, and it follows window area and layers

220–355 MB private. Minimising frees ~100 MB at once, so a good part is tiles and
render surfaces for the visible page at DPR 1.75. The params-panel blur was one such
surface: removing it took ~50 MB off the GPU process while playing (see "Done").

**Equipment images: not oversized for where they matter — checked and dropped.**
`images/equipment` holds 21 PNGs of 700–1700 px (2.3–6 MB each decoded, most already
8-bit palette PNGs). The params panel draws them with `object-fit: cover` in a box
220–300 CSS px wide and 200–620 px tall (`params-panel.css`, `.default-effect-shell-equipment-panel`),
stretched to the height of the controls. For a wide image the height sets the scale, so
at DPR 1.75 a 620 px panel needs ~1085 device px of image height: every amp, cab and rack
image is at or below that already, and only the portrait pedals (~700 × 1290) could shrink,
by ~15%. Downscaling would blur tall panels for a few MB. What is oversized is the
signal-chain thumbnail (`.node-layout-thumb`, 57 × 32 CSS px, `signalPath.ts:938`), which
decodes the same full image; it only costs extra for nodes that are not also the selected
one. Small per-node thumbnails would fix that, but it is new assets plus a path mapping
for likely well under 10% of UI memory, so it was not done.

`images/icon.png` is 1024² (4 MB decoded) on the splash at 128 CSS px, which needs
224–256 device px, so `icon_sm.png` (128²) would be blurry; a 256² copy would save ~4 MB
of decode during boot only. `images/themes/70s.png` and `steampunk.png` (4.5 MB on disk)
are referenced nowhere: package size, not memory.

**Fixes:** remove blurs over changing content (done for the params panel). Consider
WebView2's `MemoryUsageTargetLevel` low while hidden (host side, below).

### 2. Hidden views hold most of the DOM

6,491 of 8,948 elements are not rendered; the resource library alone is 3,272. They cost
renderer memory (DOM, style data, 172 SVGs) and, more importantly, CPU on every state
broadcast (CPU item 2). Rendering them only when opened, and virtualising the resource
library list (440 models, each with an inline SVG icon), shrinks both. Inline SVG icons
repeated per row could be one `<symbol>` sprite referenced with `<use>`.

From the memory survey, the lists that are rendered in full and kept after closing:
- the resource browser's Library tab (`resourceBrowser.ts:1292`), all ~440 rows by
  `innerHTML`, rebuilt on every keypress in its search box (`:406`) and not cleared by
  `close()` (`:743`). Its Folder tab is already virtualised
  (`resourceBrowser/folderVirtualList.ts`), so that class can be reused;
- Settings → Library (`settings/libraryView.ts:188,209`);
- the preset list (`views.ts:417-520`): an `<article>` per preset with five star buttons
  and per-element listeners, rebuilt per state while its popover is closed. Event
  delegation removes the per-row closures;
- Tone Sharing browse, where "load more" appends without a cap (`toneSharingPanel/feed.ts:316`)
  and re-renders the lot (`feed.ts:192`), and its thumbnail object URLs
  (`toneSharingPanel/images.ts:13,101`) are revoked only on panel init or a settings
  change. An LRU of ~100 thumbnails bounds it.

### 3. JavaScript state is small; the cost is churn, not retention

JS heap 9–13 MB. The 890 KB state is parsed and partly cloned on every broadcast, which
is what drives the 61 ms of GC per state; taking `resourceLibrary` out of it (CPU item
2) fixes the churn. It arrives through `evaluateJavascript` as a string literal in a
compiled script (`PluginEditor.cpp:507-515`, `UiBridge.cpp:139`) and is then
`JSON.parse`d, so each broadcast briefly holds the script source, the literal and the
parsed tree, and the native side makes four or five copies on the way (dump, escape,
`juce::String`, UTF-16). 31 of the 32 `BroadcastState()` call sites use the full scope,
including most signal-path edits (`PluginControllerSignalPath.cpp:260,909,1020`), so
`StateScope::PresetOnly` there is a cheap first step before splitting the library out.

**Checked and ruled out:** the survey suspected `console.log(message.substring(0, 200))`
(`messages.ts:160`) pins each whole state string. Measured: ten state broadcasts grew the
post-GC heap by 0.5 MB (11.1 → 11.6), against ~9 MB if they were retained. The logging is
still worth trimming for CPU, not memory.

`layoutLibrary` at 215 KB is custom layout images as base64 data URLs
(`PluginControllerLayouts.cpp:369,677,697,826` → `layoutHandlers.ts:60`), and
`layoutRenderer.ts:77-80` prefers the data URL and writes it into HTML and inline-style
strings, re-parsed on each render. User-imported images make it unbounded. The
`layout-images/<file>` URL fallback already exists (`layoutRenderer.ts:82`): serve them
through the resource provider and drop `dataUrl` from the payload.

Bounded but large in the worst case (from code reading):
- undo history, 100 entries × 2 A/B slots, each a deep-cloned graph plus a sorted JSON
  signature (`signalPath/historyModel.ts:24,128`): ~5–12 MB worst case. Keep a hash, not
  the signature string, or cap at 30–50;
- `presetCache` (`presetLibraryStore.ts:124`) keeps every preset ever loaded, pruned only
  on delete;
- `onState` makes up to three `JSON.parse(JSON.stringify(...))` clones per new preset
  (`stateHandlers.ts:220-228`);
- the layout designer's 50 undo steps are full-layout JSON strings (`layoutDesigner.ts:145,624`).

### 4. The jam scales iframe is loaded while hidden — small

`.jam-scales-frame` loads `https://scalex.soundshed.com/` at start even though it is not
shown. Unloading it saved only ~6 MB of renderer memory here, but it is also network and
startup work for a view most sessions never open. Set `src` when the view is first shown.

### 5. Fonts and the page's own files — load time and package size more than memory

`css/` is 2.5 MB, 1.8 MB of it two variable TTFs (`Inter-VariableFont_opsz,wght.ttf`
875 KB, the italic 904 KB; `css/base.css:2-16`). The CSS itself is ~480 KB over 74
`<link>`s, with no data URIs. A Latin-subset WOFF2 of each is ~100 KB, and the italic
can load on first use. `UiResourcePrefetch` (`juce/source/editor/UiResourcePrefetch.cpp:24-28`)
reads every CSS, TTF and `dist/*.js` (~6.5 MB) into native memory per editor open and
frees it when the page finishes loading, so it is transient.

## Host side (WebView2)

- Nothing sets `MemoryUsageTargetLevel`, `TrySuspend` or extra browser arguments, and
  JUCE 9.0.3's `WebBrowserComponent` does not expose the `ICoreWebView2`. On hide, JUCE
  only calls `put_IsVisible(false)`; with `withKeepPageLoadedWhenBrowserIsHidden()`
  (`PluginEditor.cpp:411`) the page stays loaded. The page sends `uiVisibility` and the
  engine stops its feeds, so CPU goes to zero (measured), but memory is not released
  beyond the GPU tiles (~100 MB measured).
- All editors share one user data folder, `%LOCALAPPDATA%\Soundshed Guitar\WebView2`
  (`juce/source/WebView2UserData.h:37-62`), so instances in a DAW share one browser
  process group; each editor is still its own page and renderer.
- **Proposed:** a small JUCE patch or accessor to reach `ICoreWebView2_19` and set
  `COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW` while the editor is hidden or minimised,
  `NORMAL` on show. Chromium then drops caches and decoded images. `TrySuspend` after a
  long hide is possible too, but not while the Jam video plays. Not recommended:
  `--disable-gpu`, which moves compositing onto the CPU.

## Recommended order

| # | Change | Gain | Size |
| --- | --- | --- | --- |
| 1 | Drop `backdrop-filter` from `.node-params-panel` (and per-row blurs) | GPU process −50% while playing (measured) | CSS lines |
| 2 | Diff-before-write in `updateSelectedNodePeakMeter`; no colour transitions on meters | idle style recalcs 16/s → ~0; main thread −1 point while playing (measured) | small |
| 3 | Skip rendering hidden views on `state` (library, preset list, riffs, automation); fix `isDSPPerformanceTabVisible` | ~45 ms off every state broadcast (profiled) | medium |
| 4 | `resourceLibrary` (and layout images) out of `state`; `PresetOnly` scope for graph edits | most of the remaining ~60 ms GC per state, ~90% of the bytes, engine serialisation too | medium, touches protocol |
| 5 | ~~Resize equipment PNGs and the splash icon~~ | dropped: the panel needs their resolution (Memory finding 1) | — |
| 6 | Per-area render signatures in `onState`; single chain render | the rest of the state cost | larger |
| 7 | `MemoryUsageTargetLevel` low when hidden | memory back while the window is minimised | JUCE patch |
| 8 | Virtualise the long lists; LRU the tone-sharing thumbnails; serve layout images by URL | renderer memory, smoother lists | medium |

3 and 4 want the smoke test and a live check that the hidden views are right when
opened. The code-reading items in CPU section 5 were not measured and are probably under
10% each in normal use; measure one in the case it targets (automation running, analyzer
open, practice tool playing) before building it.

## Done 2026-10-19: items 1 and 2

- `css/signal-path/params-panel.css`: no `backdrop-filter` on `.effect-visualization
  .node-params-panel` (screenshots with and without it were identical, since the stage
  behind is plain); the shell meter's `::after` transitions `transform` only.
- `css/vu-meter.css`: no `background` transition on `.vu-seg`.
- `ts/signalPath/telemetry.ts` `updateSelectedNodePeakMeter`: remembers what each rail
  shows (a `WeakMap` per element, so a re-rendered panel starts fresh), writes a class or
  the fill only when it changes, rounds the fill to hundredths, and keeps the title current
  only while the rail is hovered. `tests/selectedNodePeakMeter.test.ts` covers it (it fails
  on the old code).

Measured as two copies of the same Release Standalone, one with HEAD's UI and one with
these changes, launched alternately twice (fresh launch each), 10 s idle then three 8 s
windows playing the muted demo loop. Same window, profile and preset. Mean of six windows:

| Playing | Before | After |
| --- | --- | --- |
| Renderer process | 17.8% | 10.6% |
| GPU process | 30.8% | 11.0% |
| Renderer main thread | 9.3% | 6.6% |
| Style recalc | 2.2% | 1.3% |
| Style recalcs/s | 57 | 47 |
| GPU process memory | 282–310 MB | 240–248 MB |

| Idle, visible | Before | After |
| --- | --- | --- |
| Style recalcs/s | 15.7 | 0 |
| Renderer main thread | 1.4% | 1.05% |
| GPU process | 2.0% | 0.9% |

Renderer plus GPU while playing went from ~49% of a core to ~22%. The GPU process ran
hotter on these fresh launches than in the first session's measurements (~14.5%), but
the before/after runs alternated, so the comparison holds.

## Second pass 2026-10-19: what was left, measured

Measured on a Release build with items 1–4 in (the items 3–4 branch's Standalone), same
method: interleaved fresh launches, demo loop muted.

**Remaining playing cost** was renderer ~10%, GPU ~12%, main thread ~5.5%. The compositor
still presented ~58 frames/s although telemetry changes the page ~26 times a second.
Live CSS A/B:
- the selected-node meter fill's `transform` transition (on `.default-effect-shell-meter::after`)
  restarted on every update and kept the page animating between them: removing it took
  the main thread from ~5.8% to ~3.9% and style recalc from 1.1% to 0.5%;
- the VU segments' glow (`box-shadow` on `.vu-seg.active`): no clear effect, left alone;
- the "Animate signal flow" option (on in this profile, off by default): not measured
  cleanly (the override missed its `::before`), left alone.

Snapping the fill without easing raised GPU time (11.0% → 14.2%), because the fill lost
its compositor layer and was repainted on every update. `will-change: transform` on it
brought GPU back down (12.6% → 10.0% over three interleaved pairs).

**Memory while minimised:** simulated critical memory pressure (CDP
`Memory.simulatePressureNotification`) freed nothing; a forced JS purge freed ~15 MB of
renderer memory (98 → 83 MB) and nothing in the GPU process (198 MB). That is ~4% of what
a minimised window holds, and it would need a JUCE patch to reach
`MemoryUsageTargetLevel`, so it was not done. The forced purge also left the page with no
JavaScript context, so purging is no option either.

**MIDI and host automation** (`signalPathNodeParamUpdated`, up to 30 Hz per parameter
from `PluginControllerPresetEdits.cpp`) rebuilt the selected node's whole params panel on
every message, including for changes to other nodes and echoes of an unchanged value.
Replaying 30 Hz updates on a 7-knob NAM panel, the page went from 1% to 15–16% main
thread, 25 layouts/s.

### Done

- `css/signal-path/params-panel.css`: the meter fill steps without easing, on its own
  layer (`will-change: transform`).
- `ts/signalPath/engineParamChange.ts` (new) and `messages/signalPathHandlers.ts`: an
  engine-side change draws nothing for a node that is not selected or a value the UI
  already had; otherwise `showEngineParamChange` (`paramsPanel/paramControls.ts`) moves
  that knob in place and refreshes the node's visualisations. A change no knob can show
  alone (a toggle, menu or slider; a blend parameter; a pitch shift range setting)
  rebuilds the panel at most every 100 ms. `tests/engineParamChange.test.ts` covers the four
  cases. A side effect: automation no longer replaces a knob the user is dragging.

Before is the items 1–4 build, after adds these; two interleaved rounds each:

| Playing (6 windows) | Before | After |
| --- | --- | --- |
| Renderer process | 9.2% | 7.0% |
| GPU process | 11.3% | 8.0% |
| Renderer main thread | 5.4% | 3.6% |
| Style recalcs/s | 42 | 23 |

| Automation, 30 Hz on one knob | Before | After |
| --- | --- | --- |
| Handler time per message (median) | 1.8 ms | 0.2 ms |
| Renderer main thread | 16.3% | 6.6% |
| Renderer process | 26.9% | 11.8% |
| GPU process | 21.5% | 11.8% |

What remains under automation is the knob's own display update (~27 layouts/s), and while
playing the 26 Hz telemetry redraw of the VU meters and the opt-in signal flow animation.
Neither has a measured cause above the 10% bar yet.

### Still open from the list, not measured

Long lists (the resource browser's Library tab rebuilt per search keypress, unbounded Tone
Sharing paging and thumbnail object URLs), fonts and unused images (package size), and the
per-view items in CPU section 5. None was measured in this pass.
