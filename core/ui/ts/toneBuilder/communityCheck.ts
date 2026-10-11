/**
 * Before paying to research a tone, a free look at Tone Sharing: presets and packs whose title or
 * description names the artist or the song. Something may already be there — shared by another
 * player, or by an earlier Tone Builder run once approved.
 *
 * The API matches a query as one phrase, so the artist and the song are asked for separately and
 * the results merged; a result naming both ranks first.
 */

import { apiFetch } from "../toneSharingPanel/api.js";
import { buildInstalledToneSharingLookup } from "../toneSharingPanel/presetLinks.js";
import type { ToneSharingItem, ToneSharingPack } from "../toneSharingPanel/types.js";

export interface CommunityMatch {
  kind: "item" | "pack";
  id: string;
  title: string;
  description: string;
  creator: string | null;
  installed: boolean;
  /** How many of the asked-for terms (artist, song) the title or description names. */
  score: number;
}

const PAGE_SIZE = 20;
const MAX_MATCHES = 8;

function normalise(text: string | null | undefined): string {
  return (text ?? "").toLowerCase().replace(/\s+/g, " ").trim();
}

/** Presets and packs on Tone Sharing that name the artist or song, best matches first. */
export async function findCommunityMatches(artist: string, song: string): Promise<CommunityMatch[]> {
  const terms = [...new Set([artist, song].map((term) => term.trim()).filter((term) => term.length >= 2))];
  if (!terms.length) return [];

  const requests = terms.flatMap((term) => {
    const q = encodeURIComponent(term);
    return [
      apiFetch<{ items: ToneSharingItem[] }>(`/items?page=1&pageSize=${PAGE_SIZE}&q=${q}`)
        .then((r) => (r.items ?? []).map((entry) => ({ kind: "item" as const, entry: entry as ToneSharingItem | ToneSharingPack })))
        .catch(() => []),
      apiFetch<{ packs: ToneSharingPack[] }>(`/packs?page=1&pageSize=${PAGE_SIZE}&q=${q}`)
        .then((r) => (r.packs ?? []).map((entry) => ({ kind: "pack" as const, entry: entry as ToneSharingItem | ToneSharingPack })))
        .catch(() => []),
    ];
  });
  // One failed lookup should not stop the others; a check that finds nothing just lets the build go on.
  const results = await Promise.all(requests);
  const installed = buildInstalledToneSharingLookup();
  const matches = new Map<string, CommunityMatch>();
  for (const result of results) {
    for (const { kind, entry } of result) {
      const key = `${kind}:${entry.id}`;
      if (matches.has(key)) continue;
      const haystack = normalise(`${entry.title} ${entry.description ?? ""}`);
      const score = terms.filter((term) => haystack.includes(normalise(term))).length;
      if (!score) continue;
      matches.set(key, {
        kind,
        id: entry.id,
        title: entry.title,
        description: entry.description ?? "",
        creator: entry.creatorDisplayName ?? entry.creatorHandle ?? null,
        installed: kind === "item" ? installed.itemIds.has(entry.id) : installed.packIds.has(entry.id),
        score,
      });
    }
  }
  return [...matches.values()]
    .sort((a, b) => b.score - a.score || (a.kind === b.kind ? 0 : a.kind === "pack" ? -1 : 1))
    .slice(0, MAX_MATCHES);
}
