import { escapeHtml } from "./utils.js";
/**
 * Canonical tag vocabulary offered wherever the user can tag a preset or a
 * Multi-Rig mix: the Save Preset modal, the Save Multi-Rig modal, the Tone
 * Sharing publish modal, and the preset library's tag filter bar. Keep this
 * list as the single source of truth — each picker used to hardcode its own
 * (slightly different) copy in markup, which let them drift out of sync.
 */
export const STANDARD_TAGS: readonly string[] = [
  "lead",
  "rhythm",
  "bass",
  "clean",
  "crunch",
  "high-gain",
  "ambient",
  "atmospheric",
  "live",
  "studio",
  "stereo",
  "dual-amp",
];

/**
 * Fills a tag-picker container with one button per tag, using the given
 * chip class. Safe to call on an already-populated container — existing
 * children are replaced. Tags are escaped: a preset's own tags come from
 * whoever shared it.
 */
export function renderTagChips(container: HTMLElement, tags: readonly string[], chipClass: string): void {
  container.innerHTML = tags
    .map((tag) => `<button type="button" class="${escapeHtml(chipClass)}" data-tag="${escapeHtml(tag)}">${escapeHtml(tag)}</button>`)
    .join("");
}
