# Blend editor review (2026-10-20)

**Status:** implemented the same day, all seven steps of "Suggested order". Two departures
from the proposal: Add Models is the editor's own multi-select list of the library (with a
Tone3000… button into the resource picker), because the picker selects one resource at a
time; and the legacy `parameterId`/`parameterValue` are still written, because the engine
places a model on the sweep by them (`BlendSupport.cpp`, `BuildMappedModelRef`).

How the blend editor works today, what makes it heavy, and a proposal for a one-screen
editor that also builds blends from more than one pack. Reviewed from the code
(`core/ui/ts/blendEditor.ts`, `ui-components/modals/blend-editor-modal.html`,
`blend-model-browser-modal.html`, `ts/blendManager.ts`, `ts/settings/blendGroups.ts`) and
by driving the running app on the user's own blend library.

## What it is today

A blend is a list of NAM models, each placed at values of one or more parameters (Gain,
Tone, ...). The node turns those parameters into knobs and plays the nearest model (Snap) or
crossfades the two nearest (Interpolate).

The editor is a modal with two tabs:

- **Blend Settings**: Name, Category, Blend Mode, a "Mapped Parameters" section (chips with
  ×, a dropdown, Add Parameter, Auto Map), then one card per model: Browse, the model name,
  Auto, Remove, and a number field per mapped parameter.
- **Test Blend**: a "Matched Model" box and a knob per mapped parameter.
- Footer: Export .namz, Import .namz, Cancel, Apply, Save Blend, all styled the same.

Picking a model opens a second modal, a model browser of its own, with a tone-group
scope, a type filter, search, and the raw resource id under each name.

Ways in: Edit Blend on a node, Edit in Settings → Blends, Create Blend on a library tone
group (Settings), Import as blend in the Settings Tone3000 browser, and Create Blend in the
resource picker's Tone3000 tab.

Measured on the live app (1430 x 921 window):

| Blend | Models | Editor body height | Screens of scrolling |
| --- | --- | --- | --- |
| TS808 | 4 | 882 px in a 645 px viewport | 1.4 |
| 37-model bass preamp pack | 37 | 4,585 px | 7 |

`blendEditor.ts` is 1,506 lines, the largest UI file after `resourceBrowser.ts` and
`signalPath.ts`; about 200 of those lines are the private model browser.

## Findings, most costly first

1. **A model card is three controls deep, and one card per model does not scale.** Each
   model takes ~170 px: Browse, name, Auto, Remove, then its parameter fields on a second
   line. The 37-model blend is seven screens of cards, in import order rather than value
   order, so G7 comes before G1.

2. **The Test tab repeats the node.** Opened from a node, its knobs are the node's own knobs
   (the note says so), and the node panel now shows the same "Playing:" line. Opened from
   Settings it can only say which model would be picked, not let you hear it. The tab exists
   because edits are not heard until Apply.

3. **Models are added one at a time.** Add Model makes an empty card, then Browse opens the
   browser for that one card. A ten-model blend is ten round trips. The browser does not mark
   models already in the blend, so the same model can go in twice.

4. **Parameters are managed apart from the values they hold.** You add "Tone" in the Mapped
   Parameters section, then fill a Tone field in every card. Removing the last chip silently
   puts Gain back. Auto Map rebuilds the parameter set from names, and each card also has its own
   Auto, so there are two ways to infer values from names, and neither runs when a model is added.

5. **Nothing says when a blend is ambiguous.** The 37-model pack holds several voicings
   (different model-name prefixes), all mapped on Gain alone, so several models sit at Gain 0
   and the knob cannot reach most of them. The library also holds two one-model blends, which
   can never blend. The editor saves all of these without comment.

6. **The footer has five equal buttons, and two belong elsewhere.** Import .namz in an Edit
   dialog creates a different, new blend; Export exports the saved copy, not the edits on
   screen. Both are library actions (Settings → Blends), not editing ones.

7. **Category duplicates what the models already say,** and Blend Mode is a full-width form
   row although the node can override it anyway.

8. **Creation paths disagree.** Create Blend on a Settings tone group asks for the name with
   `prompt()` and the mode with "OK for snap, Cancel for interpolate", and saves without
   opening the editor. Tone3000 paths open the editor. Settings → Blends has no New Blend.

9. **Two copies of the snapping rule.** The Test view snaps with its own
   `findClosestBlendMappingForParam`, while the node and "Matched Model" use
   `blendUtils.describeBlendMix`, the mirror of the engine. With more than one parameter they
   can disagree about which model plays.

10. **The DOM is the editor's state.** Mappings are read back out of the number fields
    (`collectCurrentMappings`) and every edit re-renders from them, which is why each action
    re-runs `renderModelList` + `renderTestControls` + `updateMatchedModel`.

## Mixing models from other packs

The data model already allows it: a blend is a list of resource ids, and nothing ties them
to one tone group. Today it is possible but hard:

- The model browser opens scoped to the blend's tone group, inferred from the *first*
  model, so other packs are one filter change away on every pick.
- `toneGroupId` is set from that first model, so a mixed blend claims to be one pack's.
  `findBlendForToneGroup` (Create Blend's "reopen the blend made before") keys on it.
- Values come from names (`G5`, `Gain 5`). Packs name their captures differently, and the
  same number does not mean the same thing across packs: Gain 5 on one amp is not Gain 5 on
  another. A mixed blend usually wants positions the user chooses, or a plain ordered sweep.
- The blend sweep already exists: with no parameter values, the node shows one **Blend**
  knob that moves through the models in list order. That is the natural mode for "amp A
  to amp B to amp C", but the editor never offers it, because it always keeps at least Gain.
- **Level is not matched across packs.** The blend levels each model with its calibration
  input/output levels when the capture has them (`MultiModelNAMAmpEffect::UpdateAutoGains`);
  captures without them get no correction. The model's `loudness` metadata is read
  (`NAMSampleRate.h`) but I found no use of it in the blend's gain. Two packs captured at
  different levels jump in volume as the knob crosses between them.

What mixing needs:

- An Add Models picker that is library-wide by default, with a pack filter rather than a pack
  scope, multi-select, and sources (Library, Tone3000) side by side. The resource picker
  already does all of this, including Tone3000 import, so reuse it instead of the private
  browser.
- An axis choice per blend: **Sweep** (order only; drag to reorder) or **Parameters** (Gain,
  Tone, ...; values per model, filled from names where the name says).
- A per-model level trim, and a "Match levels" action that sets the trims from `loudness`
  (or from a measurement, as `measurePresetLevel` does for presets). This one needs an engine
  field; everything else above is UI.
- `toneGroupId` only when every model shares it.

## Proposal: one screen, no tabs

```
Edit Blend                                                         ×
┌─────────────────────────────────────────────────────────────────┐
│ [TS808                         ]   Mode [Interpolate|Snap]       │
│ Axis  (•) Parameters  ( ) Sweep                                  │
├─────────────────────────────────────────────────────────────────┤
│ Model                                  Gain   Tone  Level  [+]   │
│ TS808 - G 0 T 5 L 10                    0.0    5.0   0 dB   ×    │
│ Tone3000 pack 66205 - G 0 T 10 L 10     0.0   10.0   0 dB   ×    │
│ Tone3000 pack 66205 - G 3 T 8 L 10      3.0    8.0  -2 dB   ×    │
│ Tone3000 pack 66205 - G 8 T 5 L 10      8.0    5.0   0 dB   ×    │
│ ⚠ 2 models share Gain 0 · Tone 5: the knobs reach only one       │
│ [+ Add models]   [Fill values from names]   [Match levels]       │
├─────────────────────────────────────────────────────────────────┤
│ Try:  (Gain) (Tone)   Playing: Mix: TS808 G0 T5 50% / G3 T8 50%  │
├─────────────────────────────────────────────────────────────────┤
│                                         [Cancel]  [Save]         │
└─────────────────────────────────────────────────────────────────┘
```

- **A table, not cards:** one ~32 px row per model, parameters as columns, sorted by value.
  The 37-model pack becomes ~1,200 px instead of 4,585. Clicking a name swaps that model.
- **Parameters are columns:** `[+]` adds one, × in its header removes it. No separate
  section, no Auto Map button: values are filled from names when a model is added, and
  "Fill values from names" redoes it on request.
- **Sweep axis:** no columns, drag rows to order; the node shows one Blend knob.
- **Try strip** replaces the Test tab: the knobs and the "Playing" line, in the same view as
  the values they test. Opened from a node, edits play on the node as they are made
  (the engine already rebuilds on `saveBlendDefinition`); Cancel restores the saved copy.
  Apply goes.
- **Inline warnings, not blockers:** fewer than two models, models sharing a position, a model
  with no value for a mapped parameter, a missing file.
- **Footer:** Cancel, Save. Export and Import move to Settings → Blends (Export per row,
  Import plus a New Blend at the top).
- **Category** goes, inferred from the models' gear (most common), with an override in the
  blend list if anyone needs it. **Mode** becomes a two-way toggle beside the name.
- **Creation:** every path ends in this editor, pre-filled. The tone-group path loses
  `prompt()` and the OK/Cancel mode question. A pack with several voicings offers one blend
  per voicing (grouped by the name with its parameter tokens removed), or one blend with all.

## Code that goes with it

- Keep the edit as data (`{ name, mode, axis, params[], rows[] }`) and render from it, rather
  than reading the DOM back.
- Drop the private model browser for `resourceBrowserModal` with multi-select; ~200 lines and
  the `blend-model-browser-modal.html` markup go.
- Drop the Test tab's own snapping (`findClosestBlendMappingForParam`,
  `buildBlendMappedPoints`); the Try knobs use `describeBlendMix` like the node.
- Stop writing the legacy `parameterId`/`parameterValue` on save once the engine reads
  `parameters` alone (check `BlendSupport` first).
- Expect `blendEditor.ts` to land well under its 1,506-line pin; split the table into its own
  module if it does not.

## Suggested order

1. Table layout, parameters as columns, fill from names on add, sort, inline warnings
   (UI only; biggest win).
2. Try strip with live audition, remove the Test tab and Apply.
3. Add Models through the resource picker, multi-select, library-wide with a pack filter;
   `toneGroupId` only when shared. This is the cross-pack step.
4. Sweep axis.
5. Footer cleanup: Export/Import/New Blend into Settings → Blends; one creation path.
6. Per-model level trim and Match levels (engine field + UI).
7. Split-by-voicing when creating from a big pack.
