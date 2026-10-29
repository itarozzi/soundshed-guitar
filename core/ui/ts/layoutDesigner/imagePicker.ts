/**
 * Image pickers that show the images: each designer <select> of library images (imageOptionsHtml)
 * gets a dropdown of thumbnails in its place.
 *
 * The <select> stays in the page, hidden, and remains what the properties code reads and binds:
 * picking a thumbnail sets its value and fires its change event, so no binding needs to know the
 * picker is there.
 */

import { resolveLayoutImageUrl } from "../layoutImages.js";
import { uiState } from "../state.js";
import { escapeHtml } from "../utils.js";

/** CSS pixels a thumbnail is drawn across, which picks the smallest copy of a large image. */
const THUMBNAIL_EDGE = 40;

const BOUND = "imagePickerBound";

function isImageSelect(select: HTMLSelectElement): boolean {
  return select.querySelector("option[data-layout-image]") !== null;
}

function thumbnailHtml(imageId: string): string {
  const url = imageId ? resolveLayoutImageUrl(imageId, THUMBNAIL_EDGE) : null;
  return url
    ? `<img class="layout-image-picker-thumb" src="${escapeHtml(url)}" alt="" loading="lazy">`
    : `<span class="layout-image-picker-thumb is-empty" aria-hidden="true"></span>`;
}

/** An option's name, and the image's kind and pixel size underneath where the library has them. */
function choiceHtml(option: HTMLOptionElement, emptyLabel?: string): string {
  const image = option.value ? uiState.layoutLibrary?.images.find((img) => img.imageId === option.value) : undefined;
  const name = image ? image.fileName || image.imageId : emptyLabel ?? option.textContent?.trim() ?? "";
  const facts = image
    ? [image.type && image.type !== "general" ? image.type : "", image.width && image.height ? `${image.width}×${image.height}` : ""].filter(Boolean).join(" · ")
    : "";
  return `${thumbnailHtml(option.value)}
      <span class="layout-image-picker-text">
        <span class="layout-image-picker-name" title="${escapeHtml(name)}">${escapeHtml(name)}</span>${facts ? `
        <span class="layout-image-picker-facts">${escapeHtml(facts)}</span>` : ""}
      </span>`;
}

function itemsHtml(select: HTMLSelectElement): string {
  return Array.from(select.options)
    // The empty choice clears the image, so it is only offered when there is one to clear.
    .filter((option) => option.value || select.value)
    .map((option) => {
      const selected = option.value === select.value;
      return `<button type="button" class="layout-image-picker-item${selected ? " is-selected" : ""}" role="option" aria-selected="${selected}" data-value="${escapeHtml(option.value)}">${choiceHtml(option, "No image")}</button>`;
    })
    .join("") || `<div class="layout-image-picker-empty">No images in the library yet. Use Browse… to add one.</div>`;
}

function enhance(select: HTMLSelectElement): void {
  select.dataset[BOUND] = "true";
  select.hidden = true;
  select.closest(".layout-property-row")?.classList.add("has-image-picker");

  const picker = document.createElement("details");
  picker.className = "layout-image-picker";
  picker.innerHTML = `
    <summary class="layout-image-picker-trigger"${select.title ? ` title="${escapeHtml(select.title)}"` : ""}>
      <span class="layout-image-picker-selected"></span>
      <span class="layout-image-picker-caret" aria-hidden="true">▾</span>
    </summary>
    <div class="layout-image-picker-list" role="listbox"></div>`;
  select.after(picker);

  const selectedDisplay = picker.querySelector<HTMLElement>(".layout-image-picker-selected")!;
  const list = picker.querySelector<HTMLElement>(".layout-image-picker-list")!;
  const showSelection = () => {
    const option = select.selectedOptions[0];
    selectedDisplay.innerHTML = option ? choiceHtml(option) : "";
  };
  showSelection();
  select.addEventListener("change", showSelection);

  const stopListening = () => {
    document.removeEventListener("pointerdown", onOutside, true);
    document.removeEventListener("keydown", onKey, true);
  };
  // A sidebar render can replace the picker while it is open; it then only stops listening.
  const onOutside = (event: PointerEvent) => {
    if (!picker.isConnected) stopListening();
    else if (!picker.contains(event.target as Node)) picker.open = false;
  };
  const onKey = (event: KeyboardEvent) => {
    if (!picker.isConnected) {
      stopListening();
      return;
    }
    if (event.key === "Escape") {
      event.stopPropagation();
      picker.open = false;
      picker.querySelector<HTMLElement>("summary")?.focus();
    } else if (event.key === "ArrowDown" || event.key === "ArrowUp") {
      const items = Array.from(list.querySelectorAll<HTMLElement>(".layout-image-picker-item"));
      const at = items.indexOf(document.activeElement as HTMLElement);
      const next = items[Math.max(0, Math.min(items.length - 1, at + (event.key === "ArrowDown" ? 1 : -1)))];
      if (!next) return;
      event.preventDefault();
      next.focus();
    }
  };
  picker.addEventListener("toggle", () => {
    if (!picker.open) {
      stopListening();
      return;
    }
    document.querySelectorAll<HTMLDetailsElement>("details.layout-image-picker[open]").forEach((other) => {
      if (other !== picker) other.open = false;
    });
    // Built on opening, so the list shows the library as it is now.
    list.innerHTML = itemsHtml(select);
    document.addEventListener("pointerdown", onOutside, true);
    document.addEventListener("keydown", onKey, true);
    const current = list.querySelector<HTMLElement>(".is-selected") ?? (list.firstElementChild as HTMLElement | null);
    current?.focus({ preventScroll: true });
    list.scrollIntoView({ block: "nearest" });
  });

  list.addEventListener("click", (event) => {
    const item = (event.target as HTMLElement).closest<HTMLButtonElement>(".layout-image-picker-item");
    if (!item) return;
    const value = item.dataset.value ?? "";
    picker.open = false;
    if (value === select.value) return;
    select.value = value;
    // The change handler may render the sidebar again, replacing this picker; that is fine.
    select.dispatchEvent(new Event("change", { bubbles: true }));
  });
}

/** Give every not-yet-enhanced image <select> under `root` its thumbnail picker. */
export function enhanceImageSelects(root: ParentNode): void {
  root.querySelectorAll<HTMLSelectElement>("select").forEach((select) => {
    if (!select.dataset[BOUND] && isImageSelect(select)) enhance(select);
  });
}

/**
 * Keep `root`'s image selects enhanced however its content is replaced: the sidebar is rendered
 * from many places, and some re-render only part of it.
 */
export function watchImageSelects(root: HTMLElement): void {
  enhanceImageSelects(root);
  new MutationObserver(() => enhanceImageSelects(root)).observe(root, { childList: true, subtree: true });
}
