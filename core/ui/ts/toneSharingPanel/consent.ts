/**
 * The one-time publishing consent gate: what the server thinks the user has
 * agreed to, what the local copy says, and the modal that asks.
 */

import { updateAppSetting } from "../appSettingsStore.js";
import { uiState } from "../state.js";
import { apiFetch } from "./api.js";
import { element } from "./dom.js";
import { TONE_SHARING_PUBLISH_CONSENT_VERSION, storageKeys, toneSharingState } from "./state.js";
import type { ShareConsentStatus } from "./types.js";

export function getLocalPublishConsent(): { version: number; acceptedAt: string; userId?: string } | null {
  const raw = uiState.appSettings?.[storageKeys.publishConsent];
  if (!raw || typeof raw !== "object" || Array.isArray(raw)) {
    return null;
  }

  const value = raw as Record<string, unknown>;
  const version = Number(value.version ?? 0);
  const acceptedAt = typeof value.acceptedAt === "string" ? value.acceptedAt : "";
  const userId = typeof value.userId === "string" ? value.userId : undefined;
  if (!acceptedAt || !Number.isFinite(version)) {
    return null;
  }

  return { version, acceptedAt, userId };
}

export function persistLocalPublishConsent(status: ShareConsentStatus): void {
  const value = {
    version: status.version,
    acceptedAt: status.acceptedAt ?? new Date().toISOString(),
    userId: toneSharingState.user?.id ?? "",
  };
  updateAppSetting(storageKeys.publishConsent, value);
}

let shareConsentResolve: ((value: boolean) => void) | null = null;

export function closeShareConsentModal(result: boolean): void {
  const modal = element<HTMLElement>("tone-sharing-consent-modal");
  const checkbox = element<HTMLInputElement>("tone-sharing-consent-checkbox");
  const status = element<HTMLElement>("tone-sharing-consent-status");
  if (modal) {
    modal.style.display = "none";
  }
  if (checkbox) {
    checkbox.checked = false;
  }
  if (status) {
    status.textContent = "";
  }
  if (shareConsentResolve) {
    shareConsentResolve(result);
    shareConsentResolve = null;
  }
}

export async function promptForShareConsent(): Promise<boolean> {
  const modal = element<HTMLElement>("tone-sharing-consent-modal");
  const checkbox = element<HTMLInputElement>("tone-sharing-consent-checkbox");
  const acceptButton = element<HTMLButtonElement>("tone-sharing-consent-accept");
  const cancelButton = element<HTMLButtonElement>("tone-sharing-consent-cancel");
  const closeButton = element<HTMLButtonElement>("tone-sharing-consent-close");
  const status = element<HTMLElement>("tone-sharing-consent-status");
  if (!modal || !checkbox || !acceptButton || !cancelButton || !closeButton || !status) {
    throw new Error("Tone sharing consent modal is not available");
  }

  if (shareConsentResolve) {
    shareConsentResolve(false);
    shareConsentResolve = null;
  }

  checkbox.checked = false;
  acceptButton.disabled = true;
  status.textContent = "";
  modal.style.display = "flex";

  return new Promise<boolean>((resolve) => {
    shareConsentResolve = resolve;
    checkbox.onchange = () => {
      acceptButton.disabled = !checkbox.checked;
    };
    acceptButton.onclick = () => closeShareConsentModal(true);
    cancelButton.onclick = () => closeShareConsentModal(false);
    closeButton.onclick = () => closeShareConsentModal(false);
    modal.onmousedown = (event) => {
      if (event.target === modal) {
        closeShareConsentModal(false);
      }
    };
  });
}

export async function ensurePublishConsent(): Promise<void> {
  if (!toneSharingState.user) {
    throw new Error("Sign in first.");
  }

  const remote = await apiFetch<ShareConsentStatus>("/share-consent/status");
  const local = getLocalPublishConsent();
  const localAccepted = Boolean(
    local && local.version >= remote.version && (!local.userId || local.userId === toneSharingState.user.id)
  );

  if (remote.accepted) {
    if (!localAccepted) {
      persistLocalPublishConsent(remote);
    }
    return;
  }

  const accepted = await promptForShareConsent();
  if (!accepted) {
    throw new Error("Tone sharing consent is required before publishing.");
  }

  const stored = await apiFetch<ShareConsentStatus>("/share-consent/accept", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ version: TONE_SHARING_PUBLISH_CONSENT_VERSION }),
  });
  persistLocalPublishConsent(stored);
}
