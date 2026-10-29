# Nano Q against the web UI: what is still unfinished

Status: **done, 2026-10-29** (see Outcome). Reviewed at `835b1d88`. Follows [nano-q-parity.md](nano-q-parity.md), whose
phases are all marked done or mostly done. This is a visual pass: each surface opened in both
UIs at the same size and compared side by side, looking for places where Nano Q still differs
from the web UI enough to read as unfinished. Small differences, and places where Nano Q's
version is as good or clearer, are left out or listed under "Acceptable".

## Outcome, 2026-10-29

Every item below has been done, each in its own commit on main (15 and 20 share one, as do 1, 9
and 19, and 17 and 18):

| Item | Commit | What changed, and what was left out |
| --- | --- | --- |
| 1, 9, 19 | `694e966c` | Effect editor: artwork panel, stacked model slot, tabs under it, raised groups with larger knobs, two-line header, CALIBRATED chip, the right category label, the gate's NOISE FLOOR box |
| 2 | `f88c74c4` | Performance pads under the chain (Setlist page at the compact size); the old Presets page is gone. Left out: the experimental Effects pads, and press-and-hold on a pad's save button (a tap that asks before replacing) |
| 3 | `f2b26034` | Preset chooser as a popover: + Mixer, folder paths, export, folder tools, Setlists folded at the foot |
| 4 | `caaa1b77` | Settings in sections and cards, drop-downs and switches; the theme only in the footer. Left out: the DSP diagnostics card (Nano Q has the DSP flyout) and the Tone3000 API health check |
| 5 | `12f573a6` | Tuner rewritten, with Cents / Hz |
| 6 | `f1c0c0fd` | The + menu and the FX Library over the editor, with drag onto the chain. "Supercharged..." is a composite (`composite:supercharged-neural-amp`), now offered. Nano Q keeps click-to-add beside the drag |
| 7 | `809d25ac` | One dialog chrome (`ShowDialog`, `ui/DialogChrome.h`) for every dialog |
| 8 | `934c429a` | Chain cards: A2 badge, equipment art, remove and bypass corners |
| 10 | `2da69663` | Save dialog: Details / Advanced, the input peak |
| 11 | `d1321548` | Global EQ as a dialog |
| 12 | `e9a914e2` | Gate flyout as labelled rows, switches, Advanced, Reset |
| 13 | `726d2a64` | Model browser rows, row actions, All / Favourites, previous / next, footer hint. The hashed file names show in the web UI too: they are the library's data. Also fixed: the list had collapsed to one row since item 7, and Esc now cancels a dialog as its cross does |
| 14 | `a6dcd08b` | Export, Import and Delete preset (engine: `importPresetArchiveFile`) |
| 15, 20 | `b45e439c` | Tones and Jam side lists; My Content, Create Pack, Cards / List; Publish says that publishing is Soundshed Guitar's |
| 16 | `258745d1` | Input mode menu with icons and descriptions |
| 17, 18 | `d6ba766a` | EQ tile and CONTROLS light up; Edit and Save follow whether the preset can be modified |
| 21 | `b4e530ee` | Meter list in two columns |

Also on the way: `d4d9411f` (the factory archive re-import kept dropping stored metadata, which hid
the A2 and CALIBRATED badges). Not yet tried live: export and the native file pickers, the Practice
EQ's Reset, and the hosted-plugin and other effect editors.

## How it was compared

- **Web UI:** the Release Soundshed Guitar standalone, driven over CDP, its viewport emulated
  at Nano Q's size: 1472 x 877 for the full layout and 960 x 600 for the compact one.
- **Nano Q:** the Release standalone, driven with the DevDriver (`click`, `key`, `shot`).
- **Same data in both:** the web standalone was started with `APPDATA=C:\t\webapp`, where
  `C:\t\webapp\Soundshed Guitar` is a junction to Nano Q's test profile `C:\t\nqprof`. Both
  apps read the same presets, library and settings, and the real profile is not touched.
  Preset: Black Album (Full Rig), scenes Rhythm and Solo.

## Unfinished: large

1. **Effect editor body (every effect).** The artwork is cut down to a short strip as tall as
   the controls (the amp shows only its cab; the TS9 is cut through its knobs). The web UI gives
   it a panel of its own, about 300 x 260. The editor stops under the controls, about 190 px
   tall, where the web editor is about 310. MAIN / ADVANCED sit at the right of the model row
   instead of under it, and the parameter groups are not drawn as raised cards with larger
   knobs.
2. **Pads view.** The footer's grid button opens the Setlists list. "Pads" there shows eight
   small flat buttons. The web UI swaps the editor for the performance pads and keeps the chain
   above them:
   - large cards with the slot number, the preset name, CURRENT or TAP TO LOAD, and a load
     button
   - empty slots drawn dashed, reading "Empty Path / Assign in setlist"
   - a bank header card (BANK 1, its name, "2 of 2 paths visible", Edit), with First and Last
     either side
   - Add bank, the Setlist toggle and the PADS count picker
3. **Preset chooser.** It is a full page in Nano Q, which also drops the PLAY tab's highlight,
   and Esc does not close it. The web UI uses a popover over the chain. Missing from Nano Q:
   - "+ Mixer" and "In Mixer" on each preset, to add it to a Multi-Rig
   - each row's folder path ("Preset Pack Vol. 1 > High Gain Rhythm")
   - the export button in the header
   - the folder tools: a New Folder Name field, and create, rename and delete buttons
   - the Setlists section folded at the bottom
4. **Settings.** Nano Q shows one long page. The web UI has a section list (General, Audio &
   MIDI, DSP Performance, Features, Help) with each group in a card. Other differences:
   - the order differs (Nano Q starts with the audio device)
   - choices are segmented buttons where the web UI has drop-downs ("Preset switch tails",
     "Carry delay & reverb tails")
   - switches are drawn as dot buttons
   - Theme is in Settings, where the web UI keeps it only in the footer
5. **Tuner.** Still the old plain dialog: the "Tuner" title with an orange Done, a tick scale,
   "Mute while tuning", and − / A = 440 Hz / +. The web UI has:
   - the card header, with a speaker icon and ×
   - −50 / +50 over a puck that slides on a line, with the note and its neighbours under it
   - a footer with Cents / Hz, a Live Tuner switch and a 440.0 number field
   - **Missing outright:** the Cents / Hz choice.
6. **Effect chooser and the chain's +.**
   - The + opens the chooser directly. The web UI first shows a small menu (Add FX…, Add
     Scene…).
   - The web UI's Add FX… opens the FX Library over the editor: cards in a grid that are
     dragged onto the chain, a folder button on NAM entries to browse models, and Close.
   - Nano Q's chooser is a click-to-add list pinned to the top of the window, over the
     control bar.
   - Amplifiers lists 3 in Nano Q and 4 in the web UI. The web UI also lists "Supercharged…",
     with a gear icon and a hex badge. It is probably a WASM amp; this needs confirming.

## Unfinished: medium

7. **Dialog chrome, across the app.** The MIDI & Automation, Tuner, Save, menu and prompt
   dialogs have a title and an orange Done or Save button. They have no header band with ×,
   and no footer band. The web UI's checkboxes (MIDI's two options, the limiter, "Mute while
   tuning") are drawn as dot buttons. The metronome has the web chrome since `835b1d88`; a
   shared card frame from `ui/MetronomeWidgets.h` (`CardFace`, `CloseCross`) could give every
   dialog the same chrome.
8. **Chain cards.**
   - No NAM architecture badge (A2) on NAM cards or after the editor's subtitle. phase 6 says
     the engine stores `architectureVersion`, but nothing is drawn here.
   - No equipment-art thumbnail on cards whose effect has one (the Doubler shows its icon).
   - The selected card has no remove (×) and bypass buttons in its corners.
9. **Effect editor header.**
   - No CALIBRATED badge on a calibrated NAM node.
   - **Wrong category label:** the TS9 (Neural FX) reads "AMP · NEURAL FX (NAM)"; the web UI
     says "FX · NEURAL FX (NAM)".
   - The subtitle sits beside the title instead of under it.
10. **Save As dialog.**
    - No Details / Advanced tabs.
    - No Input Peak Level section (Raw Input Peak over 10 s, Designed Peak, Apply Current).
    - A large empty band under the buttons: the dialog is taller than what it holds.
11. **Global EQ.** A flyout under the EQ tile, where the web UI uses a dialog. Missing:
    - the header's Enabled switch and ×
    - the "±12 dB" scale label
    - the "Parametric EQ" heading, with each band in its own boxed card
    - the square handles that mark each band's width (Nano Q draws small dots)
12. **Global gate settings flyout.** The controls are a row of knobs, where the web UI has
    labelled rows. Mode and Stereo Link are knobs (Stereo Link reads "1") where they should be
    switches. Missing: the on/off switch in its header, the Advanced group (Hysteresis, Range,
    Stereo Link), and "Reset to defaults".
13. **Model and IR browser.**
    - Rows give only a category ("pedal", "amp"). The web UI shows creator, architecture,
      gear and genre ("by csobrincos · A2 · Diezel Herbert · Metal").
    - A row's hover actions are missing (edit, info, delete, preview ▶); only the star is
      there.
    - Missing: the ◀ ▶ buttons, the All / Favourites switch, and the footer hint ("Select an
      item to preview…, double click to select").
    - **To check:** some rows show hashed file names
      ("0a1b6589…-Ibanez-TS9-Tube-Screamer-…"). The web UI was filtered to full-rig, so it is
      not yet known whether it shows the same rows.
14. **Preset "…" menu.** No Export preset, Import preset or Delete preset (phase 2's "Left").
15. **Publish.** The full layout's PUBLISH button and the compact layout's share button are
    missing, and so are the Tones page's My Content and "+ Create Pack". Publishing stays in
    Soundshed Guitar by the parity plan, but Nano Q does not say so anywhere a user would look
    for it.

## Unfinished: small

16. **Input mode menu.** No heading, routing icons or descriptions ("Input 1 on both sides",
    and the rest); just the five names.
17. **EQ tile and the Controls button don't light.** The control bar's EQ tile is not lit in
    the accent while the global EQ is on, and in the compact layout CONTROLS stays unlit while
    its strip is open.
18. **Edit and Save stay enabled on a factory preset;** the web UI greys them out.
19. **The gate node's noise floor.** The meter and Learn sit inside the MAIN tab, with no
    "NOISE FLOOR" title. The web UI puts them in a box of their own above the tabs.
20. **Tones and Jam.** They use tabs across the top where the web UI has a side list with a
    help card under it; Tones also lacks the Cards / List switch.
21. **The metronome's meter list** is one column where the web UI has two (simple, and
    compound and odd).

## Acceptable as they are

- The output status flyout (a close match).
- Tones cards: Nano Q tints each card with its artwork's colour, which reads well.
- The footer keeps the demo clip, Rec and tempo in the compact layout, where the web UI moves
  them to Options (nano-q.md records the choice).
- Options adds input/output, mute and the layout choice.
- The editor header's DSP button is a text button where the web UI has a gauge icon.
- Rec is a ring before the grid button in Nano Q, a red dot after MIDI in the web UI.
- The MIDI & Automation tables match apart from the chrome (item 7).

## Not covered by this pass

The light and classic themes; Multi-Rig tabs and the mixer; the IR cabinet, EQ, delay and
modulation editors, the IR alignment panel and the Signal Analyzer; blends and custom layouts;
riff capture and the Riff Library (Rec was not pressed, so nothing was armed); the looper (its
feature switch is off in this profile, in both UIs); the Practice Tool; the phone layout; and
Nano Q as a plugin inside a DAW.
