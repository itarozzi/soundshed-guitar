/**
 * Shadows a layout's knobs and toggles cast, generated rather than drawn.
 *
 * A layout that sets `controlShadow` has one light for the whole panel: the direction it comes
 * from and how high it is. Every knob and toggle is taken to stand as tall as it is wide (the
 * smaller of its width and height), so its shadow is its footprint swept away from the light by
 * height / tan(elevation): a capsule for a knob, a rounded sweep for a toggle. The shadow is one
 * still SVG behind the control, so turning a knob never redraws it; it is sized in percent of the
 * control, so it follows the control's drawn size.
 *
 * It sits inside the control's own element (the panel finds a knob's label and value through its
 * parent, so the knob is never wrapped), at z-index -1 within its parent, which `has-control-shadow`
 * isolates so the shadow stays above the layout's background.
 */

import type { LayoutControlShadow } from "./layoutTypes.js";
import { escapeHtml } from "./utils.js";

/** The light a layout with `controlShadow: {}` gets. */
export const CONTROL_SHADOW_DEFAULTS: Required<LayoutControlShadow> = {
  angle: 315,
  elevation: 45,
  softness: 0.4,
  opacity: 0.5,
};

const clamp = (value: unknown, min: number, max: number, fallback: number) =>
  typeof value === "number" && Number.isFinite(value) ? Math.min(max, Math.max(min, value)) : fallback;

/** The layout's light with every field in range, or null when the layout casts no shadows. */
export function usableControlShadow(value: unknown): Required<LayoutControlShadow> | null {
  if (!value || typeof value !== "object" || Array.isArray(value)) return null;
  const light = value as LayoutControlShadow;
  return {
    angle: ((clamp(light.angle, -3600, 3600, CONTROL_SHADOW_DEFAULTS.angle) % 360) + 360) % 360,
    elevation: clamp(light.elevation, 15, 80, CONTROL_SHADOW_DEFAULTS.elevation),
    softness: clamp(light.softness, 0, 1, CONTROL_SHADOW_DEFAULTS.softness),
    opacity: clamp(light.opacity, 0, 1, CONTROL_SHADOW_DEFAULTS.opacity),
  };
}

type Point = [number, number];

/** The convex hull of `points`, anticlockwise, duplicates dropped (Andrew's monotone chain). */
function convexHull(points: Point[]): Point[] {
  const sorted = [...points].sort((a, b) => a[0] - b[0] || a[1] - b[1])
    .filter((p, i, all) => i === 0 || p[0] !== all[i - 1][0] || p[1] !== all[i - 1][1]);
  if (sorted.length < 3) return sorted;
  const cross = (o: Point, a: Point, b: Point) => (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
  const half = (list: Point[]) => {
    const hull: Point[] = [];
    for (const p of list) {
      while (hull.length >= 2 && cross(hull[hull.length - 2], hull[hull.length - 1], p) <= 0) hull.pop();
      hull.push(p);
    }
    hull.pop();
    return hull;
  };
  return [...half(sorted), ...half([...sorted].reverse())];
}

const r2 = (value: number) => Math.round(value * 100) / 100;

/**
 * The shadow of a control `width` x `height` px with corner radius `radius` under `light`: an SVG
 * placed in percent of the control's box. `radius` of half the smaller side is a round or pill
 * footprint.
 */
export function controlShadowSvg(light: Required<LayoutControlShadow>, width: number, height: number, radius: number): string {
  const r = Math.max(0, Math.min(radius, width / 2, height / 2));
  const standing = Math.min(width, height);
  const length = standing / Math.tan((light.elevation * Math.PI) / 180);
  // The light comes from `angle` (0 at the top, clockwise); the shadow falls the other way.
  const away = ((light.angle + 180) * Math.PI) / 180;
  const shift: Point = [Math.sin(away) * length, -Math.cos(away) * length];

  // The footprint is a rect inset by r, grown by r again with round joins.
  const cx = width / 2;
  const cy = height / 2;
  const hw = width / 2 - r;
  const hh = height / 2 - r;
  const corners: Point[] = [[cx - hw, cy - hh], [cx + hw, cy - hh], [cx + hw, cy + hh], [cx - hw, cy + hh]];
  const hull = convexHull([...corners, ...corners.map(([x, y]): Point => [x + shift[0], y + shift[1]])]);

  const blur = r2(light.softness * (1.5 + length * 0.35));
  const pad = r + blur * 3 + 1;
  const xs = hull.map((p) => p[0]);
  const ys = hull.map((p) => p[1]);
  const minX = Math.min(...xs) - pad;
  const minY = Math.min(...ys) - pad;
  const boxW = Math.max(...xs) + pad - minX;
  const boxH = Math.max(...ys) + pad - minY;
  const pct = (value: number, of: number) => `${r2((value / of) * 100)}%`;

  // Darkest where the control meets the panel, fading towards the far end.
  const style = [
    `left: ${pct(minX, width)}`, `top: ${pct(minY, height)}`,
    `width: ${pct(boxW, width)}`, `height: ${pct(boxH, height)}`,
    `opacity: ${r2(light.opacity)}`,
    blur > 0 ? `filter: blur(${blur}px)` : "",
    `mask-image: linear-gradient(${r2(light.angle + 180)}deg, #000 30%, rgba(0, 0, 0, 0.3) 100%)`,
  ].filter(Boolean).join("; ");
  const points = hull.map(([x, y]) => `${r2(x)},${r2(y)}`).join(" ");
  return `<svg class="custom-control-shadow" aria-hidden="true" viewBox="${r2(minX)} ${r2(minY)} ${r2(boxW)} ${r2(boxH)}" preserveAspectRatio="none" style="${escapeHtml(style)}">`
    + `<polygon points="${escapeHtml(points)}" fill="#000" stroke="#000" stroke-width="${r2(r * 2)}" stroke-linejoin="round" stroke-linecap="round"/></svg>`;
}

/** A knob's shadow: a round footprint as wide as the knob's own box. */
export function knobShadowSvg(light: Required<LayoutControlShadow>, size: number): string {
  return controlShadowSvg(light, size, size, size / 2);
}

/** A toggle's shadow: a pill the size of the switch. */
export function toggleShadowSvg(light: Required<LayoutControlShadow>, width: number, height: number): string {
  return controlShadowSvg(light, width, height, Math.min(width, height) / 2);
}

/**
 * `html` (the default controls the params panel builds) with a shadow behind each knob and toggle
 * under `light`, at their standard sizes; `html` unchanged when the layout casts no shadows.
 */
export function applyShadowsToControls(html: string, light: unknown): string {
  const usable = usableControlShadow(light);
  if (!usable || !(html.includes("node-param-knob") || html.includes("toggle-switch"))) return html;
  const template = document.createElement("template");
  template.innerHTML = html;
  const add = (element: Element, svg: string) => {
    if (element.querySelector(":scope > .custom-control-shadow")) return;
    element.insertAdjacentHTML("afterbegin", svg);
    element.parentElement?.classList.add("has-control-shadow");
  };
  template.content.querySelectorAll(".node-param-knob").forEach((knob) => add(knob, knobShadowSvg(usable, 38)));
  // A toggle drawn from the layout's switch carries its size; a standard one is 36 x 20.
  template.content.querySelectorAll<HTMLElement>("label.toggle-switch").forEach((toggle) =>
    add(toggle, toggleShadowSvg(usable, parseFloat(toggle.style.width) || 36, parseFloat(toggle.style.height) || 20)));
  return template.innerHTML;
}
