/**
 * Answers to measurePresetLevel. They settle the promise requestPresetLevel() returned and
 * change nothing else (see ../presetLevel.ts).
 */

import { applyPresetLevelMeasureFailed, applyPresetLevelMeasured } from "../presetLevel.js";
import type { IncomingPayload } from "./types.js";

export function onPresetLevelMeasured(payload: IncomingPayload): void {
  applyPresetLevelMeasured(payload);
}

export function onPresetLevelMeasureFailed(payload: IncomingPayload): void {
  applyPresetLevelMeasureFailed(payload);
}
