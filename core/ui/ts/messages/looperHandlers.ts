/**
 * The looper's three feeds from the engine (LooperService): its state, its layers and their
 * waveforms. Each is stored and the strip asked to redraw.
 */

import { requestLooperRender } from "../looper/refresh.js";
import { parseLooperLayers, parseLooperStatus, setLooperLayers, setLooperStatus, setLooperWaveforms } from "../looper/state.js";
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
