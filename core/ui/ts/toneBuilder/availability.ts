/**
 * Whether the Tone Builder service is taking work (GET /tone-builder/status): the service's kill
 * switch and its daily spend cap. The Tone Builder button shows only when the feature flag is on
 * and the service has not been switched off; a service that has hit today's spend cap keeps the
 * button and says so when the Tone Builder is opened.
 *
 * Until the service has answered (or when it cannot be reached) the button shows: the service
 * refuses the work itself, so this only spares people a button that cannot work.
 */

import { Features, isFeatureEnabled } from "../featureFlags.js";
import { toneSharingFetch } from "../toneSharingPanel/api.js";

export interface ToneBuilderServiceStatus {
  enabled: boolean;
  available: boolean;
  reason: "disabled" | "spend-limit" | null;
  message: string | null;
}

/** Dispatched on document when the service's status changes. */
export const TONE_BUILDER_STATUS_EVENT = "tone-builder:status";

let current: ToneBuilderServiceStatus | null = null;

export function toneBuilderServiceStatus(): ToneBuilderServiceStatus | null {
  return current;
}

/** Show the Tone Builder button? The feature flag is on and the service is not switched off. */
export function isToneBuilderOffered(): boolean {
  return isFeatureEnabled(Features.ToneBuilder) && current?.enabled !== false;
}

/** Asks the service; returns the new status, or the last known one when it cannot be reached. */
export async function refreshToneBuilderStatus(): Promise<ToneBuilderServiceStatus | null> {
  try {
    const response = await toneSharingFetch("/tone-builder/status", { cache: "no-store" });
    const payload = await response.json().catch(() => null) as { data?: Partial<ToneBuilderServiceStatus> } | null;
    const data = payload?.data;
    if (!response.ok || !data || typeof data.enabled !== "boolean") return current;
    const next: ToneBuilderServiceStatus = {
      enabled: data.enabled,
      available: data.available === true,
      reason: data.reason === "disabled" || data.reason === "spend-limit" ? data.reason : null,
      message: typeof data.message === "string" ? data.message : null,
    };
    const changed = JSON.stringify(next) !== JSON.stringify(current);
    current = next;
    if (changed) document.dispatchEvent(new CustomEvent(TONE_BUILDER_STATUS_EVENT, { detail: next }));
    return current;
  } catch {
    return current;
  }
}
