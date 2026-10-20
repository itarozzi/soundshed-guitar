/**
 * Window chrome: tells the standalone app the colours at the top of the page, so the
 * native title bar can match the theme (Windows' DWM caption colour). Sent at start-up and
 * on every theme change. A plugin host owns its window, and other platforms keep the
 * system's bar, so there the message is dropped.
 */
import { postMessage } from "./bridge.js";

let lastSent = "";
let swatch: CanvasRenderingContext2D | null = null;

/**
 * A CSS colour as "#rrggbb", laid over `backdrop` first so a translucent colour comes out
 * as it looks on screen. The canvas does the parsing, so any colour syntax the page uses
 * (rgb(), color-mix(), color(srgb ...)) works.
 */
function toOpaqueHex(color: string, backdrop: string): string | null {
  swatch ??= document.createElement("canvas").getContext("2d", { willReadFrequently: true });
  if (!swatch) return null;

  swatch.clearRect(0, 0, 1, 1);
  swatch.fillStyle = backdrop;
  swatch.fillRect(0, 0, 1, 1);
  swatch.fillStyle = color;
  swatch.fillRect(0, 0, 1, 1);

  const [r, g, b] = swatch.getImageData(0, 0, 1, 1).data;
  return `#${[r, g, b].map((v) => v.toString(16).padStart(2, "0")).join("")}`;
}

function luminance(hex: string): number {
  const channel = (offset: number): number => {
    const c = parseInt(hex.slice(offset, offset + 2), 16) / 255;
    return c <= 0.03928 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4;
  };
  return 0.2126 * channel(1) + 0.7152 * channel(3) + 0.0722 * channel(5);
}

function contrast(a: string, b: string): number {
  const [hi, lo] = [luminance(a), luminance(b)].sort((x, y) => y - x);
  return (hi + 0.05) / (lo + 0.05);
}

/** The header bar's colours, or nothing if the page has no header to read. */
export function readWindowChrome(): { caption: string; text: string; border: string } | null {
  const bar = document.querySelector<HTMLElement>(".icon-bar");
  if (!bar) return null;

  const page = getComputedStyle(document.body).backgroundColor || "#000";
  const barStyle = getComputedStyle(bar);
  const caption = toOpaqueHex(barStyle.backgroundColor, page);
  if (!caption) return null;

  // The bar's own text colour; a plain light or dark one if a theme leaves that hard to
  // read on the bar.
  let text = toOpaqueHex(barStyle.color, caption);
  if (!text || contrast(text, caption) < 3) text = luminance(caption) < 0.18 ? "#f2f2f2" : "#1a1a1a";

  const border = toOpaqueHex(barStyle.borderBottomColor, caption) ?? caption;
  return { caption, text, border };
}

export function sendWindowChrome(): void {
  const chrome = readWindowChrome();
  if (!chrome) return;

  const payload = JSON.stringify(chrome);
  if (payload === lastSent) return;
  lastSent = payload;

  postMessage({ type: "setWindowChrome", ...chrome });
}

/**
 * Call before asking for the theme ("getTheme"): its answer is the first send. Sending the
 * page's starting colours now would flash the default theme over the colours the window
 * opened with, which are last run's.
 */
export function initWindowChrome(): void {
  window.addEventListener("themeChanged", () => sendWindowChrome());
}
