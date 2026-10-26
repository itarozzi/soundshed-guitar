# Soundshed Nano Q: Soundshed Nano without JUCE

Status: **first working build, 2026-10-09. Windows verified; macOS builds and runs (2026-10-26, Intel,
Xcode 26.3); Linux not yet.** Branch
`nano-q`, in `nanoq/`. Soundshed Guitar Nano ([native-ui.md](native-ui.md)) is untouched and
still the shipping native product.

**Soundshed Nano Q** is Soundshed Nano rebuilt on [Cycfi Q](https://github.com/cycfi/q)'s
**QPlug** plugin framework and its **Elements** GUI library, with no JUCE anywhere in the
program. It is a separate app, with its own name, plugin id and bundle id
(`com.soundshed.guitar.nanoq`), so it installs beside Soundshed Guitar and Soundshed Guitar
Nano. It shares their engine and their profile (settings, `soundshed.db`, presets, resource
library), so a user can move between them.

## Why this is mostly a UI port

`core/` has no JUCE in it (one stale comment aside), and Nano already reaches the engine only
through `core/src/uiclient`: the same JSON messages the web UI sends, parsed into
`ClientState`. So everything JUCE did for Nano falls into three jobs:

| JUCE did | Nano Q does it with |
|---|---|
| Audio and MIDI callbacks, plugin formats (VST3, AU, CLAP, standalone) | QPlug, which builds one CLAP and has clap-wrapper make the VST3, AUv2 and a standalone app from it |
| File dialogs, thread hops, message thread, paths (`PluginProcessorAdapter` as `IPluginHost`) | `nanoq/src/Engine.*`, `FileDialogs.*`, `Paths.*` |
| The 10,000 lines of Components in `juce/source/nativeui` | `nanoq/src/ui/`, Elements elements drawn from the same theme tokens |

What is **not** reused from `juce/`: all of it, with one exception: the generated colour tokens
(`juce/source/nativeui/theme/ThemeTokens.generated.h`, a plain header made from the web UI's
CSS) and the Inter font files, so a theme looks the same in every product.

## Architecture

```
host or standalone ──► QPlug plugin (CLAP; VST3/AU/standalone via clap-wrapper)
                         ├─ NanoQController   host parameters, state, owns the engine
                         ├─ NanoQProcessor    audio and MIDI → PluginController::ProcessAudio
                         └─ NanoQPresenter    the editor: UiSession + Shell
Engine  ── guitarfx::PluginController on a thread of its own (its "message thread")
UiSession ── pumps engine messages to UiClient on the UI thread, 16 ms
Shell   ── Elements UI, rebuilt per region from ClientState
```

**The engine runs on its own thread.** `PluginController` expects a message thread that it can
post to (`IPluginHost::RunOnMainThread`) and that owns its non-realtime state. QPlug hands a
plugin no such thing, so `Engine` owns one: every call that is not the audio callback runs
there. The editor talks to it by posting requests; what it sends "to the UI" is queued while an
editor is open (and dropped when none is, as in the JUCE editor) and collected by `UiSession`'s
timer on the host's UI thread. `SerializeState` and `DeserializeState` are called from the
host's thread and hand themselves over, as they already do for AU, AAX and LV2.

**Host parameters** are the engine's automation slots, in the JUCE adapter's order (stable
defaults, custom slots padded to `kMaxCustomSlots`, later defaults after), one normalised
parameter each. A host change is queued by the audio thread and applied at the top of the next
block under the DSP lock, or at once while audio is stopped: the JUCE adapter's rules. The
values are not saved by QPlug; the engine's state blob (in the controller's extra state) holds
them, and after a load the controller asks the host to rescan values (see below).

**The UI** is owner-drawn, as Nano's is: `ui/Widgets.*` are small Elements elements (text,
button, parameter slider, level bar, list row, toast, dialog surface), and `ui/Layout.h` wraps
Elements' tiles for building from pointers. `Shell` owns five swappable regions (top bar,
scenes, chain, page, transport) and rebuilds the ones a message touched, comparing a signature
of what each shows first; the values of the page being edited and the meters are patched in
place instead, so a drag is never interrupted by the value it just set coming back.

### Things QPlug and Elements do not give, and what Nano Q does

- **No rescan request.** After a state load the engine's parameter values can differ from what
  the host last read, and CLAP hosts (clap-validator) treat that as a bug unless the plugin
  asks for a rescan. `src/clap_entry.cpp` replaces QPlug's entry shim to remember the
  `clap_host`, and `NanoQController::RescanHostValues` calls `params->rescan`.
- **No denormal guard.** `src/NoDenormals.h`, around `process`.
- **No standalone flag.** `RunningAsStandalone()`: clap-wrapper's standalone links the plugin
  into its own executable.
- **No main-thread handle.** The engine thread, above.
- **No SVG files.** Artist parses SVG path data but not SVG documents. `ui/Graphics.*` reads the
  subset the icons in `core/ui/images/icons` use (path, line, circle, ellipse, rect, polyline,
  polygon, groups with transforms, `style` declarations) into Artist paths once per icon and
  draws them in one colour, as the web UI's icons take `currentColor`. A grey in a many-toned
  icon (the Inkscape guitar) draws that colour fainter. Artist's `canvas::transform(...)` *sets*
  the matrix on every backend rather than adding to it, so a shape's transform is composed onto
  `canvas::transform()` by hand. The same file loads the equipment artwork (PNG, JPEG) once per
  file.
- **Tile layout traps.** A column is as wide as its narrowest child's *maximum* width, and a
  scroller reports its content's height as its maximum. `Row`, `Col`, `VScroll` and `HScroll`
  in `ui/Layout.h` correct for both.

## Building

```powershell
nanoq\build.cmd Release          # configures (first time: ~5 min, fetches Q, Elements, clap-wrapper, NAM, ...) and builds
```

On macOS, with Xcode's command-line tools and Ninja:

```bash
cmake -S nanoq -B nanoq/build/Release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build nanoq/build/Release
SOUNDSHED_NANOQ_PROFILE=/tmp/nanoq-profile "nanoq/build/Release/products/Soundshed Nano Q.app/Contents/MacOS/Soundshed Nano Q"
```

It needs no changes to build there: Elements takes its Cocoa host and Artist its Quartz 2D
backend, for the host's architecture. The products are `Soundshed Nano Q.app`, `.clap`,
`.component` (AUv2) and `.vst3`, with the engine data in each bundle's `Contents/Resources/ui`.
`SOUNDSHED_NANOQ_PROFILE` runs it on a throwaway data root instead of the real profile.

Products land in `nanoq/build/Release/products/`: `CLAP/`, `VST3/`, and
`Standalone-nanoq_standalone/`, each beside its fonts and the engine's runtime data
(`<name> Resources/` next to a CLAP or the exe, `Contents/Resources` in a VST3), the same
data Nano ships (`presets assets metronome demo data images` of `core/ui`).

Dependencies are fetched and pinned (Q to a commit; Q pins Elements, clap, clap-wrapper).
`GUITARFX_CORE_BUILD_WEB_UI` (new, default on) lets a native-UI product skip core's npm build
of the web UI; Nano Q turns it off, so building it needs no Node.

## Testing and driving it

`nanoq/tools/` (PowerShell, Windows):

| Script | Does |
|---|---|
| `dev.ps1 -Profile <dir> [-Shot png] [-Wait n] [-NoRun]` | Stops a running Nano Q, builds, reports only errors, runs the standalone on a throwaway data root and captures its window |
| `run-isolated.ps1` | The run-and-capture half |
| `window-shot.ps1 -Out png [-Resize WxH]` | DPI-aware `PrintWindow` capture, which works when the window is behind others |
| `click.ps1 -X -Y [-ToX -ToY] [-Wheel n] [-Right] [-Hover] [-Text s]` | Posts mouse and character messages straight to the view window, in capture coordinates; leaves the user's pointer alone |

**Driving it on any platform: `SOUNDSHED_NANOQ_DRIVE`.** macOS ignores mouse events posted to a
window that is not in front, so `click.ps1` has no Mac equivalent. With
`SOUNDSHED_NANOQ_DRIVE=<file>` set, Nano Q checks that file every tick; if it exists, its lines are
played into the Elements view and the file is deleted (`src/ui/DevDriver.*`). A script writes the
file, waits for it to disappear, then captures the window (`screencapture -l <window id>` on
macOS). Coordinates are the view's: logical pixels from its top-left, below the title bar.

```text
click 590 157          left click              double 395 170     double click
right 590 157          right click             move 380 165       pointer moves (hover)
drag 10 10 200 10      press, move, release    scroll 600 500 -40 wheel (negative: down)
```

**Always run with a profile argument.** The engine's data root is not where the host says: it
comes from the `APPDATA` environment variable through core's `FileSystem`. Nano Q's
`SOUNDSHED_NANOQ_PROFILE` calls `FileSystem::SetPlatformRootOverride`, which is what keeps a
test run off the real profile.

Plugin validation reuses `tools/validate-plugins.mjs`, which now takes `--product`:

```powershell
node tools/validate-plugins.mjs --artefacts nanoq/build/Release/products --product "Soundshed Nano Q" --formats CLAP,VST3
```

## Progress

| Area | State |
|---|---|
| Engine hosting, audio, MIDI, state, host parameters, FTZ/DAZ | Done |
| CLAP (clap-validator 0.4.1) | **35 passed, 0 failed**, 9 skipped, Windows |
| VST3 (pluginval 1.0.4, strictness 10, editor tests on) | **Passes**, Windows |
| Standalone app | Runs; audio device settings are clap-wrapper's own window (Alt+Space) |
| Top bar: preset name, previous/next, save, preset menu | Done |
| Scenes strip | Done (select); add, rename, remove from the preset menu |
| Chain strip, drawn as the web UI's (2026-10-26) | Done: input and output as cards, each effect a card with its icon (or the equipment picture for a model or IR node), name in capitals, effect, NAM badge, signal light from the node's telemetry, OFF and missing-file marks; a power button on hover bypasses; a double click replaces (the picker opens on the node's category with its effect marked Current); a cable with a + between every pair inserts on that edge of the graph, so into any parallel branch too, empty ones included; parallel blocks drawn between a split rail and a join rail, the split selectable; the chain wraps onto more lines with a cable back under each (`ui/ChainStrip.cpp`, `ui/ChainWidgets.h`), up to 40% of the window and then scrolls. Right-click menu adds Replace, Rename, Use the automatic name. No drag-to-reorder yet |
| Effect page, drawn as the web UI's effect view (2026-10-26) | Done: on the category's gradient (`visualBackground`), a header with the light, the name (a tap renames), category and effect, Presets (with Save current settings) and the on/off switch; the equipment artwork at the left when the window has room; parameters in boxed groups laid out as they fit; a parameter's `visibleWhen` honoured (the page redraws when the controlling value changes); an enum of more than six choices, or one the engine marks `picker`, opens a list grouped by its `labelInfo`; a splitter shows Branches (2 to 4, fewer than the branches holding effects disabled) and Back to one lane (`ui/EffectPage.cpp`, `ui/NodeControls.cpp`) |
| Model and IR slots | Done: Model (NAM), IR, and the IR Cabinet's IR A and optional IR B: the loaded resource, previous and next through the library (by category, then name), the library as a list by category, Browse (the engine's file dialog; the file joins the library) and clear for an optional slot. Not the web UI's category filter, Tone3000 results, or the IR list between IR A and IR B |
| Presets page: search, All/Favourites/Recents/Folders, favourite, delete, load with unsaved-changes confirm | Done |
| Setlists: choose, step, jump to a slot | Done (editing is in Soundshed Guitar, as in Nano) |
| Input and output sheet (global chain) | Done |
| Tuner, metronome (tap tempo, meter, subdivision, click, volume) | Done |
| Settings: theme, limiter, preset switch tails, NAM quality, oversampling, anti-alias | Done |
| Toasts for the engine's notifications | Done |
| **Tones page** (tone sharing) | **Not done.** Needs an HTTP client: core has none (JUCE's `URL` did it). Windows could use WinHTTP, macOS NSURLSession, Linux libcurl, plus image decoding for pack pictures |
| Effect icons | Done: the shared SVGs, on chain cards, the effect page and the effect picker |
| UI scale setting, wrapped chain layout, touch long-press, compact (phone) layout | Not done; Nano Q targets desktop plugin windows first |
| macOS build | **Builds and runs** (2026-10-26): all four products, the engine starts and loads the default preset, the window draws correctly, `NanoQEngineDialogTests` passes. Open and save dialogs are AppKit panels (`FileDialogs_mac.mm`) and open; before, macOS reported every dialog as cancelled. Apple silicon not yet built |
| Linux build | **Untested.** A Linux compile in a container was started and abandoned (see the session notes); QPlug documents it |
| iOS / iPadOS | **Runs in the iPad Simulator** (2026-10-26): a UIKit host for Elements (`nanoq/ios/elements_host`), Artist's Quartz backend on Core Text (`nanoq/ios/patches`, applied at configure in iOS builds only) and an app that hosts the CLAP and plays it through RemoteIO (`nanoq/ios/app`). Built with `nanoq/ios/build.sh`. Device builds and what is left: [nano-q-ios.md](nano-q-ios.md) |
| Resource browsing (NAM and IR models) | The slots above. No folder browser, search, favourites or Tone3000 import |
| **Not yet at parity with the web UI's chain and effect view** | Undo/redo and A/B; drag to reorder or onto another line; the NAM badge for a model whose library row lacks `architectureVersion` (the web UI reads the model file for it); artwork from a model's own metadata (an http URL); custom effect layouts (Layout Designer); the EQ curve and spectrum, cabinet and Pickup Match response curves, IR alignment, Gate Learn, the spatial panner; channel mode; per-input mixer levels as their own control (they show as the mixer's knobs); blends, composites and hosted plugins; the cables' live-signal animation |
| CI | None yet; `nanoq/build.cmd` and the validator command above are what a workflow would run |

## Decisions and open questions

- **Separate directory, not a target in `juce/`.** `juce/CMakeLists.txt` brings in JUCE for
  everything it builds; Nano Q is its own CMake project that pulls in `core/` directly.
- **Host parameters follow the JUCE layout** so the two products expose automation the same
  way, even though a DAW project cannot move between them (different plugin ids).
- **Elements, not a bespoke UI toolkit.** Elements brings a window host for each platform (Win32,
  Cocoa, X11), a Direct2D/Quartz/Cairo drawing layer, text input and scrolling.
- **Open: Tones.** Which HTTP stack per platform, or one library (cpr/libcurl, cpp-httplib with
  TLS), is the decision to make before porting that page.
- **Open: QPlug is new (Q 1.5).** Its API may change between minor releases; Q is pinned to a
  commit, and `clap_entry.cpp` leans on its `q_plug_entry_*` symbols.
