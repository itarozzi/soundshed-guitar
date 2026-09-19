/**
 * Capability + preference helpers for the signal-chain 3D view.
 *
 * Deliberately free of three.js imports so the effect panel can decide whether
 * to offer the 3D view without pulling the (large) renderer into the initial
 * page load. three.js is only fetched by the dynamic import in signalPath.ts.
 */

import { updateAppSetting } from "../appSettingsStore.js";
import { uiState } from "../state.js";

/** Canonical preference for the full signal-chain 3D stage. */
export const SIGNAL_CHAIN_3D_SETTING = "ui.signalChain3d.enabled";

/** @deprecated Read fallback only; prefer SIGNAL_CHAIN_3D_SETTING. */
export const NEURAL_AMP_3D_SETTING = "ui.neuralAmp3dView.enabled";

let cachedWebglSupport: boolean | null = null;

/** True when the WebView can create a WebGL context for the 3D view. */
export function isWebglSupported(): boolean {
  if (cachedWebglSupport !== null) {
    return cachedWebglSupport;
  }
  try {
    if (typeof document === "undefined" || typeof window === "undefined" || !window.WebGLRenderingContext) {
      cachedWebglSupport = false;
      return cachedWebglSupport;
    }
    const canvas = document.createElement("canvas");
    const context = canvas.getContext("webgl2") ?? canvas.getContext("webgl");
    cachedWebglSupport = Boolean(context);
    (context as WebGLRenderingContext | null)?.getExtension("WEBGL_lose_context")?.loseContext();
  } catch {
    cachedWebglSupport = false;
  }
  return cachedWebglSupport;
}

function readSettingFlag(key: string): boolean | undefined {
  const settings = uiState.appSettings;
  if (!settings || !(key in settings)) {
    return undefined;
  }
  return settings[key] === true;
}

/**
 * User preference: render the signal chain as the immersive 3D stage.
 * Migrates legacy `ui.neuralAmp3dView.enabled` on first read when the new key
 * has never been written.
 */
export function isSignalChain3dEnabled(): boolean {
  const current = readSettingFlag(SIGNAL_CHAIN_3D_SETTING);
  if (current !== undefined) {
    return current;
  }
  const legacy = readSettingFlag(NEURAL_AMP_3D_SETTING);
  if (legacy !== undefined) {
    updateAppSetting(SIGNAL_CHAIN_3D_SETTING, legacy);
    return legacy;
  }
  return false;
}

/** @deprecated Use isSignalChain3dEnabled. */
export function isNeuralAmp3dViewEnabled(): boolean {
  return isSignalChain3dEnabled();
}

export function setSignalChain3dEnabled(enabled: boolean): void {
  updateAppSetting(SIGNAL_CHAIN_3D_SETTING, enabled);
}

/** @deprecated Use setSignalChain3dEnabled. */
export function setNeuralAmp3dViewEnabled(enabled: boolean): void {
  setSignalChain3dEnabled(enabled);
}
