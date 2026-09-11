/**
 * Sign-in and the signed-in user's profile.
 *
 * Auth and profile sit together because they are the same round trip from the
 * panel's point of view: every one of these calls ends by re-reading the user
 * object and pushing it back into the header and the profile form.
 */

import { apiFetch, buildApiUrl, isToneSharingAdmin, updateToneSharingSession } from "./api.js";
import { loadBrowse, loadMine } from "./browse.js";
import { element, setText } from "./dom.js";
import { resizeImageToMaxWidth } from "./images.js";
import { closePackModal } from "./packEditor.js";
import { closeToneSharingPublishPresetModal } from "./publish.js";
import { browseState, toneSharingState } from "./state.js";
import type { ToneSharingUser } from "./types.js";

let profileAvatarPreviewObjectUrl: string | null = null;

let authCodeRequested = false;

export function clearProfileAvatarPreviewObjectUrl(): void {
  if (!profileAvatarPreviewObjectUrl) {
    return;
  }
  URL.revokeObjectURL(profileAvatarPreviewObjectUrl);
  profileAvatarPreviewObjectUrl = null;
}

export function setProfileAvatarPreview(source: string | null): void {
  const wrap = element<HTMLElement>("tone-sharing-avatar-preview-wrap");
  const image = element<HTMLImageElement>("tone-sharing-avatar-preview");
  if (!wrap || !image) {
    return;
  }

  const resolved = source?.trim() ?? "";
  if (!resolved) {
    image.src = "data:image/gif;base64,R0lGODlhAQABAAAAACwAAAAAAQABAAA=";
    image.setAttribute("aria-hidden", "true");
    wrap.hidden = true;
    return;
  }

  image.src = resolved;
  image.removeAttribute("aria-hidden");
  wrap.hidden = false;
}

export function setProfileFieldsFromUser(user: ToneSharingUser | null): void {
  const displayNameInput = element<HTMLInputElement>("tone-sharing-display-name");
  const bioInput = element<HTMLTextAreaElement>("tone-sharing-bio");
  const avatarInput = element<HTMLInputElement>("tone-sharing-avatar-image");

  if (displayNameInput) {
    displayNameInput.value = user?.displayName ?? "";
  }
  if (bioInput) {
    bioInput.value = user?.bio ?? "";
  }
  if (avatarInput) {
    avatarInput.value = "";
  }

  clearProfileAvatarPreviewObjectUrl();
  setProfileAvatarPreview(user?.avatarUrl ? buildApiUrl(user.avatarUrl) : null);
  setText("tone-sharing-profile-status", "");
}

export function previewSelectedProfileAvatar(): void {
  const avatarInput = element<HTMLInputElement>("tone-sharing-avatar-image");
  const selectedFile = avatarInput?.files?.[0] ?? null;

  clearProfileAvatarPreviewObjectUrl();
  if (!selectedFile) {
    setProfileAvatarPreview(toneSharingState.user?.avatarUrl ? buildApiUrl(toneSharingState.user.avatarUrl) : null);
    return;
  }

  profileAvatarPreviewObjectUrl = URL.createObjectURL(selectedFile);
  setProfileAvatarPreview(profileAvatarPreviewObjectUrl);
  setText("tone-sharing-profile-status", "");
}

export function openToneSharingSignInModal(): void {
  const modal = element<HTMLElement>("tone-sharing-signin-modal");
  if (modal) {
    updateAuthButtonVisibility();
    setProfileFieldsFromUser(toneSharingState.user);
    modal.style.display = "flex";
  }
}

export function closeSignInModal(): void {
  const modal = element<HTMLElement>("tone-sharing-signin-modal");
  if (modal) {
    modal.style.display = "none";
  }
}

export function updateAuthButtonVisibility(): void {
  const sendCodeButton = element<HTMLButtonElement>("tone-sharing-send-code");
  const signInButton = element<HTMLButtonElement>("tone-sharing-verify");
  const signOutButton = element<HTMLButtonElement>("tone-sharing-logout");
  const saveProfileButton = element<HTMLButtonElement>("tone-sharing-save-profile");
  const accountChip = element<HTMLButtonElement>("tone-sharing-account-btn");
  const createPackButton = element<HTMLButtonElement>("tone-sharing-open-pack-modal");
  const authFields = element<HTMLElement>("tone-sharing-auth-fields");
  const profileFields = element<HTMLElement>("tone-sharing-profile-fields");
  const signedIn = !!toneSharingState.user;
  const showVerifyActions = !signedIn && authCodeRequested;

  if (sendCodeButton) {
    sendCodeButton.style.display = signedIn ? "none" : "";
    sendCodeButton.textContent = authCodeRequested ? "Re-Send Code" : "Send Code";
  }

  if (signInButton) {
    signInButton.style.display = showVerifyActions ? "" : "none";
  }
  if (signOutButton) {
    signOutButton.style.display = signedIn ? "" : "none";
  }
  if (saveProfileButton) {
    saveProfileButton.style.display = signedIn ? "" : "none";
  }
  if (authFields) {
    authFields.style.display = signedIn ? "none" : "";
  }
  if (profileFields) {
    profileFields.style.display = signedIn ? "" : "none";
  }
  if (accountChip) {
    const chipLabel = signedIn ? (toneSharingState.user?.displayName?.trim() || toneSharingState.user?.email || "Account") : "Sign In";
    accountChip.textContent = chipLabel;
    accountChip.classList.toggle("signed-in", signedIn);
  }
  if (createPackButton) {
    createPackButton.disabled = !signedIn;
    createPackButton.title = signedIn ? "Create a new pack" : "Sign in to create packs";
  }
  const reviewButton = element<HTMLButtonElement>("tone-sharing-browse-review");
  if (reviewButton) {
    reviewButton.style.display = isToneSharingAdmin() ? "" : "none";
  }
  if (!isToneSharingAdmin() && browseState.mode === "review") {
    browseState.mode = "featured";
  }
  if (!signedIn) {
    setText("tone-sharing-profile-status", "");
    closeToneSharingPublishPresetModal();
    closePackModal();
  }
}

export async function loadAuthSession(): Promise<void> {
  try {
    const data = await apiFetch<{ user: ToneSharingUser | null }>("/auth/me");
    toneSharingState.user = data.user;
    setProfileFieldsFromUser(data.user);
    authCodeRequested = false;
    updateAuthButtonVisibility();
    if (data.user) {
      setText("tone-sharing-auth-status", `Signed in as ${data.user.email}`);
      await loadMine();
    } else {
      setText("tone-sharing-auth-status", "Signed out");
    }
  } catch (error) {
    setProfileFieldsFromUser(null);
    setText("tone-sharing-auth-status", `Auth check failed: ${(error as Error).message}`);
  }
}

export async function sendCode(): Promise<void> {
  const email = element<HTMLInputElement>("tone-sharing-email")?.value.trim() ?? "";
  if (!email) {
    setText("tone-sharing-auth-status", "Enter an email address");
    return;
  }

  setText("tone-sharing-auth-status", "Sending code...");
  try {
    await apiFetch("/auth/start", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ email })
    });
    authCodeRequested = true;
    updateAuthButtonVisibility();
    setText("tone-sharing-auth-status", "Code sent. Check your email.");
  } catch (error) {
    setText("tone-sharing-auth-status", `Send code failed: ${(error as Error).message}`);
  }
}

export async function verifyCode(): Promise<void> {
  const email = element<HTMLInputElement>("tone-sharing-email")?.value.trim() ?? "";
  const code = element<HTMLInputElement>("tone-sharing-code")?.value.trim() ?? "";
  if (!email || !code) {
    setText("tone-sharing-auth-status", "Enter email and code");
    return;
  }

  setText("tone-sharing-auth-status", "Signing in...");
  try {
    const data = await apiFetch<{ sessionId?: string; user: ToneSharingUser }>("/auth/verify", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ email, code })
    });

    toneSharingState.user = data.user;
    setProfileFieldsFromUser(data.user);
    updateToneSharingSession(data.sessionId);
    authCodeRequested = false;
    updateAuthButtonVisibility();
    setText("tone-sharing-auth-status", `Signed in as ${data.user.email}`);
    closeSignInModal();
    await Promise.all([loadBrowse(), loadMine()]);
  } catch (error) {
    setText("tone-sharing-auth-status", `Sign-in failed: ${(error as Error).message}`);
  }
}

export async function signOut(): Promise<void> {
  try {
    await apiFetch("/auth/logout", { method: "POST" });
  } catch {
  }

  updateToneSharingSession("");
  toneSharingState.user = null;
  setProfileFieldsFromUser(null);
  authCodeRequested = false;
  updateAuthButtonVisibility();
  setText("tone-sharing-auth-status", "Signed out");
  closeSignInModal();
  await loadBrowse();
}

export async function uploadProfileAvatar(file: File): Promise<string> {
  const resized = await resizeImageToMaxWidth(file, 512);
  const avatarBlob = resized.blob;

  const init = await apiFetch<{ uploadId: string }>("/uploads/init", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({
      kind: "thumbnail",
      mimeType: avatarBlob.type || "application/octet-stream",
      byteSize: avatarBlob.size,
    })
  });

  const uploadResponse = await fetch(buildApiUrl(`/uploads/${init.uploadId}`), {
    method: "PUT",
    headers: {
      "content-type": avatarBlob.type || "application/octet-stream",
      ...(toneSharingState.sessionId ? { "x-session-id": toneSharingState.sessionId } : {}),
    },
    body: avatarBlob,
    credentials: "include",
  });

  const uploadPayload = await uploadResponse.json().catch(() => null);
  if (!uploadResponse.ok || uploadPayload?.ok === false) {
    throw new Error(uploadPayload?.error?.message || `Avatar upload failed (${uploadResponse.status})`);
  }

  const complete = await apiFetch<{ assetId: string }>("/uploads/complete", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ uploadId: init.uploadId }),
  });

  return complete.assetId;
}

export async function saveProfile(): Promise<void> {
  if (!toneSharingState.user) {
    setText("tone-sharing-auth-status", "Sign in to edit your profile.");
    return;
  }

  const saveButton = element<HTMLButtonElement>("tone-sharing-save-profile");
  const displayNameInput = element<HTMLInputElement>("tone-sharing-display-name");
  const bioInput = element<HTMLTextAreaElement>("tone-sharing-bio");
  const avatarInput = element<HTMLInputElement>("tone-sharing-avatar-image");

  const displayName = displayNameInput?.value.trim() ?? "";
  const bio = bioInput?.value.trim() ?? "";
  const avatarFile = avatarInput?.files?.[0] ?? null;

  if (saveButton) {
    saveButton.disabled = true;
  }
  setText("tone-sharing-profile-status", "Saving profile...");

  try {
    let avatarAssetId: string | null | undefined = undefined;
    if (avatarFile) {
      setText("tone-sharing-profile-status", "Uploading avatar...");
      avatarAssetId = await uploadProfileAvatar(avatarFile);
    }

    const payload: {
      displayName?: string;
      bio?: string;
      avatarAssetId?: string | null;
    } = {
      displayName,
      bio,
    };
    if (avatarAssetId !== undefined) {
      payload.avatarAssetId = avatarAssetId;
    }

    const result = await apiFetch<{ user: ToneSharingUser | null }>("/auth/profile", {
      method: "PATCH",
      headers: { "content-type": "application/json" },
      body: JSON.stringify(payload),
    });

    if (!result.user) {
      throw new Error("Profile response missing user.");
    }

    toneSharingState.user = result.user;
    setProfileFieldsFromUser(result.user);
    updateAuthButtonVisibility();
    setText("tone-sharing-auth-status", `Signed in as ${result.user.email}`);
    setText("tone-sharing-profile-status", "Profile saved.");
    await Promise.all([loadBrowse(), loadMine()]);
  } catch (error) {
    setText("tone-sharing-profile-status", `Profile save failed: ${(error as Error).message}`);
  } finally {
    if (saveButton) {
      saveButton.disabled = false;
    }
  }
}
