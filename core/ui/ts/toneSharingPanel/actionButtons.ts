/**
 * The icon buttons that appear on every tone card, and the busy state they show
 * while the action behind them is in flight.
 */

import { escapeHtml } from "../utils.js";
import type { ToneActionIcon } from "./types.js";

export function toneActionIconMarkup(icon: ToneActionIcon): string {
  switch (icon) {
    case "preview":
      return `<svg class="tone-sharing-btn-icon" viewBox="0 0 16 16" fill="currentColor" aria-hidden="true"><polygon points="3,2 14,8 3,14"/></svg>`;
    case "download":
      return `<svg class="tone-sharing-btn-icon" viewBox="0 0 16 16" fill="currentColor" aria-hidden="true"><path d="M8 1a1 1 0 011 1v6.172l1.586-1.586a1 1 0 111.414 1.414l-3.293 3.293a1 1 0 01-1.414 0L3.999 8.001a1 1 0 111.414-1.414L7.001 8.172V2A1 1 0 018 1z"/><rect x="2" y="12.5" width="12" height="2" rx="1"/></svg>`;
    case "share":
      return `<svg class="tone-sharing-btn-icon" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" aria-hidden="true"><circle cx="12.5" cy="3.5" r="1.8"/><circle cx="3.5" cy="8" r="1.8"/><circle cx="12.5" cy="12.5" r="1.8"/><path d="M5.2 7l5.6-2.7M5.2 9l5.6 2.7"/></svg>`;
    case "view":
      return `<svg class="tone-sharing-btn-icon" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" aria-hidden="true"><path d="M1.3 8s2.3-4.2 6.7-4.2S14.7 8 14.7 8s-2.3 4.2-6.7 4.2S1.3 8 1.3 8z"/><circle cx="8" cy="8" r="2.1"/></svg>`;
    case "approve":
      return `<svg class="tone-sharing-btn-icon" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m3.2 8.3 2.7 2.8 6-6.2"/></svg>`;
    case "reject":
      return `<svg class="tone-sharing-btn-icon" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" aria-hidden="true"><path d="M3.5 3.5 12.5 12.5M12.5 3.5l-9 9"/></svg>`;
    case "edit":
      return `<svg class="tone-sharing-btn-icon" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M2.2 11.8V13.8h2l6.7-6.7-2-2z"/><path d="m9.7 3.1 2 2"/></svg>`;
    case "delete":
      return `<svg class="tone-sharing-btn-icon" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M2.5 4.2h11"/><path d="M6 4.2V2.8h4v1.4"/><path d="M4.5 4.2l.7 8.5h5.6l.7-8.5"/></svg>`;
  }
}

export function renderToneIconButton(options: {
  kind: "action" | "pack-action";
  value: string;
  icon: ToneActionIcon;
  label: string;
  primary?: boolean;
  active?: boolean;
  disabled?: boolean;
  attrs?: Record<string, string>;
}): string {
  const className = [
    "btn",
    options.primary ? "btn-primary" : "btn-secondary",
    "tone-sharing-card-btn",
    "tone-sharing-card-btn--icon",
    options.active ? "is-active" : "",
  ].filter(Boolean).join(" ");
  const dataName = options.kind === "action" ? "data-action" : "data-pack-action";
  const attrs = options.attrs
    ? Object.entries(options.attrs)
      .map(([key, value]) => ` ${key}="${escapeHtml(value)}"`)
      .join("")
    : "";

  const disabledAttr = options.disabled ? " disabled" : "";
  return `<button class="${className}" type="button" ${dataName}="${escapeHtml(options.value)}" aria-label="${escapeHtml(options.label)}" title="${escapeHtml(options.label)}"${disabledAttr}${attrs}>${toneActionIconMarkup(options.icon)}</button>`;
}

export function setActionButtonBusy(button: HTMLButtonElement, busyLabel: string): () => void {
  const previousHtml = button.innerHTML;
  const previousDisabled = button.disabled;
  const previousMinWidth = button.style.minWidth;
  const previousTitle = button.getAttribute("title");
  const previousAriaLabel = button.getAttribute("aria-label");
  const width = button.getBoundingClientRect().width;
  const iconOnly = button.classList.contains("tone-sharing-card-btn--icon");

  if (width > 0) {
    button.style.minWidth = `${Math.ceil(width)}px`;
  }
  button.disabled = true;
  button.setAttribute("aria-busy", "true");
  button.setAttribute("title", busyLabel);
  button.setAttribute("aria-label", busyLabel);
  button.classList.add("is-busy");
  button.innerHTML = iconOnly
    ? `<span class="tone-sharing-btn-busy-spinner" aria-hidden="true"></span>`
    : `<span class="tone-sharing-btn-busy-spinner" aria-hidden="true"></span>${busyLabel}`;

  return () => {
    button.innerHTML = previousHtml;
    button.disabled = previousDisabled;
    button.style.minWidth = previousMinWidth;
    if (previousTitle === null) {
      button.removeAttribute("title");
    } else {
      button.setAttribute("title", previousTitle);
    }
    if (previousAriaLabel === null) {
      button.removeAttribute("aria-label");
    } else {
      button.setAttribute("aria-label", previousAriaLabel);
    }
    button.removeAttribute("aria-busy");
    button.classList.remove("is-busy");
  };
}

export function syncPackPreviewButtons(presetsEl: HTMLElement, activeItemId: string): void {
  presetsEl.querySelectorAll<HTMLElement>(".tone-sharing-pack-preset-row").forEach((row) => {
    row.classList.toggle("is-previewing", row.dataset.itemId === activeItemId);
  });

  presetsEl.querySelectorAll<HTMLButtonElement>("[data-pack-action='preview']").forEach((btn) => {
    const isThis = btn.dataset.itemId === activeItemId;
    btn.innerHTML = toneActionIconMarkup("preview");
    const label = isThis ? "Previewing preset" : "Preview preset";
    btn.setAttribute("title", label);
    btn.setAttribute("aria-label", label);
    btn.classList.toggle("is-active", isThis);
    btn.disabled = false;
    btn.removeAttribute("aria-busy");
    btn.classList.remove("is-busy");
    btn.style.minWidth = "";
  });
}

export function syncFeedPreviewButton(card: HTMLElement, isPreviewing: boolean): void {
  card.classList.toggle("is-previewing", isPreviewing);
  const btn = card.querySelector<HTMLButtonElement>("[data-action='preview']");
  if (!btn) {
    return;
  }
  btn.innerHTML = toneActionIconMarkup("preview");
  const label = isPreviewing ? "Previewing preset" : "Preview preset";
  btn.setAttribute("title", label);
  btn.setAttribute("aria-label", label);
  btn.classList.toggle("is-active", isPreviewing);
}
