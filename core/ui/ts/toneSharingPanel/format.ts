/**
 * Pure presentation helpers: reading loosely-typed API fields into something
 * displayable, and formatting counts, badges and creator identities.
 *
 * Nothing here touches the DOM or the network, which is what makes it safe for
 * every renderer in the feature to depend on.
 */

import { escapeHtml } from "../utils.js";
import { buildApiUrl } from "./api.js";

export function normalizeCommunityPresetSearchValue(value: unknown): string {
  if (typeof value !== "string") {
    return "";
  }
  return value.trim().toLowerCase();
}

export function getToneSharingDisplayTags(tags: string[] | null | undefined): string[] {
  return Array.isArray(tags)
    ? tags.filter((tag) => normalizeCommunityPresetSearchValue(tag) !== "preset")
    : [];
}

export function resolveDownloadCountValue(value: unknown): number | undefined {
  if (typeof value !== "number" || !Number.isFinite(value) || value < 0) {
    return undefined;
  }
  return Math.floor(value);
}

export function resolveToneSharingDownloadCount(item: Record<string, unknown>): number | undefined {
  const directCount = resolveDownloadCountValue(item.downloadCount);
  if (typeof directCount === "number") {
    return directCount;
  }
  const pluralCount = resolveDownloadCountValue(item.downloadsCount);
  if (typeof pluralCount === "number") {
    return pluralCount;
  }
  return resolveDownloadCountValue(item.downloads_count);
}

export function normalizeCreatorHandle(raw: unknown): string | null {
  if (typeof raw !== "string") {
    return null;
  }

  const trimmed = raw.trim();
  if (!trimmed) {
    return null;
  }

  const withoutPrefix = trimmed.replace(/^@+/, "");
  if (!withoutPrefix || /\s/.test(withoutPrefix)) {
    return null;
  }

  if (!/^[A-Za-z0-9._-]{2,64}$/.test(withoutPrefix)) {
    return null;
  }

  return `@${withoutPrefix}`;
}

export function resolveCreatorProfileHandle(item: Record<string, unknown>): string | null {
  const explicit = [
    item.creatorProfileHandle,
    item.creatorHandle,
    item.profileHandle,
    item.handle,
  ];

  for (const candidate of explicit) {
    const handle = normalizeCreatorHandle(candidate);
    if (handle) {
      return handle;
    }
  }

  const displayName = typeof item.creatorDisplayName === "string" ? item.creatorDisplayName.trim() : "";
  if (displayName.startsWith("@")) {
    return normalizeCreatorHandle(displayName);
  }

  return null;
}

export function resolveCreatorAvatarUrl(item: Record<string, unknown>): string | null {
  const explicit = [
    item.creatorAvatarUrl,
    item.avatarUrl,
  ];

  for (const candidate of explicit) {
    if (typeof candidate !== "string") {
      continue;
    }
    const trimmed = candidate.trim();
    if (!trimmed) {
      continue;
    }
    return buildApiUrl(trimmed);
  }

  return null;
}

export function formatCompactMetric(value: number | null | undefined): string {
  if (typeof value !== "number" || !Number.isFinite(value) || value < 0) {
    return "0";
  }

  const whole = Math.floor(value);
  if (whole >= 1_000_000) {
    const scaled = whole / 1_000_000;
    const formatted = scaled >= 10 ? scaled.toFixed(0) : scaled.toFixed(1);
    return `${formatted.replace(/\.0$/, "")}M`;
  }
  if (whole >= 1_000) {
    const scaled = whole / 1_000;
    const formatted = scaled >= 10 ? scaled.toFixed(0) : scaled.toFixed(1);
    return `${formatted.replace(/\.0$/, "")}K`;
  }
  return String(whole);
}

export function buildModerationBadge(status: string | undefined): string {
  if (typeof status !== "string") {
    return "";
  }
  const normalized = status.trim().toLowerCase().replace(/\s+/g, "_");
  if (normalized === "approved") {
    return "";
  }
  const className = normalized.replace(/[^a-z0-9_-]/g, "");
  if (!className) {
    return "";
  }
  const label = normalized.replace(/_/g, " ");
  return `<span class="tone-sharing-moderation-badge tone-sharing-moderation-badge--${escapeHtml(className)}">${escapeHtml(label)}</span>`;
}
