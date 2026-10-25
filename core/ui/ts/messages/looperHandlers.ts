/**
 * The looper's feeds from the engine (LooperService): its state, its layers, their waveforms
 * and the saved projects. Each is stored and the strip asked to redraw.
 */

import { requestLooperRender } from "../looper/refresh.js";
import {
  parseLooperLayers,
  parseLooperProjects,
  parseLooperStatus,
  setLooperLayers,
  setLooperProjects,
  setLooperStatus,
  setLooperWaveforms,
} from "../looper/state.js";
import { showNotification } from "../notifications.js";
import type { IncomingPayload } from "./types.js";

export function onLooperState(payload: IncomingPayload): void {
  setLooperStatus(parseLooperStatus(payload));
  requestLooperRender();
}

export function onLooperLayers(payload: IncomingPayload): void {
  setLooperLayers(parseLooperLayers(payload.layers));
  requestLooperRender(true);
}

export function onLooperWaveforms(payload: IncomingPayload): void {
  setLooperWaveforms(payload);
  requestLooperRender();
}

export function onLooperProjects(payload: IncomingPayload): void {
  setLooperProjects(parseLooperProjects(payload.projects));
  requestLooperRender(true);
}

export function onLooperProjectSaved(payload: IncomingPayload): void {
  const title = typeof payload.title === "string" && payload.title ? payload.title : "Loop";
  showNotification(`Saved “${title}” in the riff library`);
}
