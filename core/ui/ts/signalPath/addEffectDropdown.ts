/**
 * The effect chooser a + button between two nodes opens: every effect by category, in a
 * list that floats below (or above) the button it was opened from.
 *
 * The chooser only presents the choice. What choosing an effect does is the caller's,
 * handed in as `onChoose`, so this module never needs the signal path renderer.
 */

import { EffectGuids } from "../effectGuids.js";
import { CATEGORY_METADATA, getFxLibraryItems, getOrderedFxCategories, type FxLibraryItem } from "../fxSelector.js";
import { getBadgeIcon } from "../iconAssets.js";
import { getCustomLayout } from "../layoutRenderer.js";
import { getUiZoom } from "../pointerDrag.js";
import { escapeHtml } from "../utils.js";
import { getNodeIcon } from "./nodeTypes.js";
import { signalPathNodesElement } from "./state.js";

/** The open chooser, if any. */
let openDropdown: { close: () => void; reposition: () => void } | null = null;

/** The data attributes that say which insertion point a + button stands for. */
const ADD_BUTTON_POSITION_KEYS = ["edgeFrom", "edgeTo", "edgeFromPort", "edgeToPort", "insertAfter"] as const;

/** The + button a re-render put in place of `previous`: the one for the same insertion point. */
function findReplacementAddButton(previous: HTMLElement): HTMLElement | null {
  const buttons = Array.from(signalPathNodesElement?.querySelectorAll<HTMLElement>(".signal-add-btn") ?? []);
  return buttons.find((button) => ADD_BUTTON_POSITION_KEYS.every((key) => button.dataset[key] === previous.dataset[key])) ?? null;
}

function buildDropdownHtml(): string {
  const dropdownItems = getFxLibraryItems({ excludeTypes: [EffectGuids.kMixer] });
  const effectsByCategory = new Map<string, FxLibraryItem[]>();

  dropdownItems.forEach((effect) => {
    if (!effectsByCategory.has(effect.category)) {
      effectsByCategory.set(effect.category, []);
    }
    effectsByCategory.get(effect.category)!.push(effect);
  });

  const categoryOrder = getOrderedFxCategories(dropdownItems);

  let dropdownHtml = '<div class="effect-dropdown-header">Add Effect</div>';

  categoryOrder.forEach((categoryId) => {
    const effects = effectsByCategory.get(categoryId) ?? [];
    if (effects.length > 0) {
      const categoryInfo = CATEGORY_METADATA[categoryId];
      const categoryColor = categoryInfo?.color || "var(--color-accent)";
      dropdownHtml += `
        <div class="effect-dropdown-category" style="--category-color: ${escapeHtml(categoryColor)}">
          <div class="effect-dropdown-category-name">
            ${categoryInfo?.name || categoryId}
          </div>
          ${effects.map((effect) => {
              const thumb = effect.blendId
                ? (getCustomLayout(effect.type, effect.blendId) ?? getCustomLayout(effect.type))?.thumbnailDataUrl
                : (getCustomLayout(effect.type)?.thumbnailDataUrl ?? effect.thumbnailDataUrl);
            const icon = thumb
              ? `<img src="${thumb.replace(/"/g, '&quot;')}" alt="" aria-hidden="true" class="effect-dropdown-thumb" />`
              : `<span class="effect-dropdown-icon">${effect.blendId ? getBadgeIcon("blend", "Custom blend") : getNodeIcon(effect.type)}</span>`;
              return `
              <div class="effect-dropdown-item"
                data-effect-type="${effect.type}"
                data-blend-id="${escapeHtml(effect.blendId ?? "")}"
                data-blend-name="${escapeHtml(effect.blendId ? effect.displayName : "")}"
                data-blend-category="${escapeHtml(effect.blendCategory ?? "") }"
                data-effect-category="${escapeHtml(effect.category ?? "utility") }"
                data-custom-effect-id="${escapeHtml(effect.customEffectId ?? "") }"
                data-custom-effect-resource-type="${escapeHtml(effect.moduleResourceType ?? "") }"
                data-custom-effect-resource-id="${escapeHtml(effect.moduleResourceId ?? "") }"
                data-custom-effect-default-params="${escapeHtml(encodeURIComponent(JSON.stringify(effect.defaultParams ?? {})))}"
                style="--category-color: ${escapeHtml(categoryColor)}">
              ${icon}
              <span class="effect-dropdown-name">${escapeHtml(effect.displayName)}</span>
            </div>
          `;
          }).join('')}
        </div>
      `;
    }
  });

  return dropdownHtml;
}

/**
 * Opens the chooser under `button`, replacing any chooser already open. Choosing an effect
 * hands its `.effect-dropdown-item` row, whose data attributes describe the effect, to
 * `onChoose`, then closes the chooser.
 */
export function showAddEffectDropdown(button: HTMLElement, onChoose: (item: HTMLElement) => void): void {
  openDropdown?.close();

  const dropdown = document.createElement("div");
  dropdown.className = "effect-selection-dropdown";
  dropdown.innerHTML = buildDropdownHtml();
  document.body.appendChild(dropdown);

  // Every render of the chain replaces its + buttons, and a detached button measures as a
  // zero rect, which put the chooser in the window's top-left corner. Follow the button
  // that replaced it; when the chain no longer has that insertion point, or it is not on
  // screen, there is nothing to hang from and nowhere to insert, so close.
  let anchor = button;
  const resolveAnchor = (): HTMLElement | null => {
    if (!anchor.isConnected) {
      const replacement = findReplacementAddButton(anchor);
      if (!replacement) {
        return null;
      }
      anchor = replacement;
    }
    return anchor.getClientRects().length > 0 ? anchor : null;
  };

  const positionDropdown = (): void => {
    const target = resolveAnchor();
    if (!target) {
      closeDropdown();
      return;
    }
    const buttonRect = target.getBoundingClientRect();
    const margin = 8;
    const uiZoom = getUiZoom();
    const viewportWidth = window.innerWidth / uiZoom;
    const viewportHeight = window.innerHeight / uiZoom;

    dropdown.style.maxWidth = `${Math.min(300, viewportWidth - margin * 2)}px`;
    dropdown.style.maxHeight = `${Math.min(500, viewportHeight - margin * 2)}px`;
    const dropdownWidth = dropdown.offsetWidth;
    const dropdownHeight = dropdown.offsetHeight;
    const left = Math.max(
      margin,
      Math.min(buttonRect.left / uiZoom, viewportWidth - dropdownWidth - margin),
    );
    let top = buttonRect.bottom / uiZoom + 5;

    if (top + dropdownHeight > viewportHeight - margin) {
      top = Math.max(margin, buttonRect.top / uiZoom - dropdownHeight - 5);
    }

    dropdown.style.left = `${Math.round(left)}px`;
    dropdown.style.top = `${Math.round(top)}px`;
  };

  // Scrolling the list inside the chooser does not move the button it hangs from.
  const onScroll = (event: Event): void => {
    if (event.target instanceof Node && dropdown.contains(event.target)) {
      return;
    }
    positionDropdown();
  };

  const closeOnOutsideClick = (event: MouseEvent): void => {
    if (!dropdown.contains(event.target as Node)) {
      closeDropdown();
    }
  };

  const handle = { close: closeDropdown, reposition: positionDropdown };

  function closeDropdown(): void {
    window.removeEventListener("resize", positionDropdown);
    window.removeEventListener("scroll", onScroll, true);
    document.removeEventListener("click", closeOnOutsideClick);
    dropdown.remove();
    if (openDropdown === handle) {
      openDropdown = null;
    }
  }

  openDropdown = handle;
  positionDropdown();
  window.addEventListener("resize", positionDropdown);
  window.addEventListener("scroll", onScroll, true);

  dropdown.querySelectorAll<HTMLElement>(".effect-dropdown-item").forEach((item) => {
    item.addEventListener("click", () => {
      if (item.dataset.effectType) {
        onChoose(item);
        closeDropdown();
      }
    });
  });

  // Close when clicking outside. Deferred past the click that opened the chooser, and not
  // registered at all if something closed it in the meantime.
  setTimeout(() => {
    if (openDropdown === handle) {
      document.addEventListener("click", closeOnOutsideClick);
    }
  }, 0);
}

/**
 * Called after the chain re-renders: moves an open chooser to the + button that replaced
 * the one it was opened from, or closes it when that insertion point is gone.
 */
export function reanchorAddEffectDropdown(): void {
  openDropdown?.reposition();
}
