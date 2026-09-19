/**
 * Input calibration: measuring the peak an instrument actually produces, and
 * storing the trim that brings it to the level the amp models expect.
 *
 * A profile is trained in a modal that watches the live input peak; the toolbar
 * menu is the same data in a smaller form, so both render from the same read of
 * the stored profiles.
 */

import { postMessage } from "../bridge.js";
import { updateAppSetting } from "../appSettingsStore.js";
import { showConfirm } from "../dialogs.js";
import { showNotification } from "../notifications.js";
import { uiState } from "../state.js";
import type { AppSettingValue, UserInputCalibrationProfile } from "../types.js";
import { userInputCalibrationCancelButton, userInputCalibrationCapturedPeak, userInputCalibrationCloseButton, userInputCalibrationDeleteButton, userInputCalibrationDescriptionInput, userInputCalibrationLivePeak, userInputCalibrationModal, userInputCalibrationNameInput, userInputCalibrationProfileSelect, userInputCalibrationRecommendedTrim, userInputCalibrationResetButton, userInputCalibrationSaveButton, userInputCalibrationStatus, userInputCalibrationSummary, userInputCalibrationToolbarMenu, userInputCalibrationToolbarTrigger, userInputCalibrationTrainButton } from "./dom.js";
import { USER_INPUT_CALIBRATION_ACTIVE_PROFILE_SETTING, USER_INPUT_CALIBRATION_NONE_VALUE, USER_INPUT_CALIBRATION_PROFILES_SETTING, USER_INPUT_CALIBRATION_TARGET_PEAK_DBFS } from "./keys.js";
import { getSettingValue } from "./values.js";

let userInputCalibrationControlsInitialized = false;

let userInputCalibrationTrainingPeakDbfs = Number.NEGATIVE_INFINITY;

let userInputCalibrationLivePeakDbfs = Number.NEGATIVE_INFINITY;

let userInputCalibrationTrainingBypassActive = false;

export function readUserInputCalibrationProfiles(): UserInputCalibrationProfile[] {
  const raw = getSettingValue(USER_INPUT_CALIBRATION_PROFILES_SETTING);
  if (!Array.isArray(raw)) {
    return [];
  }

  return raw.flatMap((entry) => {
    if (!entry || typeof entry !== "object" || Array.isArray(entry)) {
      return [];
    }

    const candidate = entry as Record<string, unknown>;
    const id = typeof candidate.id === "string" ? candidate.id.trim() : "";
    const name = typeof candidate.name === "string" ? candidate.name.trim() : "";
    if (!id || !name) {
      return [];
    }

    const capturedPeakDbfs = Number(candidate.capturedPeakDbfs);
    const targetPeakDbfsRaw = Number(candidate.targetPeakDbfs);
    const targetPeakDbfs = Number.isFinite(targetPeakDbfsRaw)
      ? targetPeakDbfsRaw
      : USER_INPUT_CALIBRATION_TARGET_PEAK_DBFS;
    const gainDbRaw = Number(candidate.gainDb);
    const gainDb = Number.isFinite(gainDbRaw)
      ? gainDbRaw
      : (Number.isFinite(capturedPeakDbfs) ? targetPeakDbfs - capturedPeakDbfs : 0.0);

    return [{
      id,
      name,
      description: typeof candidate.description === "string" ? candidate.description : "",
      capturedPeakDbfs: Number.isFinite(capturedPeakDbfs) ? capturedPeakDbfs : Number.NaN,
      targetPeakDbfs,
      gainDb: Math.max(-24.0, Math.min(24.0, gainDb)),
      createdAt: typeof candidate.createdAt === "string" ? candidate.createdAt : undefined,
      updatedAt: typeof candidate.updatedAt === "string" ? candidate.updatedAt : undefined,
    } satisfies UserInputCalibrationProfile];
  });
}

export function readActiveUserInputCalibrationProfileId(): string | null {
  const raw = getSettingValue(USER_INPUT_CALIBRATION_ACTIVE_PROFILE_SETTING);
  if (typeof raw !== "string") {
    return null;
  }
  const trimmed = raw.trim();
  return trimmed ? trimmed : null;
}

export function getActiveUserInputCalibrationProfile(): UserInputCalibrationProfile | null {
  const profiles = readUserInputCalibrationProfiles();
  const activeProfileId = readActiveUserInputCalibrationProfileId();
  if (!activeProfileId) {
    return null;
  }
  return profiles.find((profile) => profile.id === activeProfileId) ?? null;
}

export function serializeUserInputCalibrationProfiles(profiles: UserInputCalibrationProfile[]): AppSettingValue {
  return profiles.map((profile) => {
    const payload: Record<string, AppSettingValue> = {
      id: profile.id,
      name: profile.name,
      description: profile.description,
      capturedPeakDbfs: profile.capturedPeakDbfs,
      targetPeakDbfs: profile.targetPeakDbfs,
      gainDb: profile.gainDb,
    };
    if (profile.createdAt) {
      payload.createdAt = profile.createdAt;
    }
    if (profile.updatedAt) {
      payload.updatedAt = profile.updatedAt;
    }
    return payload;
  });
}

export function persistUserInputCalibrationState(profiles: UserInputCalibrationProfile[], activeProfileId: string | null): void {
  const serializedProfiles = serializeUserInputCalibrationProfiles(profiles);
  updateAppSetting(USER_INPUT_CALIBRATION_PROFILES_SETTING, serializedProfiles);
  updateAppSetting(USER_INPUT_CALIBRATION_ACTIVE_PROFILE_SETTING, activeProfileId);
  refreshUserInputCalibrationView();
}

export function setUserInputCalibrationTrainingBypassActive(active: boolean): void {
  if (userInputCalibrationTrainingBypassActive === active) {
    return;
  }

  userInputCalibrationTrainingBypassActive = active;
  postMessage({ type: "setUserInputCalibrationTrainingActive", active });
  refreshUserInputCalibrationView();
}

export function readDisplayedUserInputCalibrationProfileId(): string | null {
  if (userInputCalibrationTrainingBypassActive) {
    return null;
  }

  return readActiveUserInputCalibrationProfileId();
}

export function formatDbfsValue(value: number): string {
  return Number.isFinite(value) ? `${value.toFixed(1)} dBFS` : "-";
}

export function formatSignedDbValue(value: number): string {
  if (!Number.isFinite(value)) {
    return "-";
  }
  const sign = value >= 0 ? "+" : "";
  return `${sign}${value.toFixed(1)} dB`;
}

export function getCurrentRawInputPeakDbfs(): number {
  return uiState.signalDiagnostics?.rawInput?.peakDbfs
    ?? uiState.signalDiagnostics?.input?.peakDbfs
    ?? Number.NEGATIVE_INFINITY;
}

export function isUserInputCalibrationModalOpen(): boolean {
  return Boolean(userInputCalibrationModal && userInputCalibrationModal.style.display !== "none");
}

export function isUserInputCalibrationToolbarMenuOpen(): boolean {
  return Boolean(userInputCalibrationToolbarMenu && !userInputCalibrationToolbarMenu.hidden);
}

export function positionUserInputCalibrationToolbarMenu(): void {
  if (!userInputCalibrationToolbarMenu || !userInputCalibrationToolbarTrigger) {
    return;
  }

  const offsetParent = userInputCalibrationToolbarMenu.offsetParent;
  if (!(offsetParent instanceof HTMLElement)) {
    return;
  }

  const viewportMargin = 8;
  const parentRect = offsetParent.getBoundingClientRect();
  const triggerRect = userInputCalibrationToolbarTrigger.getBoundingClientRect();
  const menuWidth = userInputCalibrationToolbarMenu.offsetWidth;

  const centeredLeft = (triggerRect.left + (triggerRect.width / 2)) - parentRect.left - (menuWidth / 2);
  const minLeft = viewportMargin - parentRect.left;
  const maxLeft = window.innerWidth - viewportMargin - parentRect.left - menuWidth;
  const clampedLeft = Math.min(Math.max(centeredLeft, minLeft), Math.max(minLeft, maxLeft));

  userInputCalibrationToolbarMenu.style.left = `${Math.round(clampedLeft)}px`;
}

export function closeUserInputCalibrationToolbarMenu(): void {
  if (userInputCalibrationToolbarMenu) {
    userInputCalibrationToolbarMenu.hidden = true;
  }
  userInputCalibrationToolbarTrigger?.setAttribute("aria-expanded", "false");
  userInputCalibrationToolbarTrigger
    ?.closest(".control-bar")
    ?.classList.remove("has-open-calibration");
}

export function openUserInputCalibrationToolbarMenu(): void {
  if (!userInputCalibrationToolbarMenu) {
    return;
  }

  userInputCalibrationToolbarMenu.hidden = false;
  positionUserInputCalibrationToolbarMenu();
  userInputCalibrationToolbarTrigger?.setAttribute("aria-expanded", "true");
  userInputCalibrationToolbarTrigger
    ?.closest(".control-bar")
    ?.classList.add("has-open-calibration");
}

export function toggleUserInputCalibrationToolbarMenu(): void {
  if (isUserInputCalibrationToolbarMenuOpen()) {
    closeUserInputCalibrationToolbarMenu();
    return;
  }

  openUserInputCalibrationToolbarMenu();
}

export function refreshUserInputCalibrationTrainingView(): void {
  if (userInputCalibrationLivePeak) {
    userInputCalibrationLivePeak.textContent = formatDbfsValue(userInputCalibrationLivePeakDbfs);
  }
  if (userInputCalibrationCapturedPeak) {
    userInputCalibrationCapturedPeak.textContent = formatDbfsValue(userInputCalibrationTrainingPeakDbfs);
  }

  const recommendedTrimDb = Number.isFinite(userInputCalibrationTrainingPeakDbfs)
    ? Math.max(-24.0, Math.min(24.0, USER_INPUT_CALIBRATION_TARGET_PEAK_DBFS - userInputCalibrationTrainingPeakDbfs))
    : Number.NaN;
  if (userInputCalibrationRecommendedTrim) {
    userInputCalibrationRecommendedTrim.textContent = formatSignedDbValue(recommendedTrimDb);
  }

  const rawInputClipped = Boolean(uiState.signalDiagnostics?.rawInput?.clipped);
  const trimmedName = userInputCalibrationNameInput?.value.trim() ?? "";
  const canSave = trimmedName.length > 0 && Number.isFinite(userInputCalibrationTrainingPeakDbfs) && !rawInputClipped;

  if (userInputCalibrationSaveButton) {
    userInputCalibrationSaveButton.disabled = !canSave;
  }

  if (!userInputCalibrationStatus) {
    return;
  }

  if (rawInputClipped) {
    userInputCalibrationStatus.textContent = "Input clipped during training. Back the interface gain down and capture again.";
    return;
  }
  if (!Number.isFinite(userInputCalibrationTrainingPeakDbfs)) {
    userInputCalibrationStatus.textContent = "Play your loudest picking or strumming to capture a training peak.";
    return;
  }
  userInputCalibrationStatus.textContent = `Captured ${formatDbfsValue(userInputCalibrationTrainingPeakDbfs)}. Saving this profile will apply ${formatSignedDbValue(recommendedTrimDb)} toward a ${formatDbfsValue(USER_INPUT_CALIBRATION_TARGET_PEAK_DBFS)} target.`;
}

export function resetUserInputCalibrationTrainingPeak(): void {
  userInputCalibrationLivePeakDbfs = getCurrentRawInputPeakDbfs();
  userInputCalibrationTrainingPeakDbfs = Number.isFinite(userInputCalibrationLivePeakDbfs)
    ? userInputCalibrationLivePeakDbfs
    : Number.NEGATIVE_INFINITY;
  refreshUserInputCalibrationTrainingView();
}

export function openUserInputCalibrationTrainingModal(): void {
  if (!userInputCalibrationModal || isUserInputCalibrationModalOpen()) {
    return;
  }

  closeUserInputCalibrationToolbarMenu();
  setUserInputCalibrationTrainingBypassActive(true);
  if (userInputCalibrationNameInput) userInputCalibrationNameInput.value = "";
  if (userInputCalibrationDescriptionInput) userInputCalibrationDescriptionInput.value = "";
  userInputCalibrationLivePeakDbfs = Number.NEGATIVE_INFINITY;
  userInputCalibrationTrainingPeakDbfs = Number.NEGATIVE_INFINITY;
  userInputCalibrationModal.style.display = "flex";
  refreshUserInputCalibrationTrainingView();
  userInputCalibrationNameInput?.focus();
}

export function closeUserInputCalibrationTrainingModal(): void {
  if (!userInputCalibrationModal) {
    return;
  }
  userInputCalibrationModal.style.display = "none";
  userInputCalibrationLivePeakDbfs = Number.NEGATIVE_INFINITY;
  userInputCalibrationTrainingPeakDbfs = Number.NEGATIVE_INFINITY;
  setUserInputCalibrationTrainingBypassActive(false);
}

export async function deleteActiveUserInputCalibrationProfile(): Promise<void> {
  const activeProfile = getActiveUserInputCalibrationProfile();
  if (!activeProfile) {
    return;
  }

  await deleteUserInputCalibrationProfile(activeProfile.id);
}

export async function deleteUserInputCalibrationProfile(profileId: string): Promise<void> {
  const profiles = readUserInputCalibrationProfiles();
  const profile = profiles.find((entry) => entry.id === profileId) ?? null;
  if (!profile) {
    return;
  }

  const confirmed = await showConfirm(
    `Delete input calibration profile "${profile.name}"?`,
    "Delete Input Calibration",
  );
  if (!confirmed) {
    return;
  }

  const activeProfileId = readActiveUserInputCalibrationProfileId();
  const remainingProfiles = profiles.filter((entry) => entry.id !== profile.id);
  persistUserInputCalibrationState(remainingProfiles, activeProfileId === profile.id ? null : activeProfileId);
  showNotification("Input calibration deleted", profile.name);
}

export function saveUserInputCalibrationProfile(): void {
  const name = userInputCalibrationNameInput?.value.trim() ?? "";
  if (!name) {
    showNotification("Input calibration name required");
    return;
  }

  if (!Number.isFinite(userInputCalibrationTrainingPeakDbfs)) {
    showNotification("No training level captured", "Play your guitar first.");
    return;
  }

  if (uiState.signalDiagnostics?.rawInput?.clipped) {
    showNotification("Input clipped during training", "Reduce interface gain and capture again.");
    return;
  }

  const gainDb = Math.max(-24.0, Math.min(24.0, USER_INPUT_CALIBRATION_TARGET_PEAK_DBFS - userInputCalibrationTrainingPeakDbfs));
  const timestamp = new Date().toISOString();
  const profile: UserInputCalibrationProfile = {
    id: globalThis.crypto?.randomUUID?.() ?? `input-cal-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`,
    name,
    description: userInputCalibrationDescriptionInput?.value.trim() ?? "",
    capturedPeakDbfs: userInputCalibrationTrainingPeakDbfs,
    targetPeakDbfs: USER_INPUT_CALIBRATION_TARGET_PEAK_DBFS,
    gainDb,
    createdAt: timestamp,
    updatedAt: timestamp,
  };

  const profiles = readUserInputCalibrationProfiles();
  profiles.push(profile);
  persistUserInputCalibrationState(profiles, profile.id);
  closeUserInputCalibrationTrainingModal();
  showNotification("Input calibration saved", `${profile.name} • ${formatSignedDbValue(profile.gainDb)}`);
}

export function applyUserInputCalibrationProfileSelection(activeProfileId: string | null): void {
  updateAppSetting(USER_INPUT_CALIBRATION_ACTIVE_PROFILE_SETTING, activeProfileId);
  refreshUserInputCalibrationView();
}

export function handleUserInputCalibrationProfileSelection(): void {
  const selectedValue = userInputCalibrationProfileSelect?.value ?? USER_INPUT_CALIBRATION_NONE_VALUE;
  const activeProfileId = selectedValue === USER_INPUT_CALIBRATION_NONE_VALUE ? null : selectedValue;
  applyUserInputCalibrationProfileSelection(activeProfileId);
}

export function renderUserInputCalibrationToolbarMenu(profiles: UserInputCalibrationProfile[], activeProfile: UserInputCalibrationProfile | null): void {
  // Bound to a local so the null check narrows inside the closures below; a
  // narrowing on an imported binding does not survive into a closure.
  const toolbarMenu = userInputCalibrationToolbarMenu;
  if (!toolbarMenu) {
    return;
  }

  toolbarMenu.innerHTML = "";

  const header = document.createElement("div");
  header.className = "input-calibration-toolbar-menu-header";

  const title = document.createElement("span");
  title.className = "input-calibration-toolbar-menu-title";
  title.textContent = "Input Calibration";
  header.appendChild(title);

  const subtitle = document.createElement("span");
  subtitle.className = "input-calibration-toolbar-menu-subtitle";
  subtitle.textContent = userInputCalibrationTrainingBypassActive
    ? "Training bypass active"
    : (activeProfile ? `${activeProfile.name} active` : "No active calibration");
  header.appendChild(subtitle);

  toolbarMenu.appendChild(header);

  const appendProfileItem = (profile: UserInputCalibrationProfile | null): void => {
    const itemRow = document.createElement("div");
    itemRow.className = "input-calibration-toolbar-row";

    const button = document.createElement("button");
    button.type = "button";
    button.className = "input-calibration-toolbar-item";
    button.dataset.profileId = profile?.id ?? USER_INPUT_CALIBRATION_NONE_VALUE;
    button.setAttribute("role", "menuitemradio");

    const isActive = profile ? activeProfile?.id === profile.id : !activeProfile;
    button.classList.toggle("active", isActive);
    button.setAttribute("aria-checked", isActive ? "true" : "false");

    const copy = document.createElement("span");
    copy.className = "input-calibration-toolbar-item-copy";

    const name = document.createElement("span");
    name.className = "input-calibration-toolbar-item-name";
    name.textContent = profile?.name ?? "No calibration";
    copy.appendChild(name);

    const note = document.createElement("span");
    note.className = "input-calibration-toolbar-item-note";
    note.textContent = profile
      ? (isActive
        ? `Applies ${formatSignedDbValue(profile.gainDb)} toward ${formatDbfsValue(profile.targetPeakDbfs)}`
        : (profile.description || `Captured ${formatDbfsValue(profile.capturedPeakDbfs)}`))
      : "Use raw input without a global trim";
    copy.appendChild(note);

    button.appendChild(copy);

    const meta = document.createElement("span");
    meta.className = "input-calibration-toolbar-item-meta";
    meta.textContent = isActive ? "ACTIVE" : (profile ? formatSignedDbValue(profile.gainDb) : "OFF");
    button.appendChild(meta);

    itemRow.appendChild(button);

    if (profile) {
      const deleteButton = document.createElement("button");
      deleteButton.type = "button";
      deleteButton.className = "input-calibration-toolbar-item-delete";
      deleteButton.dataset.action = "delete";
      deleteButton.dataset.profileId = profile.id;
      deleteButton.setAttribute("role", "menuitem");
      deleteButton.setAttribute("aria-label", `Delete ${profile.name}`);
      deleteButton.title = `Delete ${profile.name}`;
      deleteButton.innerHTML = '<svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><polyline points="3 6 5 6 21 6"/><path d="M19 6l-1 14a2 2 0 0 1-2 2H8a2 2 0 0 1-2-2L5 6"/><path d="M10 11v6"/><path d="M14 11v6"/><path d="M9 6V4a1 1 0 0 1 1-1h4a1 1 0 0 1 1 1v2"/></svg>';
      itemRow.appendChild(deleteButton);
    }

    toolbarMenu.appendChild(itemRow);
  };

  appendProfileItem(null);
  profiles.forEach((profile) => appendProfileItem(profile));

  const divider = document.createElement("div");
  divider.className = "input-calibration-toolbar-menu-divider";
  toolbarMenu.appendChild(divider);

  const trainButton = document.createElement("button");
  trainButton.type = "button";
  trainButton.className = "input-calibration-toolbar-item action";
  trainButton.dataset.action = "train";
  trainButton.setAttribute("role", "menuitem");

  const trainCopy = document.createElement("span");
  trainCopy.className = "input-calibration-toolbar-item-copy";

  const trainName = document.createElement("span");
  trainName.className = "input-calibration-toolbar-item-name";
  trainName.textContent = "Train Input Level";
  trainCopy.appendChild(trainName);

  const trainNote = document.createElement("span");
  trainNote.className = "input-calibration-toolbar-item-note";
  trainNote.textContent = "Open the training dialog and save a new profile";
  trainCopy.appendChild(trainNote);

  trainButton.appendChild(trainCopy);

  const trainMeta = document.createElement("span");
  trainMeta.className = "input-calibration-toolbar-item-meta";
  trainMeta.textContent = "TRAIN";
  trainButton.appendChild(trainMeta);

  toolbarMenu.appendChild(trainButton);

  if (userInputCalibrationToolbarTrigger) {
    userInputCalibrationToolbarTrigger.classList.toggle("has-active-calibration", Boolean(activeProfile));
    userInputCalibrationToolbarTrigger.title = activeProfile
      ? `Input calibration options (${activeProfile.name})`
      : "Input calibration options";
    userInputCalibrationToolbarTrigger.setAttribute(
      "aria-label",
      activeProfile
        ? `Input calibration options. Active profile ${activeProfile.name}`
        : "Input calibration options",
    );
  }
}

export async function handleUserInputCalibrationToolbarMenuClick(event: MouseEvent): Promise<void> {
  const target = event.target;
  if (!(target instanceof HTMLElement)) {
    return;
  }

  const deleteButton = target.closest(".input-calibration-toolbar-item-delete") as HTMLButtonElement | null;
  if (deleteButton) {
    const profileId = deleteButton.dataset.profileId?.trim();
    if (!profileId) {
      return;
    }

    await deleteUserInputCalibrationProfile(profileId);
    return;
  }

  const item = target.closest(".input-calibration-toolbar-item") as HTMLButtonElement | null;
  if (!item) {
    return;
  }

  if (item.dataset.action === "train") {
    openUserInputCalibrationTrainingModal();
    return;
  }

  const selectedValue = item.dataset.profileId ?? USER_INPUT_CALIBRATION_NONE_VALUE;
  const activeProfileId = selectedValue === USER_INPUT_CALIBRATION_NONE_VALUE ? null : selectedValue;
  applyUserInputCalibrationProfileSelection(activeProfileId);
  closeUserInputCalibrationToolbarMenu();
}

export function refreshUserInputCalibrationView(): void {
  // Bound to a local so the null check narrows inside the closures below; a
  // narrowing on an imported binding does not survive into a closure.
  const profileSelect = userInputCalibrationProfileSelect;
  const profiles = readUserInputCalibrationProfiles();
  const persistedActiveProfileId = readActiveUserInputCalibrationProfileId();
  const activeProfileId = readDisplayedUserInputCalibrationProfileId();
  const activeProfile = profiles.find((profile) => profile.id === activeProfileId) ?? null;
  const persistedActiveProfile = profiles.find((profile) => profile.id === persistedActiveProfileId) ?? null;

  if (profileSelect) {
    profileSelect.innerHTML = "";

    const noneOption = document.createElement("option");
    noneOption.value = USER_INPUT_CALIBRATION_NONE_VALUE;
    noneOption.textContent = "No calibration";
    profileSelect.appendChild(noneOption);

    profiles.forEach((profile) => {
      const option = document.createElement("option");
      option.value = profile.id;
      option.textContent = profile.name;
      profileSelect.appendChild(option);
    });

    profileSelect.value = activeProfile ? activeProfile.id : USER_INPUT_CALIBRATION_NONE_VALUE;
  }

  if (userInputCalibrationDeleteButton) {
    userInputCalibrationDeleteButton.disabled = !activeProfile;
  }

  renderUserInputCalibrationToolbarMenu(profiles, activeProfile);

  if (userInputCalibrationSummary) {
    const currentPeakDbfs = getCurrentRawInputPeakDbfs();
    if (userInputCalibrationTrainingBypassActive) {
      userInputCalibrationSummary.textContent = persistedActiveProfile
        ? `${persistedActiveProfile.name} temporarily bypassed while training a new input level profile.`
        : "Input calibration temporarily bypassed while training a new profile.";
    } else if (activeProfile) {
      const description = activeProfile.description ? `${activeProfile.description}. ` : "";
      const currentPeak = Number.isFinite(currentPeakDbfs) ? ` Current raw peak: ${formatDbfsValue(currentPeakDbfs)}.` : "";
      userInputCalibrationSummary.textContent = `${activeProfile.name}. ${description}Applies ${formatSignedDbValue(activeProfile.gainDb)} toward ${formatDbfsValue(activeProfile.targetPeakDbfs)} from a captured ${formatDbfsValue(activeProfile.capturedPeakDbfs)}.${currentPeak}`;
    } else {
      userInputCalibrationSummary.textContent = "No global input calibration active. Train one profile for each guitar or interface gain position you want Soundshed to normalize.";
    }
  }

  if (isUserInputCalibrationToolbarMenuOpen()) {
    positionUserInputCalibrationToolbarMenu();
  }

  if (isUserInputCalibrationModalOpen()) {
    refreshUserInputCalibrationTrainingView();
  }
}

export function initUserInputCalibrationControls(): void {
  if (userInputCalibrationControlsInitialized) {
    refreshUserInputCalibrationView();
    return;
  }

  userInputCalibrationControlsInitialized = true;
  userInputCalibrationProfileSelect?.addEventListener("change", handleUserInputCalibrationProfileSelection);
  userInputCalibrationTrainButton?.addEventListener("click", openUserInputCalibrationTrainingModal);
  userInputCalibrationDeleteButton?.addEventListener("click", () => void deleteActiveUserInputCalibrationProfile());
  userInputCalibrationToolbarTrigger?.addEventListener("click", toggleUserInputCalibrationToolbarMenu);
  userInputCalibrationToolbarMenu?.addEventListener("click", (event) => void handleUserInputCalibrationToolbarMenuClick(event));
  userInputCalibrationCloseButton?.addEventListener("click", closeUserInputCalibrationTrainingModal);
  userInputCalibrationCancelButton?.addEventListener("click", closeUserInputCalibrationTrainingModal);
  userInputCalibrationResetButton?.addEventListener("click", resetUserInputCalibrationTrainingPeak);
  userInputCalibrationSaveButton?.addEventListener("click", saveUserInputCalibrationProfile);
  userInputCalibrationNameInput?.addEventListener("input", refreshUserInputCalibrationTrainingView);
  userInputCalibrationDescriptionInput?.addEventListener("input", refreshUserInputCalibrationTrainingView);
  userInputCalibrationModal?.addEventListener("mousedown", (event) => {
    if (event.target === userInputCalibrationModal) {
      closeUserInputCalibrationTrainingModal();
    }
  });
  document.addEventListener("mousedown", (event) => {
    const target = event.target;
    if (!(target instanceof Node)) {
      return;
    }

    if (userInputCalibrationToolbarMenu?.contains(target) || userInputCalibrationToolbarTrigger?.contains(target)) {
      return;
    }

    closeUserInputCalibrationToolbarMenu();
  });
  document.addEventListener("keydown", (event) => {
    if (event.key !== "Escape") {
      return;
    }

    if (isUserInputCalibrationToolbarMenuOpen()) {
      closeUserInputCalibrationToolbarMenu();
    }

    if (isUserInputCalibrationModalOpen()) {
      closeUserInputCalibrationTrainingModal();
    }
  });
  window.addEventListener("resize", () => {
    if (isUserInputCalibrationToolbarMenuOpen()) {
      positionUserInputCalibrationToolbarMenu();
    }
  });
  refreshUserInputCalibrationView();
}

export function handleUserInputCalibrationDiagnosticsUpdate(): void {
  userInputCalibrationLivePeakDbfs = getCurrentRawInputPeakDbfs();
  if (!isUserInputCalibrationModalOpen()) {
    return;
  }

  if (Number.isFinite(userInputCalibrationLivePeakDbfs)
      && (!Number.isFinite(userInputCalibrationTrainingPeakDbfs)
        || userInputCalibrationLivePeakDbfs > userInputCalibrationTrainingPeakDbfs)) {
    userInputCalibrationTrainingPeakDbfs = userInputCalibrationLivePeakDbfs;
  }

  refreshUserInputCalibrationTrainingView();
}
