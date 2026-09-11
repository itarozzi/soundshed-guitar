/**
 * Shapes the tone sharing API returns, and the shapes this panel keeps locally.
 *
 * Types only, so importing this costs nothing at runtime and it can sit at the
 * bottom of the feature's dependency graph.
 */

export type ToneSharingUser = {
  id: string;
  email: string;
  role: string;
  displayName?: string | null;
  bio?: string | null;
  creatorProfileHandle?: string | null;
  avatarAssetId?: string | null;
  avatarUrl?: string | null;
};

export type ToneSharingItem = {
  id: string;
  title: string;
  type: string;
  featured?: boolean;
  creatorUserId?: string | null;
  moderationStatus?: string;
  creatorEmail?: string | null;
  creatorDisplayName?: string | null;
  creatorHandle?: string | null;
  creatorProfileHandle?: string | null;
  profileHandle?: string | null;
  creatorBio?: string | null;
  creatorAvatarUrl?: string | null;
  favoriteCount?: number;
  ratingCount?: number;
  averageRating?: number | null;
  currentUserFavorite?: boolean;
  currentUserRating?: number | null;
  downloadCount?: number;
  downloadsCount?: number;
  downloads_count?: number;
  description?: string | null;
  tags?: string[] | null;
};

export type ToneSharingPack = {
  id: string;
  title: string;
  featured?: boolean;
  creatorUserId?: string | null;
  moderationStatus?: string;
  creatorEmail?: string | null;
  creatorDisplayName?: string | null;
  creatorHandle?: string | null;
  creatorProfileHandle?: string | null;
  profileHandle?: string | null;
  creatorBio?: string | null;
  creatorAvatarUrl?: string | null;
  description?: string | null;
  thumbnailUrl?: string | null;
  thumbnailAssetId?: string | null;
};

export type ToneSharingPackDetails = {
  pack: ToneSharingPack;
  items: Array<{ itemId: string; sortOrder: number; title: string; type: string; moderationStatus?: string; description?: string | null; tags?: string[] | null }>;
};

export type ItemArchiveResource = {
  id: string;
  name?: string;
  category?: string;
  type: string;
  fileName: string;
  hash?: string;
};

export type Tone3000ResourceRef = {
  id: string;
  name?: string;
  category?: string;
  type: string;
  toneId?: string;
  modelId?: string;
  modelUrl?: string;
  creatorId?: string;
  creatorName?: string;
};

export type ItemArchive = {
  formatVersion: number;
  preset: Record<string, unknown>;
  resources: ItemArchiveResource[];
  blends?: Array<Record<string, unknown>>;
  tone3000Resources?: Tone3000ResourceRef[];
};

export type ItemCollectionArchive = {
  formatVersion: number;
  createdAt: string;
  presets: Array<Record<string, unknown>>;
  resources: ItemArchiveResource[];
  blends?: Array<Record<string, unknown>>;
  tone3000Resources?: Tone3000ResourceRef[];
};

export type ShareConsentStatus = {
  consentType: string;
  version: number;
  accepted: boolean;
  acceptedAt: string | null;
};

export type ToneSharingRow = {
  id: string;
  slug: string;
  title: string;
  items: Array<{
    id: string;
    kind: "item" | "pack";
    title: string;
    type: string | null;
    featured?: boolean;
    moderationStatus?: string;
    creatorEmail?: string | null;
    creatorDisplayName?: string | null;
    creatorHandle?: string | null;
    creatorProfileHandle?: string | null;
    profileHandle?: string | null;
    creatorBio?: string | null;
    creatorAvatarUrl?: string | null;
    favoriteCount?: number;
    ratingCount?: number;
    averageRating?: number | null;
    currentUserFavorite?: boolean;
    currentUserRating?: number | null;
    downloadCount?: number;
    description?: string | null;
    tags?: string[] | null;
    thumbnailUrl?: string | null;
    thumbnailAssetId?: string | null;
  }>;
};

export type InstalledPackSource = "zipImport" | "toneSharingApi" | "generatedPack";

export type InstalledPackResourceRef = {
  type: string;
  id: string;
};

export type InstalledPackDeletionPlan = {
  removablePresetIds: string[];
  preservedPresetIds: string[];
  missingPresetIds: string[];
  removableResourceEntries: InstalledPackResourceRef[];
  preservedResourceEntries: InstalledPackResourceRef[];
};

export type InstalledPackMetadata = {
  id: string;
  title: string;
  source: InstalledPackSource;
  importedAt: string;
  packId?: string;
  archivePath?: string;
  archiveFileName?: string;
  presetIds: string[];
  presetSignatures?: Record<string, string>;
  resources: InstalledPackResourceRef[];
};

export type AiToneEffect = { type: string; name: string; settings?: Record<string, string | number> };

export type AiToneCombination = {
  name: string;
  description: string;
  amp: string;
  cabinet: string;
  pedals: string[];
  effects: AiToneEffect[];
};

export type AiToneSearchResult = {
  query: { band?: string; song?: string };
  combinations: AiToneCombination[];
  generatedAt: string;
};

export type ToneSharingShareTarget = {
  kind: "item" | "pack";
  id: string;
};

/** Which feed the browse area is showing. */
export type BrowseMode = "featured" | "items" | "packs" | "installed" | "mine" | "ai-search" | "review";

export type BrowseCollectionState = {
  page: number;
  pageSize: number;
  hasMore: boolean;
  loadingMore: boolean;
  items: ToneSharingItem[];
  packs: ToneSharingPack[];
};

export type ToneActionIcon = "preview" | "download" | "share" | "view" | "approve" | "reject" | "edit" | "delete";

export type InstalledToneSharingLookup = {
  itemIds: Set<string>;
  packIds: Set<string>;
};

export type ResizedImageResult = {
  blob: Blob;
  width: number;
  height: number;
};
