/**
 * The panel's shared mutable state, and the storage keys behind it.
 *
 * These were plain module locals when the panel was one file. Splitting it means
 * they need one owner, and they are grouped into objects rather than exposed as
 * `export let`: an ES module's live bindings are readable by importers but only
 * the declaring module may assign them, so a bare `export let` would compile and
 * then fail at the first write from another file.
 */

import { API_BASE_URL } from "../apiConfig.js";
import { Features, isFeatureEnabled } from "../featureFlags.js";
import type { AiToneCombination, BrowseCollectionState, BrowseMode, InstalledPackMetadata, ToneSharingItem, ToneSharingShareTarget, ToneSharingUser } from "./types.js";

export const storageKeys = {
  sessionId: "toneSharing.sessionId",
  installedPacks: "toneSharing.installedPacks",
  publishConsent: "toneSharing.publishConsent"
};

export const TONE_SHARING_PUBLISH_CONSENT_VERSION = 1;

export const FEATURED_PRESET_MAX = 10;

export const SHOW_TONE_SHARING_STATS = false;

export const toneSharingState = {
  apiBase: API_BASE_URL, //"http://127.0.0.1:8787/v1", 
  sessionId: "",
  user: null as ToneSharingUser | null,
  myItems: [] as ToneSharingItem[],
  installedPacks: [] as InstalledPackMetadata[]
};

export const aiSearchState = {
  band: "",
  song: "",
  combinations: [] as AiToneCombination[],
  loading: false,
  error: ""
};

export function isAiToneSearchEnabled(): boolean {
  return isFeatureEnabled(Features.AiToneSearch);
}

export const browseCollections: BrowseCollectionState = {
  page: 1,
  pageSize: 36,
  hasMore: false,
  loadingMore: false,
  items: [],
  packs: [],
};

/** Which browse tab the panel is showing, and the query behind it. */
export const browseState = {
  mode: "featured" as BrowseMode,
  activeSharedTarget: null as ToneSharingShareTarget | null,
  featuredHiddenPresetCount: 0,
  searchQuery: "",
  tagFilter: "",
  /**
   * Bumped on every new browse request, so a slow page response that lands
   * after the user has moved on can recognise itself as stale and drop out.
   */
  requestVersion: 0
};

/** The community preset currently auditioned over the user's own tone. */
export const previewState = {
  itemId: null as string | null,
  priorPresetId: null as string | null
};
