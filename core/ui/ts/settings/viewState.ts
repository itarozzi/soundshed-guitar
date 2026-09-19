/**
 * Which settings tab the user was last on, pushed back to the host so it
 * survives a restart — except while the host is restoring that view itself.
 */

import { postMessage } from "../bridge.js";
import { getNavigationViewState, mergeNavigationViewState } from "../navigationState.js";

let suppressViewStateUpdates = false;

export function updateSettingsViewState(update: { equipmentTab?: string; libraryTab?: string; advancedTab?: string }): void {
  if (!mergeNavigationViewState({ settings: update })) {
    return;
  }

  if (suppressViewStateUpdates) {
    return;
  }
  postMessage({ type: "uiViewStateChanged", viewState: getNavigationViewState() });
}

export function setSettingsViewStateSuppressed(suppressed: boolean): void {
  suppressViewStateUpdates = suppressed;
}

/**
 * Whether a view-state change should stay local instead of being pushed back to
 * the host. Set while the host is itself restoring a saved view, so replaying
 * that view does not echo straight back at it.
 */
export function isSettingsViewStateSuppressed(): boolean {
  return suppressViewStateUpdates;
}
