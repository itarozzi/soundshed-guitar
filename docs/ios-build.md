# iOS and iPadOS Build

The iOS app is the JUCE **Standalone** target built for iOS, as the Android app
is ([android-build.md](android-build.md)). It runs the same `SoundshedGuitarCore`
DSP engine and, for Soundshed Guitar, the same web UI in a `WKWebView`; Soundshed
Guitar Nano draws its native JUCE UI. One universal app per product covers iPhone
and iPad.

There is no separate project to keep in step: `juce/CMakeLists.txt` builds for
iOS when configured with `-DCMAKE_SYSTEM_NAME=iOS` and the Xcode generator, and
`ios/build.sh` wraps that.

## Layout

```
ios/
  build.sh      Configures ios/build (Xcode), builds either app for the Simulator or a
                device, and optionally installs and launches it in the Simulator
  .gitignore    Ignores ios/build
```

Everything else is shared with the desktop and Android builds:

| File | iOS-specific part |
| ---- | ----------------- |
| `juce/CMakeLists.txt` | `SSG_MOBILE`, Standalone-only formats, no plugin hosting, WASM off, iOS 16.5 floor, the Info.plist options, a flat-bundle resource copy of only the shipped UI folders |
| `juce/cmake/SoundshedNano.cmake` | Nano's Info.plist options (all orientations) and its resource copy |
| `juce/source/Main.cpp` | `SOUNDSHED_MOBILE` window handling; `setFullScreen (true)`; the 128-frame default buffer |
| `juce/source/LaunchCurtain.*` | Compiled out on mobile |
| `juce/source/PluginProcessorAdapter.cpp` | `locateAssetsRoot` also looks beside the executable |
| `core/src/dsp/RealtimeParallel.h` | Parallel DSP off, as on Android |
| `core/CMakeLists.txt` | stftPitchShift fetched for its headers only; its own project (with a command-line tool) does not configure for iOS |

## Prerequisites

| Component | Version used | Notes |
| --------- | ------------ | ----- |
| Xcode | 26.3 | iOS 26.2 SDK |
| iOS Simulator runtime | 26.3.1 | `xcodebuild -downloadPlatform iOS` (about 8 GB) |
| CMake | 4.2 | JUCE needs ≥ 3.25 |
| Node / npm | as for desktop | Builds `core/ui/dist` before the WebView app is packaged |

The deployment target is **iOS 16.5**. That is the WebKit floor, as macOS's 11.0
is: the UI's `color-mix()` and CSS nesting need Safari 16.5, and on iOS Safari's
WebKit is the system's. Nano could go lower, but it shares the build tree.

## Building

```bash
ios/build.sh                      # Soundshed Guitar, Simulator, Debug
ios/build.sh nano run             # Nano, Simulator, then boot an iPad, install and launch
ios/build.sh guitar release run   # Release build in the Simulator
```

Arguments may appear in any order: `guitar|nano`, `sim|device`, `debug|release`,
`run`, `reconfigure`. The first run configures `ios/build`, which takes a few
minutes (it fetches the core's dependencies and builds JUCE's `juceaide`); later
runs only build. The app lands in
`ios/build/<Target>_artefacts/<Config>/Standalone/`. That folder is the same for
the Simulator and a device, so each build replaces the other's `.app` there.

After changing a CMake file, pass `reconfigure`. Xcode does re-run CMake on its
own, but only after it has loaded the old project for the current build, so that
build still runs the old build steps. A post-build copy that has moved goes on
writing to the old place, for instance.

`run` uses a booted Simulator if there is one, otherwise boots
`$SSG_IOS_SIMULATOR` (a UDID) or the first iPad the runtime lists, its largest
current model.

The Simulator runs the host's architecture, so on an Intel Mac the Simulator
build is x86_64 and on Apple silicon it is arm64; `build.sh` passes `uname -m`.

To work in Xcode instead, open `ios/build/SoundshedGuitar.xcodeproj` after the
first configure and pick the `SoundshedGuitar_Standalone` scheme. Nano has no
generated scheme (`juce/cmake/XcodePrettify.cmake` makes them for Soundshed
Guitar's targets only): use Product > Scheme > New Scheme and pick
`SoundshedGuitarNano_Standalone`, or build it with `ios/build.sh nano`.

**Use a release build for anything to do with audio**, as on Android: a debug
build compiles the DSP unoptimised.

### Signing for a device

The Simulator needs only an ad-hoc signature, so it builds with no Apple
account. A device build needs a development team:

```bash
SSG_IOS_DEVELOPMENT_TEAM=<team id> ios/build.sh device release reconfigure
```

The team id is written into the build tree, so later device builds need only
`ios/build.sh device release`. `-allowProvisioningUpdates` lets Xcode create the
development profiles for `com.soundshed.guitar` and `com.soundshed.guitar.nano`.
Install with Xcode (Window > Devices and Simulators) or
`xcrun devicectl device install app --device <id> <path to .app>`.

Distribution (TestFlight, the App Store) needs an archive and a distribution
profile; see "Archiving for iOS" in JUCE's `docs/CMake API.md`. Nothing here
sets that up yet.

## How it differs from the desktop builds

### Formats

Only the Standalone is built (`FORMATS` is `Standalone` whenever `SSG_MOBILE` is
on). iOS could take an **AUv3** of each product, which would let the engine run
inside AUM, GarageBand, Logic for iPad and the like. That is a separate piece of
work: an app extension needs its own bundle id, an app group if it is to share
the profile with the app, and an editor that copes with the host's view sizes.

Plugin hosting is off, as on Android. iOS can host AUv3s, but the app has never
been built or tested against that, so the format manager registers nothing.

### Features that are off

| Feature | Why |
| ------- | --- |
| WASM effect host | Wasmtime publishes no iOS C API bundle, and iOS allows no JIT |
| Hosting third-party plugins | See above |
| CLAP / VST3 / AU / AAX | Desktop formats |
| Launch curtain | One full-screen window; the system's launch screen covers start-up |
| Single-instance lock | iOS runs one instance per app anyway |

### The UI ships inside the app bundle

An iOS bundle is flat: the executable sits at the bundle's root, next to its
resources, rather than in `Contents/MacOS`. The post-build step copies the UI to
`<App>.app/ui`, and `PluginProcessorAdapter::locateAssetsRoot` adds the
executable's own directory as a candidate on iOS. The WebView editor and Nano's
`NanoContext` take the adapter's root before their own guesses, so that one
lookup covers all three.

It is not `<App>.app/resources/ui`, which every existing lookup would have found
unchanged: a directory called `resources` at the root of an iOS bundle makes
`codesign` take the bundle for a macOS one, and it fails with "code object is
not signed at all", naming whichever file it met first (an app icon, here).

It copies only the folders that ship (`GUITARFX_IOS_UI_DIRS`, the same list as
Android's `stageUiAssets`) plus `index.html` and the generated `build-flags.js`.
The desktop copy takes the whole of `core/ui`, which includes `node_modules`, the
TypeScript sources and the tests: tolerable on disk, but an app bundle carries
every file to the device and signs it.

Unlike Android, nothing has to be unpacked: the bundle is an ordinary read-only
directory, so the engine's factory presets, metronome clicks and layout images
resolve as files in place.

None of Android's WebView workarounds apply. `WKWebView` runs JUCE's integration
script natively (`WKUserScript`), takes a charset in the MIME type, has DOM
storage on, and sizes itself from its frame.

### Paths and the profile

`userApplicationDataDirectory` is the app container's `Library`, and the core's
`HOME/Library` resolves to the same place, so the profile is
`<container>/Library/Soundshed Guitar` and no override is needed.

Each iOS app has its own container, so **Soundshed Guitar and Nano do not share
a profile on iOS** the way they do on desktop. Sharing one would need an app
group (`APP_GROUPS_ENABLED`, `APP_GROUP_IDS`) and the profile moved into the
group container.

### Window, orientation and safe area

`MainWindow` calls `setFullScreen (true)` on iOS, the opposite of Android, where
that collapses the window. JUCE's iOS peer then takes the display's user area,
the screen less its safe-area insets, and takes it again on every display change,
so rotation and the iPhone's camera housing are handled by JUCE rather than by
CSS.

| | Soundshed Guitar | Nano |
| - | ---------------- | ---- |
| Orientations | Landscape only, as on Android | All (portrait upside-down on iPad only) |
| iPad multitasking | Off (`REQUIRES_FULL_SCREEN`) | On |
| Status bar | Hidden | System default |

An iPad app that allows Split View must allow every orientation, so the WebView
app's landscape lock and its opting out of multitasking go together. Nano lays
itself out for narrow and portrait windows already.

### Deep links

**Tone-sharing links (`soundshed://`) do not open the iOS app yet.** Two things
are missing:

- The scheme never reaches the Info.plist, on iOS or on macOS.
  `juce/CMakeLists.txt` passes it as `XCODE_EXTRA_PLIST_ENTRIES`, which is not a
  `juce_add_plugin` keyword (JUCE's is `PLIST_TO_MERGE`), and the built macOS
  app's Info.plist has no `CFBundleURLTypes` either.
- On macOS JUCE hands an opened URL to `anotherInstanceStarted`. Its iOS app
  delegate implements no `application:openURL:options:` (nor the scene
  equivalent), so even with the scheme registered the URL would not reach the
  app. That needs a JUCE patch or swizzling JUCE's delegate class.

## Audio

JUCE's iOS device opens an `AVAudioSession` in `PlayAndRecord` with
mix-with-others, speaker default, AirPlay and Bluetooth A2DP allowed. The
microphone prompt appears when the input is first opened; the text is the
`MICROPHONE_PERMISSION_TEXT` in the CMake files.

### Buffer size

JUCE asks iOS for 256 frames (512 in the Simulator). `createPluginHolder` asks
for **128** instead, as the preferred setup for any open without a saved setup;
a size chosen in Settings > Audio Device wins from then on, as on Android. 64 is
worth trying on a recent device.

### Background audio

Both apps declare the `audio` background mode, so the amp keeps running when
another app (a backing track, a lesson video) comes to the front. Without it iOS
suspends the app and the guitar goes silent the moment it leaves the screen.

### Parallel DSP is off

`kParallelDspSupported` is false on iOS for Android's reason: the render
callback is a time-constraint thread and the helper threads are not. Doing this
properly means the helpers joining the device's audio workgroup
(`os_workgroup`), which JUCE exposes through `AudioWorkgroup`; until then
everything runs on the callback thread.

### Inputs

As on Android, the built-in microphone is no guitar input; a USB or Lightning
audio interface is. iOS routes audio to the most recently connected interface
and JUCE follows route changes, so a hot-plugged interface works without a
restart, unlike on Android.

The Simulator routes the Mac's default input and output and has no low-latency
path: use it to check that the app boots and the UI works, not for tone or
latency.
