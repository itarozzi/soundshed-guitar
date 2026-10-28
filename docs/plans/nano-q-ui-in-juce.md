# Nano Q's UI in the JUCE app

Status: **spike, 2026-10-28, Windows only.** Built with `-DSOUNDSHED_NANOQ_UI=ON`; the normal
build is unchanged. See Results.

## The question

Can one native UI serve both shells, Nano Q (QPlug) and Soundshed Guitar (JUCE), so the UI is
written once and neither plugin framework becomes a permanent dependency?

## The shape

```
core/                      engine, controller, uiclient            (no framework)
nanoq/src/ui/              the native UI, drawn with Elements      (no plugin framework)
   UiLink.h                what the UI needs from its host: a JSON mailbox, the asset root,
                           layout images, a per-pump hook
   UiRoot.*                session + shell on an Elements view (both shells use it)
   EmbeddedUi.*            the UI in a child window of a window someone else owns
shells
   nanoq/src/              QPlug: Engine is the UiLink; QPlug's presenter gives the view
   juce/source/NanoQUiEditor.*   JUCE: the processor's WebView bridge is the UiLink; the
                           editor's peer HWND is the parent window
```

The UI never depended on QPlug. Before the spike it depended on Nano Q's `Engine` for five calls
(`SendRequest`, `TakeMessagesForView`, `SetViewAttached`, the asset path and the standalone's
device pump) plus one reach into the controller (`ResolveLayoutImageFile`). Those are now
`UiLink`, and `Engine` implements it. The UI also uses four platform helpers from `nanoq/src`
(`Paths`, `Log`, `WebView`, `WindowChrome`), which name no framework and compile in either shell.

JUCE's side is a `SoundshedEditorBase` subclass. Requests go to
`PluginProcessorAdapter::handleWebMessage`, posted to the message thread so they are handled
after the UI event that sent them, as a WebView message is. What the controller sends the page
arrives on `setWebMessageCallback` and waits for the UI's next pump. The engine cannot tell this
editor from the WebView one. The base class still drives `OnIdle`, remembers the size, and drops
the standalone's launch curtain.

## How the window is shared

Elements on Windows is a Win32 child window that draws with Direct2D and runs entirely on the
thread's message loop (its timer is a `WM_TIMER`; `view.post` is drained from it). JUCE's message
loop dispatches those messages like any other window's, so nothing has to be pumped.

The editor makes the Elements view a child of its peer's HWND and keeps it over the editor's area
in physical pixels, using the sums `juce::HWNDComponent` uses. `HWNDComponent` itself cannot be
used: it destroys the window it is handed when it lets go, and the Elements view destroys its own.

## Build

```powershell
cmake -G "Visual Studio 18 2026" -A x64 -S juce -B juce/builds-nanoq-ui -DSOUNDSHED_NANOQ_UI=ON
cmake --build juce/builds-nanoq-ui --config Release --target SoundshedGuitar_Standalone
```

`juce/cmake/NanoQUi.cmake` fetches Elements at Nano Q's pin with Nano Q's patch, compiles
`nanoq/src/ui` and the four helpers into `SoundshedNanoQUi`, links it into the shared code, and
copies the fonts into `resources/` beside each binary, where Elements looks. Elements' own
CMake is added with `add_subdirectory`, never `include(ElementsConfigCommon)` at the top level,
which would call `project(elements)` and rename the JUCE project.

## Results

Windows 11, Release, standalone, 175% display scaling, run on a throwaway profile (`APPDATA`
pointed at an empty folder) and driven with Nano Q's dev driver (`SOUNDSHED_NANOQ_DRIVE`, which
is in `ui/` and so came along). The PC was locked, so whole-window captures were black and the
driver's offscreen `shot` was used instead.

| Check | Result |
|---|---|
| Builds first time with Nano Q's UI sources unchanged apart from the link | Yes |
| The UI starts on the JUCE engine and loads the default factory preset | Yes |
| The child window covers the editor's area | Yes: maximised, client 3840x2036, `ElementsView` 3842x2037 (rounded up) |
| A click round-trips (next preset) | Yes |
| Typing reaches a text box through the child window | Yes (the effect chooser's search) |
| Plugin Host is offered, since the JUCE engine registers it | Yes |
| A VST3 loads through Browse... (DelayChorus) and is listed | Yes |
| Open Plugin shows the plugin's own window | Yes |
| The window shrinks to 2100x1300 and the UI lays out again | Yes: client 2075x1235, child 2076x1236 |
| Close quits cleanly | Yes, no crash events |

Nano Q after the refactor: the CLAP builds and `NanoQEngineDialogTests` passes. Nano Q itself
was not run (its standalone was in use), so its presenter and the engine-as-link path are built,
not exercised.

Not tried: a DAW (VST3/CLAP inside a host), keyboard focus there, a second monitor at another
DPI, macOS, the Jam tab's web view (built without WebView2 here, so it opens the browser),
zoom (no `getZoom`/`setZoom` from JUCE yet, so the zoom control is hidden).

## What this means

- **The UI can be written once.** Only `UiLink` and `EmbeddedUi` know there is more than one
  shell, and the JUCE half is one small editor class.
- **JUCE becomes a shell, not the UI.** It is still needed for what only it does today: hosting
  third-party plugins, AUv3, Android. Hosting moved into core without JUCE would make the JUCE
  shell optional; nothing in the UI would change.
- **Android** keeps the web UI either way: Elements has no Android host.

## Problems found

- **Two Elements binaries in one process collide on Windows.** Elements registers one window
  class, `ElementsView`, against the host executable's module (`GetModuleHandleW(nullptr)`), and
  `ELEMENTS_CLASS_PREFIX` renames only the macOS class. A second binary with Elements in the
  same DAW (Nano Q's CLAP and VST3 together, or Nano Q beside this build) fails `RegisterClassW`,
  shows a "Could not register class" box, and makes its windows with the first binary's window
  procedure, which goes when that binary unloads. Read from the source, not reproduced. It is
  Nano Q's problem today as much as this spike's; the fix is in Elements (a class name per
  binary, registered against the binary's own module).
- **JUCE's `HWNDComponent` cannot hold another library's window**: it destroys the window when it
  lets go. The editor places the window itself.

## Next steps, if this goes ahead

1. Elements: a window class per binary (above), upstream or as a patch beside
   `elements-input-redraws.patch`.
2. Run the JUCE build in DAWs (Reaper, Cubase, Studio One): focus, DPI, editor reopen, two
   instances.
3. macOS: `EmbeddedUi` on an `NSView` parent (Elements' Cocoa host takes one), and
   `NSViewComponent`-style placement in the editor.
4. Zoom and the Jam tab's WebView2 in the JUCE shell.
5. Decide the product shape: one app with a UI switch, or the JUCE build's editor replaced.
