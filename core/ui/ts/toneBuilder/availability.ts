/**
 * Whether the Tone Builder service is taking work for this account (GET /tone-builder/status):
 * whether it is turned on for the signed-in account, the service's kill switch and its daily spend
 * cap. The Tone Builder button shows only when the feature flag is on and the service has said the
 * account may use it; a service that has hit today's spend cap keeps the button and says so when
 * the Tone Builder is opened.
 *
 * Until the service has answered the button stays hidden, since most accounts do not have it. The
 * status is asked again whenever the account signs in or out.
 */

import { Features, isFeatureEnabled } from "../featureFlags.js";
import { toneSharingFetch } from "../toneSharingPanel/api.js";

export interface ToneBuilderServiceStatus {
  enabled: boolean;
  available: boolean;
  reason: "disabled" | "spend-limit" | "not-enabled" | null;
  message: string | null;
}

/** Dispatched on document when the service's status changes. */
export const TONE_BUILDER_STATUS_EVENT = "tone-builder:status";

let current: ToneBuilderServiceStatus | null = null;

export function toneBuilderServiceStatus(): ToneBuilderServiceStatus | null {
  return current;
}

/** Show the Tone Builder button? The feature flag is on and the service has offered it to this account. */
export function isToneBuilderOffered(): boolean {
  return isFeatureEnabled(Features.ToneBuilder) && current?.enabled === true;
}

/** The account changed (signed in or out): ask again, as the Tone Builder is turned on per account. */
export function recheckToneBuilderForAccount(): void {
  if (isFeatureEnabled(Features.ToneBuilder)) void refreshToneBuilderStatus();
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
      reason: data.reason === "disabled" || data.reason === "spend-limit" || data.reason === "not-enabled" ? data.reason : null,
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
