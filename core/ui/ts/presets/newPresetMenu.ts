import { getUiZoom } from "../pointerDrag.js";
import { createDefaultPreset } from "./saveModal.js";

const TRIGGERS = "#preset-new-btn, #create-default-preset";
let initialized = false;
let menu: HTMLDivElement | null = null;
let anchor: HTMLButtonElement | null = null;

function closeMenu(restoreFocus = false): void {
  menu?.remove();
  menu = null;
  anchor?.setAttribute("aria-expanded", "false");
  if (restoreFocus) {
    anchor?.focus();
  }
  anchor = null;
}

function openMenu(button: HTMLButtonElement, last = false): void {
  closeMenu();
  anchor = button;
  menu = document.createElement("div");
  menu.id = "new-preset-menu";
  menu.className = "preset-folder-menu";
  menu.setAttribute("role", "menu");
  menu.setAttribute("aria-label", "New preset");
  menu.setAttribute("data-control-bar-popover", "");
  menu.innerHTML = `
    <button class="preset-folder-menu-item" type="button" role="menuitem" data-configuration="empty">Empty Preset</button>
    <button class="preset-folder-menu-item" type="button" role="menuitem" data-configuration="basic">Basic Signal Chain</button>
  `;
  menu.addEventListener("click", (event) => {
    const option = event.target instanceof Element
      ? event.target.closest<HTMLButtonElement>("[data-configuration]") : null;
    const configuration = option?.dataset.configuration;
    if (configuration !== "empty" && configuration !== "basic") {
      return;
    }
    closeMenu(true);
    createDefaultPreset(configuration);
  });
  menu.addEventListener("keydown", (event) => {
    if (!menu) {
      return;
    }
    const options = Array.from(menu.querySelectorAll<HTMLButtonElement>("button"));
    const index = options.indexOf(document.activeElement as HTMLButtonElement);
    let next: number;
    switch (event.key) {
      case "ArrowDown": next = (index + 1) % options.length; break;
      case "ArrowUp": next = (index - 1 + options.length) % options.length; break;
      case "Home": next = 0; break;
      case "End": next = options.length - 1; break;
      default: return;
    }
    event.preventDefault();
    options[next]?.focus();
  });
  document.body.append(menu);
  button.setAttribute("aria-expanded", "true");

  const zoom = getUiZoom();
  const rect = button.getBoundingClientRect();
  const width = window.innerWidth / zoom;
  const height = window.innerHeight / zoom;
  const margin = 8;
  menu.style.maxWidth = `${Math.max(0, width - margin * 2)}px`;
  menu.style.maxHeight = `${Math.max(0, height - margin * 2)}px`;
  menu.style.overflowY = "auto";
  const left = Math.max(margin, Math.min(rect.left / zoom, width - menu.offsetWidth - margin));
  const below = rect.bottom / zoom + 6;
  const top = below + menu.offsetHeight <= height - margin
    ? below : Math.max(margin, rect.top / zoom - menu.offsetHeight - 6);
  menu.style.left = `${Math.round(left)}px`;
  menu.style.top = `${Math.round(top)}px`;
  const options = menu.querySelectorAll<HTMLButtonElement>("button");
  options[last ? options.length - 1 : 0]?.focus();
}

export function initializeNewPresetMenu(): void {
  if (initialized) {
    return;
  }
  initialized = true;
  // Delegation also covers the details pane's button, which is rebuilt on preset changes.
  document.addEventListener("click", (event) => {
    const button = event.target instanceof Element
      ? event.target.closest<HTMLButtonElement>(TRIGGERS) : null;
    if (!button || button.disabled) {
      return;
    }
    if (anchor === button) {
      closeMenu(true);
    } else {
      openMenu(button);
    }
  });
  document.addEventListener("pointerdown", (event) => {
    if (event.target instanceof Node && !menu?.contains(event.target) && !anchor?.contains(event.target)) {
      closeMenu();
    }
  }, true);
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && menu) {
      event.preventDefault();
      event.stopPropagation();
      closeMenu(true);
      return;
    }
    const button = event.target instanceof Element
      ? event.target.closest<HTMLButtonElement>(TRIGGERS) : null;
    if (button && !button.disabled && (event.key === "ArrowDown" || event.key === "ArrowUp")) {
      event.preventDefault();
      openMenu(button, event.key === "ArrowUp");
    }
  });
  document.addEventListener("focusin", (event) => {
    if (event.target instanceof Node && !menu?.contains(event.target) && !anchor?.contains(event.target)) {
      closeMenu();
    }
  });
  document.addEventListener("scroll", (event) => {
    if (!(event.target instanceof Node) || !menu?.contains(event.target)) {
      closeMenu();
    }
  }, true);
  window.addEventListener("resize", () => closeMenu());
  window.addEventListener("blur", () => closeMenu());
}
