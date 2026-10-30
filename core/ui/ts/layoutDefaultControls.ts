/**
 * How a useDefaultControls layout draws the params panel's own controls over its background.
 *
 * The params panel builds the controls (tabs, groups, knobs) knowing nothing of layouts, so the
 * layout's choices are applied to that markup here, for the live panel and the designer alike:
 *
 * - `defaultControlsMainOnly`: only the Main tab's controls, with no tab bar and no Advanced tab.
 * - `defaultControlsPanels: false`: the groups lose their titled panels (the heading and the themed
 *   box), so the controls sit straight on the layout's background, where they were.
 *
 * then its knob image and switch (layoutControlImages.ts) and shadows (layoutShadows.ts). Older
 * apps ignore the two options and draw the tabs and panels as before.
 */

import { applyKnobImageToKnobs, applySwitchImageToToggles, type ImageCssResolver } from "./layoutControlImages.js";
import { applyShadowsToControls } from "./layoutShadows.js";
import type { EffectLayout } from "./layoutTypes.js";

/** `html` with only the Main tab's controls, no tab bar; unchanged when it has no tabs. */
export function mainTabOnly(html: string): string {
  if (!html.includes("node-param-tab")) return html;
  const template = document.createElement("template");
  template.innerHTML = html;
  template.content.querySelectorAll(".node-param-tabs").forEach((tabs) => tabs.remove());
  template.content.querySelectorAll(".node-param-tab-panels").forEach((panels) => {
    const main = panels.querySelector(":scope > .node-param-tab-panel[data-tab='main']");
    panels.replaceWith(...Array.from(main?.childNodes ?? []));
  });
  return template.innerHTML;
}

/** The default controls' markup as `layout` draws it. */
export function layoutDefaultControlsHtml(html: string, layout: EffectLayout, resolve: ImageCssResolver): string {
  const controls = layout.defaultControlsMainOnly === true ? mainTabOnly(html) : html;
  const drawn = applySwitchImageToToggles(applyKnobImageToKnobs(controls, layout.knobImage, resolve), layout.switchImage, resolve);
  return applyShadowsToControls(drawn, layout.controlShadow);
}

/** The class the default controls' wrapper takes for `layout`: no-group-panels, or none. */
export function defaultControlsPanelClass(layout: EffectLayout): string {
  return layout.defaultControlsPanels === false ? " no-group-panels" : "";
}
