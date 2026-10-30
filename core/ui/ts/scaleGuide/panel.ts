/**
 * The Scale Guide in Jam → Scales (docs/plans/scale-guide.md): the controls, and the neck and chord
 * boxes the engine's view model is drawn into. Every change is saved to the `scaleGuide.view` app
 * setting and sent as a `scaleGuide` request; the answer (`scaleGuideView`) carries the request's
 * id, and only the latest is drawn, so a fast run of clicks never paints an older answer last.
 */

import { postMessage } from "../bridge.js";
import type { IncomingPayload } from "../messages/types.js";
import { escapeHtml } from "../utils.js";
import { renderChordBoxSvg } from "./chordBox.js";
import { renderNeckSvg } from "./neck.js";
import { loadSettings, MAX_CAPO, saveSettings, viewRequest } from "./state.js";
import type { ScaleGuideCatalog, ScaleGuideLabels, ScaleGuideMode, ScaleGuideSettings, ScaleGuideView } from "./types.js";

const FALLBACK_KEYS = ["C", "C♯/D♭", "D", "D♯/E♭", "E", "F", "F♯/G♭", "G", "G♯/A♭", "A", "A♯/B♭", "B"];
const INSTRUMENT_GROUPS: ReadonlyArray<[string, string]> = [
  ["guitar", "Guitar"],
  ["bass", "Bass"],
  ["ukulele", "Ukulele"],
];
/** What a diatonic chord's quality is called in the chord list. */
const DIATONIC_CHORDS: Readonly<Record<string, string>> = { maj: "maj", m: "m", dim: "dim", aug: "aug" };

let settings: ScaleGuideSettings | null = null;
let catalog: ScaleGuideCatalog | null = null;
let view: ScaleGuideView | null = null;
let latestRequestId = 0;
let catalogRequested = false;
let pickersBuilt = false;
let bound = false;

function element<T extends HTMLElement>(id: string): T | null {
  return document.getElementById(id) as T | null;
}

function current(): ScaleGuideSettings {
  settings ??= loadSettings();
  return settings;
}

/** Draws the guide into its section; called whenever Jam → Scales is shown. */
export function renderScaleGuidePanel(): void {
  const root = element<HTMLElement>("scale-guide");

  if (!root) {
    return;
  }

  if (!bound) {
    bindEvents(root);
    bound = true;
  }

  if (!catalog && !catalogRequested) {
    catalogRequested = true;
    postMessage({ type: "scaleGuide", action: "catalog" });
  }

  renderControls();
  requestView();
}

function requestView(): void {
  latestRequestId += 1;
  postMessage({ type: "scaleGuide", action: "view", requestId: latestRequestId, request: viewRequest(current()) });
}

/** Applies a change: saved, drawn and asked for again. A new scale, chord, tuning or mode starts on the whole neck. */
function change(patch: Partial<ScaleGuideSettings>): void {
  const before = current();
  const next = { ...before, ...patch };
  const reshaped = ["mode", "scale", "chord", "tuning"].some(
    (key) => key in patch && patch[key as keyof ScaleGuideSettings] !== before[key as keyof ScaleGuideSettings],
  );

  if (reshaped && !("position" in patch)) {
    next.position = 0;
  }

  settings = next;
  saveSettings(next);
  renderControls();

  // Show the chosen position at once; the engine's answer then dims the rest of the neck.
  if ("position" in patch) {
    document.querySelectorAll<HTMLElement>("#scale-guide [data-sg-position]").forEach((button) => {
      const chosen = Number(button.dataset.sgPosition) === next.position;
      button.classList.toggle("active", chosen);
      button.setAttribute("aria-pressed", String(chosen));
    });
  }

  // Labels and handedness only change how the same model is drawn.
  const redrawOnly = Object.keys(patch).every((key) => key === "labels" || key === "leftHanded");

  if (redrawOnly && view) {
    renderView();
    return;
  }

  requestView();
}

export function onScaleGuideCatalog(payload: IncomingPayload): void {
  if (!Array.isArray(payload.scales) || !Array.isArray(payload.tunings)) {
    return;
  }

  catalog = payload as unknown as ScaleGuideCatalog;
  pickersBuilt = false;
  renderControls();
}

export function onScaleGuideView(payload: IncomingPayload): void {
  if (payload.requestId !== latestRequestId || typeof payload.view !== "object" || payload.view === null) {
    return;
  }

  view = payload.view as ScaleGuideView;

  // The engine falls back to the whole neck for a position the new scale or tuning lacks.
  if (settings && settings.position !== view.position) {
    settings = { ...settings, position: view.position };
  }

  renderView();
}

function bindEvents(root: HTMLElement): void {
  root.addEventListener("click", (event) => {
    const target = (event.target as HTMLElement | null)?.closest<HTMLElement>("[data-sg-mode],[data-sg-labels],[data-sg-position],[data-sg-chord-root]");

    if (!target || !root.contains(target)) {
      return;
    }

    const data = target.dataset;

    if (data.sgMode) {
      change({ mode: data.sgMode as ScaleGuideMode });
    } else if (data.sgLabels) {
      change({ labels: data.sgLabels as ScaleGuideLabels });
    } else if (data.sgPosition !== undefined) {
      change({ position: Number(data.sgPosition) });
    } else if (data.sgChordRoot !== undefined) {
      // A chord of the scale: open it in Chords mode.
      change({ mode: "chord", key: Number(data.sgChordRoot), chord: DIATONIC_CHORDS[data.sgChordQuality ?? ""] ?? "maj" });
    }
  });

  element<HTMLSelectElement>("sg-key")?.addEventListener("change", (event) => change({ key: Number((event.target as HTMLSelectElement).value) }));
  element<HTMLSelectElement>("sg-scale")?.addEventListener("change", (event) => change({ scale: (event.target as HTMLSelectElement).value }));
  element<HTMLSelectElement>("sg-chord")?.addEventListener("change", (event) => change({ chord: (event.target as HTMLSelectElement).value }));
  element<HTMLSelectElement>("sg-tuning")?.addEventListener("change", (event) => change({ tuning: (event.target as HTMLSelectElement).value }));
  element<HTMLSelectElement>("sg-capo")?.addEventListener("change", (event) => change({ capo: Number((event.target as HTMLSelectElement).value) }));
  element<HTMLInputElement>("sg-left-handed")?.addEventListener("change", (event) => change({ leftHanded: (event.target as HTMLInputElement).checked }));
}

function buildPickers(): void {
  const keySelect = element<HTMLSelectElement>("sg-key");

  if (keySelect && (keySelect.options.length === 0 || (catalog && !pickersBuilt))) {
    const names = catalog?.keys.length === 12 ? catalog.keys : FALLBACK_KEYS;
    keySelect.innerHTML = names.map((name, pc) => `<option value="${pc}">${escapeHtml(name)}</option>`).join("");
  }

  const scaleSelect = element<HTMLSelectElement>("sg-scale");
  const chordSelect = element<HTMLSelectElement>("sg-chord");
  const tuningSelect = element<HTMLSelectElement>("sg-tuning");
  const capoSelect = element<HTMLSelectElement>("sg-capo");

  if (capoSelect && capoSelect.options.length === 0) {
    const capos = Array.from({ length: MAX_CAPO + 1 }, (_, fret) => `<option value="${fret}">${fret === 0 ? "None" : fret}</option>`);
    capoSelect.innerHTML = capos.join("");
  }

  if (!catalog || pickersBuilt || !scaleSelect || !chordSelect || !tuningSelect) {
    return;
  }

  const groups = [...new Set(catalog.scales.map((scale) => scale.group))];
  scaleSelect.innerHTML = groups
    .map((group) => {
      const options = catalog!.scales
        .filter((scale) => scale.group === group)
        .map((scale) => `<option value="${escapeHtml(scale.id)}" title="${escapeHtml(scale.formula)}">${escapeHtml(scale.name)}</option>`);
      return `<optgroup label="${escapeHtml(group)}">${options.join("")}</optgroup>`;
    })
    .join("");
  chordSelect.innerHTML = catalog.chords.map((chord) => `<option value="${escapeHtml(chord.id)}">${escapeHtml(chord.name)}</option>`).join("");
  tuningSelect.innerHTML = INSTRUMENT_GROUPS.map(([instrument, label]) => {
    const options = catalog!.tunings
      .filter((tuning) => tuning.instrument === instrument)
      .map((tuning) => `<option value="${escapeHtml(tuning.id)}">${escapeHtml(tuning.name)}</option>`);
    return options.length ? `<optgroup label="${escapeHtml(label)}">${options.join("")}</optgroup>` : "";
  }).join("");
  pickersBuilt = true;
}

function renderControls(): void {
  const state = current();
  const chordMode = state.mode === "chord";
  buildPickers();

  document.querySelectorAll<HTMLElement>("#scale-guide [data-sg-mode]").forEach((button) => {
    button.classList.toggle("active", button.dataset.sgMode === state.mode);
    button.setAttribute("aria-pressed", String(button.dataset.sgMode === state.mode));
  });
  document.querySelectorAll<HTMLElement>("#scale-guide [data-sg-labels]").forEach((button) => {
    button.classList.toggle("active", button.dataset.sgLabels === state.labels);
    button.setAttribute("aria-pressed", String(button.dataset.sgLabels === state.labels));
  });

  const keyLabel = element<HTMLElement>("sg-key-label");

  if (keyLabel) {
    keyLabel.textContent = chordMode ? "Root" : "Key";
  }

  element<HTMLElement>("sg-scale-field")?.toggleAttribute("hidden", chordMode);
  element<HTMLElement>("sg-chord-field")?.toggleAttribute("hidden", !chordMode);

  const setValue = (id: string, value: string): void => {
    const select = element<HTMLSelectElement>(id);

    if (select && Array.from(select.options).some((option) => option.value === value)) {
      select.value = value;
    }
  };

  setValue("sg-key", String(state.key));
  setValue("sg-scale", state.scale);
  setValue("sg-chord", state.chord);
  setValue("sg-tuning", state.tuning);
  setValue("sg-capo", String(state.capo));

  const leftHanded = element<HTMLInputElement>("sg-left-handed");

  if (leftHanded) {
    leftHanded.checked = state.leftHanded;
  }

  const positionLabel = element<HTMLElement>("sg-position-label");

  if (positionLabel) {
    positionLabel.textContent = chordMode ? "Shape" : "Position";
  }
}

function renderPositions(model: ScaleGuideView): string {
  const chordMode = model.mode === "chord";
  const count = chordMode ? (model.shapes?.length ?? 0) : (model.positions?.length ?? 0);
  const segment = (position: number, label: string, title: string, hint = ""): string =>
    `<button type="button" class="sg-segment${model.position === position ? " active" : ""}" data-sg-position="${position}" aria-pressed="${model.position === position}" title="${escapeHtml(title)}">${escapeHtml(label)}${hint ? `<small>${escapeHtml(hint)}</small>` : ""}</button>`;
  const segments = [segment(0, "All", chordMode ? "Every chord tone on the neck" : "The whole neck")];

  for (let position = 1; position <= count; position += 1) {
    const caged = chordMode ? (model.shapes?.[position - 1]?.caged ?? "") : "";
    segments.push(segment(position, String(position), caged ? `${caged} shape` : `${chordMode ? "Shape" : "Position"} ${position}`, caged));
  }

  return segments.join("");
}

/** The middle of the summary: the scale's chords, or the chord's formula and shape. */
function renderInfo(model: ScaleGuideView): string {
  if (model.mode === "chord" && model.chord) {
    const pills = model.tones.map(
      (tone) => `<span class="sg-pill${tone.root ? " sg-pill-root" : ""}"><b>${escapeHtml(tone.interval)}</b>${escapeHtml(tone.name)}</span>`,
    );
    return pills.join("");
  }

  return (model.scale?.diatonic ?? [])
    .map(
      (chord, index) =>
        `<button type="button" class="sg-pill${index === 0 ? " sg-pill-root" : ""}" data-sg-chord-root="${chord.rootPitchClass}" data-sg-chord-quality="${escapeHtml(chord.quality)}" title="${escapeHtml(chord.numeral)}: open in Chords">${escapeHtml(chord.name)}</button>`,
    )
    .join("");
}

function setText(id: string, text: string): void {
  const target = element<HTMLElement>(id);

  if (target) {
    target.textContent = text;
  }
}

function renderSummary(model: ScaleGuideView): void {
  const chordMode = model.mode === "chord";
  const root = model.tones[0]?.name ?? "";
  const names = model.tones.map((tone) => tone.name);
  const shape = chordMode && model.position > 0 ? model.shapes?.[model.position - 1] : undefined;

  setText("sg-root-badge", root);
  setText("sg-summary-label", chordMode ? "Chord" : "Scale");
  setText("sg-summary-name", chordMode ? `${model.chord?.name ?? ""} · ${model.chord?.quality ?? ""}` : (model.scale?.name ?? ""));
  // A scale reads up to its octave; a chord is just its tones.
  setText("sg-tones", (chordMode ? names : [...names, root]).join("  "));
  setText("sg-info-label", chordMode ? "Formula" : model.scale?.diatonic.length ? "Chords" : "Steps");
  setText(
    "sg-reference",
    chordMode
      ? shape
        ? `${shape.caged ? `The ${shape.caged} shape. ` : ""}Numbers are fingers; × is a string you don't play.`
        : "Pick a shape to see one way to play it; All shows every chord tone."
      : `Steps ${model.scale?.steps ?? ""}. Pick a position for one box; All shows the whole neck. Tap a chord to open it.`,
  );

  const info = element<HTMLElement>("sg-info");

  if (info) {
    info.innerHTML = !chordMode && !model.scale?.diatonic.length ? `<span class="sg-steps">${escapeHtml(model.scale?.steps ?? "")}</span>` : renderInfo(model);
  }
}

function renderView(): void {
  if (!view) {
    return;
  }

  const state = current();
  const model = view;
  setText("sg-title", `${model.title} · ${model.tuning.name}`);

  const positions = element<HTMLElement>("sg-positions");

  if (positions) {
    positions.innerHTML = renderPositions(model);
  }

  const neck = element<HTMLElement>("sg-neck");

  if (neck) {
    neck.innerHTML =
      model.mode === "chord" && (model.shapes?.length ?? 0) === 0
        ? `${renderNeckSvg(model, { labels: state.labels, leftHanded: state.leftHanded })}<p class="sg-note">No playable shape in this tuning: every chord tone is shown.</p>`
        : renderNeckSvg(model, { labels: state.labels, leftHanded: state.leftHanded });
  }

  const boxes = element<HTMLElement>("sg-boxes");

  if (boxes) {
    const shapes = model.mode === "chord" ? (model.shapes ?? []) : [];
    boxes.toggleAttribute("hidden", shapes.length === 0);
    boxes.innerHTML = shapes
      .map(
        (shape, index) =>
          `<button type="button" class="sg-box${model.position === index + 1 ? " active" : ""}" data-sg-position="${index + 1}" aria-pressed="${model.position === index + 1}">${renderChordBoxSvg(shape, model, { leftHanded: state.leftHanded })}</button>`,
      )
      .join("");
  }

  renderSummary(model);
}

/** For tests: forget everything, as on a fresh page. */
export function resetScaleGuideForTests(): void {
  settings = null;
  catalog = null;
  view = null;
  latestRequestId = 0;
  catalogRequested = false;
  pickersBuilt = false;
  bound = false;
}
