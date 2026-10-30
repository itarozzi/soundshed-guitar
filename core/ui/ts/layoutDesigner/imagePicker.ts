/**
 * Image pickers that show the images: each designer <select> of library images (imageOptionsHtml)
 * gets a dropdown of thumbnails in its place, filtered by category (All, Knob, Background, Switch,
 * Misc), with your own images above the factory ones.
 *
 * The <select> stays in the page, hidden, and remains what the properties code reads and binds:
 * picking a thumbnail sets its value and fires its change event, so no binding needs to know the
 * picker is there.
 *
 * Dragging one of your images onto a category tab files it there (setLayoutImageCategory); a
 * factory image keeps the category it ships under.
 */

import { postMessage } from "../bridge.js";
import {
  LAYOUT_IMAGE_CATEGORIES,
  isLayoutImageCategory,
  layoutImageCategory,
  layoutImageCategoryLabel,
  layoutImageUsage,
  type LayoutImageCategory,
} from "../layoutImageCategories.js";
import { resolveLayoutImageUrl } from "../layoutImages.js";
import type { LayoutImageRef } from "../layoutTypes.js";
import { uiState } from "../state.js";
import { escapeHtml } from "../utils.js";

/** CSS pixels a thumbnail is drawn across, which picks the smallest copy of a large image. */
const THUMBNAIL_EDGE = 40;
/** Pointer travel before a press on an image becomes a drag. */
const DRAG_THRESHOLD_PX = 5;

const BOUND = "imagePickerBound";

type Filter = LayoutImageCategory | "all";

function isImageSelect(select: HTMLSelectElement): boolean {
  return select.querySelector("option[data-layout-image]") !== null;
}

function libraryImage(imageId: string): LayoutImageRef | undefined {
  return imageId ? uiState.layoutLibrary?.images.find((img) => img.imageId === imageId) : undefined;
}

function thumbnailHtml(imageId: string): string {
  const url = imageId ? resolveLayoutImageUrl(imageId, THUMBNAIL_EDGE) : null;
  return url
    ? `<img class="layout-image-picker-thumb" src="${escapeHtml(url)}" alt="" loading="lazy" draggable="false">`
    : `<span class="layout-image-picker-thumb is-empty" aria-hidden="true"></span>`;
}

/** An option's name, and the image's category and pixel size underneath where the library has them. */
function choiceHtml(option: HTMLOptionElement, usage: Map<string, LayoutImageCategory>, emptyLabel?: string): string {
  const image = libraryImage(option.value);
  const name = image ? image.fileName || image.imageId : emptyLabel ?? option.textContent?.trim() ?? "";
  const facts = image
    ? [
      layoutImageCategoryLabel(layoutImageCategory(image, usage)),
      image.width && image.height ? `${image.width}×${image.height}` : "",
      image.factory ? "Factory" : "",
    ].filter(Boolean).join(" · ")
    : "";
  return `${thumbnailHtml(option.value)}
      <span class="layout-image-picker-text">
        <span class="layout-image-picker-name" title="${escapeHtml(name)}">${escapeHtml(name)}</span>${facts ? `
        <span class="layout-image-picker-facts">${escapeHtml(facts)}</span>` : ""}
      </span>`;
}

function itemHtml(option: HTMLOptionElement, select: HTMLSelectElement, usage: Map<string, LayoutImageCategory>): string {
  const selected = option.value === select.value;
  const factory = Boolean(libraryImage(option.value)?.factory);
  const title = option.value && !factory ? ` title="Drag onto a category to file it there"` : "";
  return `<button type="button" class="layout-image-picker-item${selected ? " is-selected" : ""}" role="option" aria-selected="${selected}" data-value="${escapeHtml(option.value)}"${factory ? ` data-factory="true"` : ""}${title}>${choiceHtml(option, usage, "No image")}</button>`;
}

/** The images in `select` with the category each is filed under. */
function imageOptions(select: HTMLSelectElement, usage: Map<string, LayoutImageCategory>): Array<{ option: HTMLOptionElement; category: LayoutImageCategory; factory: boolean }> {
  return Array.from(select.options)
    .filter((option) => option.value)
    .map((option) => {
      const image = libraryImage(option.value);
      return { option, category: image ? layoutImageCategory(image, usage) : "general", factory: Boolean(image?.factory) };
    });
}

function tabsHtml(filter: Filter, counts: Map<Filter, number>): string {
  const tabs: Array<{ id: Filter; label: string }> = [{ id: "all", label: "All" }, ...LAYOUT_IMAGE_CATEGORIES];
  return tabs.map((tab) => `<button type="button" class="layout-image-picker-tab${tab.id === filter ? " is-active" : ""}" role="tab" aria-selected="${tab.id === filter}" data-filter="${tab.id}"${tab.id === "all" ? "" : ` data-category="${tab.id}"`}>${escapeHtml(tab.label)}<span class="layout-image-picker-count">${counts.get(tab.id) ?? 0}</span></button>`).join("");
}

function listHtml(select: HTMLSelectElement, filter: Filter, usage: Map<string, LayoutImageCategory>): string {
  const shown = imageOptions(select, usage).filter((entry) => filter === "all" || entry.category === filter);
  const own = shown.filter((entry) => !entry.factory);
  const factory = shown.filter((entry) => entry.factory);
  const section = (label: string, entries: typeof shown) => entries.length
    ? `${own.length && factory.length ? `<div class="layout-image-picker-section">${label}</div>` : ""}${entries.map((entry) => itemHtml(entry.option, select, usage)).join("")}`
    : "";
  // The empty choice clears the image, so it is only offered when there is one to clear.
  const clear = select.value ? itemHtml(select.options[0], select, usage) : "";
  const items = section("Your images", own) + section("Factory", factory);
  if (items) return clear + items;
  const what = filter === "all" ? "" : `${layoutImageCategoryLabel(filter).toLowerCase()} `;
  return `${clear}<div class="layout-image-picker-empty">No ${what}images yet.${filter === "all" ? " Use Browse… to add one." : " Drag one onto this tab from All to file it here."}</div>`;
}

/**
 * The filter a picker opens on: the chosen image's category, else the one the row is for (a
 * toggle's Switch, a background's Background), else All when that category has nothing in it.
 */
function initialFilter(select: HTMLSelectElement, usage: Map<string, LayoutImageCategory>): Filter {
  const chosen = libraryImage(select.value);
  if (chosen) return layoutImageCategory(chosen, usage);
  const purpose = select.querySelector<HTMLOptionElement>("option[data-purpose]")?.dataset.purpose;
  if (isLayoutImageCategory(purpose) && imageOptions(select, usage).some((entry) => entry.category === purpose)) return purpose;
  return "all";
}

/** File `imageId` under `category`, here and in the engine. */
function fileImage(imageId: string, category: LayoutImageCategory): void {
  const image = libraryImage(imageId);
  if (!image || image.factory) return;
  image.type = category;
  postMessage({ type: "setLayoutImageCategory", imageId, category });
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
    <div class="layout-image-picker-panel">
      <div class="layout-image-picker-tabs" role="tablist" aria-label="Image category"></div>
      <div class="layout-image-picker-list" role="listbox"></div>
    </div>`;
  select.after(picker);

  const selectedDisplay = picker.querySelector<HTMLElement>(".layout-image-picker-selected")!;
  const panel = picker.querySelector<HTMLElement>(".layout-image-picker-panel")!;
  const tabs = picker.querySelector<HTMLElement>(".layout-image-picker-tabs")!;
  const list = picker.querySelector<HTMLElement>(".layout-image-picker-list")!;
  let filter: Filter = "all";

  const showSelection = () => {
    const option = select.selectedOptions[0];
    selectedDisplay.innerHTML = option ? choiceHtml(option, layoutImageUsage()) : "";
  };
  // Built on opening and after each change, so the list shows the library as it is now.
  const renderPanel = () => {
    const usage = layoutImageUsage();
    const counts = new Map<Filter, number>([["all", 0]]);
    for (const entry of imageOptions(select, usage)) {
      counts.set("all", (counts.get("all") ?? 0) + 1);
      counts.set(entry.category, (counts.get(entry.category) ?? 0) + 1);
    }
    tabs.innerHTML = tabsHtml(filter, counts);
    list.innerHTML = listHtml(select, filter, usage);
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
    filter = initialFilter(select, layoutImageUsage());
    renderPanel();
    document.addEventListener("pointerdown", onOutside, true);
    document.addEventListener("keydown", onKey, true);
    const current = list.querySelector<HTMLElement>(".is-selected") ?? list.querySelector<HTMLElement>(".layout-image-picker-item");
    current?.focus({ preventScroll: true });
    panel.scrollIntoView({ block: "nearest" });
  });

  tabs.addEventListener("click", (event) => {
    const tab = (event.target as HTMLElement).closest<HTMLElement>(".layout-image-picker-tab");
    const next = tab?.dataset.filter;
    if (!next || (next !== "all" && !isLayoutImageCategory(next))) return;
    filter = next;
    renderPanel();
  });

  // A drag ends with the button released over a tab; the click that can follow is not a pick.
  let suppressClick = false;
  list.addEventListener("click", (event) => {
    if (suppressClick) return;
    const item = (event.target as HTMLElement).closest<HTMLButtonElement>(".layout-image-picker-item");
    if (!item) return;
    const value = item.dataset.value ?? "";
    picker.open = false;
    if (value === select.value) return;
    select.value = value;
    // The change handler may render the sidebar again, replacing this picker; that is fine.
    select.dispatchEvent(new Event("change", { bubbles: true }));
  });

  list.addEventListener("pointerdown", (event) => {
    const item = (event.target as HTMLElement).closest<HTMLElement>(".layout-image-picker-item");
    const imageId = item?.dataset.value;
    if (event.button !== 0 || !item || !imageId || item.dataset.factory) return;
    const startX = event.clientX;
    const startY = event.clientY;
    let dragging = false;
    let target: HTMLElement | null = null;

    const setTarget = (next: HTMLElement | null) => {
      if (next === target) return;
      target?.classList.remove("is-drop-target");
      target = next;
      target?.classList.add("is-drop-target");
    };
    const onMove = (move: PointerEvent) => {
      if (move.pointerId !== event.pointerId) return;
      if (!dragging) {
        if (Math.hypot(move.clientX - startX, move.clientY - startY) < DRAG_THRESHOLD_PX) return;
        dragging = true;
        item.classList.add("is-dragging");
        panel.classList.add("is-dragging-image");
      }
      move.preventDefault();
      const under = document.elementFromPoint(move.clientX, move.clientY)?.closest<HTMLElement>(".layout-image-picker-tab[data-category]");
      setTarget(under && tabs.contains(under) ? under : null);
    };
    const finish = (end: PointerEvent) => {
      if (end.pointerId !== event.pointerId) return;
      document.removeEventListener("pointermove", onMove, true);
      document.removeEventListener("pointerup", finish, true);
      document.removeEventListener("pointercancel", finish, true);
      if (!dragging) return;
      item.classList.remove("is-dragging");
      panel.classList.remove("is-dragging-image");
      suppressClick = true;
      setTimeout(() => { suppressClick = false; }, 0);
      const category = end.type === "pointerup" ? target?.dataset.category : undefined;
      setTarget(null);
      if (isLayoutImageCategory(category)) {
        fileImage(imageId, category);
        renderPanel();
        showSelection();
      }
    };
    document.addEventListener("pointermove", onMove, true);
    document.addEventListener("pointerup", finish, true);
    document.addEventListener("pointercancel", finish, true);
  });
}

/** Give every not-yet-enhanced image <select> under `root` its thumbnail picker. */
export function enhanceImageSelects(root: ParentNode): void {
  root.querySelectorAll<HTMLSelectElement>("select").forEach((select) => {
    if (!select.dataset[BOUND] && isImageSelect(select)) enhance(select);
  });
}
