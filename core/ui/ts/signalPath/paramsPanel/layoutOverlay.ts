/**
 * Custom effect layouts drawn over the panel: scaling them to the panel width,
 * and the bypass hotspots on top.
 */

import type { GraphNode, Preset } from "../../types.js";
import { toggleSignalPathNodeBypass } from "../bypass.js";
import { nodeParamsPanelElement } from "../state.js";

let overlayBypassClickCleanup: (() => void) | null = null;

let layoutScaleObserverCleanups: (() => void)[] = [];

/**
 * Apply uniform CSS-transform scaling to custom layout containers so that
 * background images and control placements always match the design canvas,
 * regardless of the available panel width.
 *
 * When the panel is wider than the design, the layout is centred at its native
 * size. When it is narrower, the entire layout (backgrounds + controls + labels)
 * is scaled down proportionally as a single unit so nothing diverges.
 *
 * Uses ResizeObserver so the scale stays correct if the panel is resized.
 */
export function applyCustomLayoutScaling(container: HTMLElement | null): void {
  // Disconnect any previous observers from a prior render.
  layoutScaleObserverCleanups.forEach((fn) => fn());
  layoutScaleObserverCleanups = [];

  if (!container) return;

  const outers = container.querySelectorAll<HTMLElement>(".custom-layout-scale-outer");
  outers.forEach((outer) => {
    const inner = outer.querySelector<HTMLElement>(".custom-layout-container[data-design-w]");
    if (!inner) return;

    const designW = parseInt(inner.dataset.designW ?? "0", 10);
    const designH = parseInt(inner.dataset.designH ?? "0", 10);
    if (!designW || !designH) return;

    const applyScale = () => {
      const outerW = outer.offsetWidth;
      if (!outerW) return;

      if (outerW >= designW) {
        // Enough space: render at native size, centred.
        inner.style.transform = "";
        inner.style.marginLeft = "auto";
        inner.style.marginRight = "auto";
        outer.style.height = `${designH}px`;
      } else {
        // Scale the whole layout down to fit, preserving all proportions.
        const scale = outerW / designW;
        inner.style.transform = `scale(${scale})`;
        inner.style.marginLeft = "0";
        inner.style.marginRight = "0";
        outer.style.height = `${Math.round(designH * scale)}px`;
      }
    };

    applyScale();

    const ro = new ResizeObserver(applyScale);
    ro.observe(outer);
    layoutScaleObserverCleanups.push(() => ro.disconnect());
  });
}

export function bindLayoutOverlayBypassToggles(node: GraphNode, preset: Preset): void {
  if (!nodeParamsPanelElement) {
    return;
  }

  overlayBypassClickCleanup?.();

  const clickHandler = (event: Event) => {
    const target = event.target as HTMLElement | null;
    const overlay = target?.closest('.custom-layout-overlay[data-toggle-bypass="true"]') as HTMLElement | null;
    if (!overlay) {
      return;
    }

    event.preventDefault();
    event.stopPropagation();

    toggleSignalPathNodeBypass(node, preset);
  };

  nodeParamsPanelElement.addEventListener("click", clickHandler);
  overlayBypassClickCleanup = () => {
    nodeParamsPanelElement?.removeEventListener("click", clickHandler);
  };
}
