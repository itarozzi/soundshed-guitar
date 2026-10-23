/**
 * The effect chooser a + button between two nodes opens: the FX library's browser (its
 * Favourites and Recent tabs, the categories and the search) in a flyout that floats below
 * (or above) the button it was opened from, where a click adds the effect.
 *
 * The chooser only presents the choice. What choosing an effect does is the caller's,
 * handed in as `onChoose`, so this module never needs the signal path renderer.
 */

import { EffectGuids } from "../effectGuids.js";
import type { FxPointerDragPayload } from "../fxLibrary/catalog.js";
import { FxBrowser, preferredMarkTab } from "../fxLibrary/fxBrowser.js";
import { getUiZoom } from "../pointerDrag.js";
import { signalPathNodesElement } from "./state.js";

/** The open chooser, if any. */
let openDropdown: { close: () => void; reposition: () => void } | null = null;

/** The category last browsed here, opened next time when there are no favourites or recents. */
let lastCategory: string | undefined;

/** The data attributes that say which insertion point a + button stands for. */
const ADD_BUTTON_POSITION_KEYS = ["edgeFrom", "edgeTo", "edgeFromPort", "edgeToPort", "insertAfter"] as const;

/** The mixer joins a split; it is never added on its own. */
const EXCLUDED_TYPES = [EffectGuids.kMixer];

/** The + button a re-render put in place of `previous`: the one for the same insertion point. */
function findReplacementAddButton(previous: HTMLElement): HTMLElement | null {
  const buttons = Array.from(signalPathNodesElement?.querySelectorAll<HTMLElement>(".signal-add-btn") ?? []);
  return buttons.find((button) => ADD_BUTTON_POSITION_KEYS.every((key) => button.dataset[key] === previous.dataset[key])) ?? null;
}

const DROPDOWN_HTML = `
  <div class="fx-flyout-header">
    <span class="fx-flyout-title" id="fx-flyout-title">Add Effect</span>
    <div class="fx-selector-search">
      <input type="text" placeholder="Search effects…" aria-label="Search effects" />
    </div>
  </div>
  <div class="fx-selector-content">
    <div class="fx-selector-categories scrollbar-thin"></div>
    <div class="fx-selector-effects-list scrollbar-thin"></div>
  </div>
`;

/**
 * Opens the chooser under `button`, replacing any chooser already open. Choosing an effect
 * hands what it carries into the chain to `onChoose`, then closes the chooser. Its effects
 * can be dragged into the chain as well, as from the docked library.
 */
export function showAddEffectDropdown(button: HTMLElement, onChoose: (payload: FxPointerDragPayload) => void): void {
  openDropdown?.close();

  const dropdown = document.createElement("div");
  dropdown.className = "fx-flyout";
  dropdown.setAttribute("role", "dialog");
  dropdown.setAttribute("aria-labelledby", "fx-flyout-title");
  dropdown.innerHTML = DROPDOWN_HTML;
  document.body.appendChild(dropdown);

  const search = dropdown.querySelector<HTMLInputElement>(".fx-selector-search input");
  const browser = new FxBrowser({
    categories: dropdown.querySelector<HTMLElement>(".fx-selector-categories")!,
    list: dropdown.querySelector<HTMLElement>(".fx-selector-effects-list")!,
    search,
    tab: preferredMarkTab(EXCLUDED_TYPES) ?? lastCategory,
    excludeTypes: EXCLUDED_TYPES,
    getPick: () => ({
      onPick: (payload) => {
        closeDropdown();
        onChoose(payload);
      },
    }),
    onTabChange: (tab) => {
      lastCategory = tab;
    },
  });
  browser.render();

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

    dropdown.style.width = `${Math.min(560, viewportWidth - margin * 2)}px`;
    dropdown.style.height = `${Math.min(440, viewportHeight - margin * 2)}px`;
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

  // Scrolling the lists inside the chooser does not move the button it hangs from.
  const onScroll = (event: Event): void => {
    if (event.target instanceof Node && dropdown.contains(event.target)) {
      return;
    }
    positionDropdown();
  };

  // Starring an effect redraws the list under the click, so by the time the click reaches
  // the document its target is detached. The path it took still runs through the chooser.
  const closeOnOutsideClick = (event: MouseEvent): void => {
    if (!event.composedPath().includes(dropdown)) {
      closeDropdown();
    }
  };

  const closeOnEscape = (event: KeyboardEvent): void => {
    if (event.key === "Escape") {
      event.preventDefault();
      const target = anchor;
      closeDropdown();
      if (target.isConnected) {
        target.focus();
      }
    }
  };

  const handle = { close: closeDropdown, reposition: positionDropdown };

  function closeDropdown(): void {
    window.removeEventListener("resize", positionDropdown);
    window.removeEventListener("scroll", onScroll, true);
    document.removeEventListener("click", closeOnOutsideClick);
    browser.destroy();
    dropdown.remove();
    if (openDropdown === handle) {
      openDropdown = null;
    }
  }

  openDropdown = handle;
  positionDropdown();
  window.addEventListener("resize", positionDropdown);
  window.addEventListener("scroll", onScroll, true);
  dropdown.addEventListener("keydown", closeOnEscape);

  // Typing filters at once; on a touch screen the keyboard would cover the list, so not there.
  if (!window.matchMedia?.("(pointer: coarse)").matches) {
    search?.focus({ preventScroll: true });
  }

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
