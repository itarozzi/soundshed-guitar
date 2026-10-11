/**
 * The text a Tone Builder preset and pack carry, saved locally and published on Tone Sharing.
 *
 * It reads as a tone designer's notes: the proposal's own description of the sound and how it was
 * made, then who made the captures it plays and the sources the research drew on. Nothing about
 * licences, providers or how the presets were produced; the captures' licences travel with the
 * resources themselves.
 */

import type { ProposalCredit, ProposalNode, ProposalPreset, ProposalSplit, ToneProposal } from "./jobTypes.js";

const MAX_SOURCES = 3;

function hostOf(url: string): string {
  try {
    return new URL(url).hostname.replace(/^www\./, "");
  } catch {
    return "";
  }
}

/** "Guitar Player interview (guitarplayer.com); Fender history (fender.com)". */
export function sourcesLine(proposal: ToneProposal): string {
  const sources = (proposal.sources ?? []).slice(0, MAX_SOURCES).map((source) => {
    const host = hostOf(source.url);
    const title = source.title.trim();
    if (!title) return host;
    return host && !title.toLowerCase().includes(host) ? `${title} (${host})` : title;
  }).filter(Boolean);
  return sources.length ? `Sources: ${sources.join("; ")}.` : "";
}

function isSplit(item: ProposalNode | ProposalSplit): item is ProposalSplit {
  return Array.isArray((item as ProposalSplit).split);
}

function refsIn(preset: ProposalPreset): Set<string> {
  const refs = new Set<string>();
  const visit = (items: Array<ProposalNode | ProposalSplit>): void => {
    for (const item of items) {
      if (isSplit(item)) item.split.forEach(visit);
      else if (item.resource && "ref" in item.resource && item.resource.ref) refs.add(item.resource.ref);
    }
  };
  preset.scenes.forEach((scene) => visit(scene.chain));
  return refs;
}

/** "Captures: JCM800 2203 by arthm; V30 4x12 by Ann." for the captures this preset plays. */
export function capturesLine(credits: ProposalCredit[], preset: ProposalPreset | undefined): string {
  if (!preset) return "";
  const refs = refsIn(preset);
  const used = credits.filter((credit) => refs.has(credit.ref));
  return used.length
    ? `Captures: ${used.map((credit) => `${credit.title.trim()}${credit.creator ? ` by ${credit.creator}` : ""}`).join("; ")}.`
    : "";
}

function joinParagraphs(...parts: string[]): string {
  return parts.map((part) => part.trim()).filter(Boolean).join("\n\n");
}

/** A preset's description: the sound and the rig, then its captures' makers and the sources. */
export function presetDescription(proposal: ToneProposal, key: string, builtDescription: string): string {
  const preset = proposal.presets.find((p) => p.key === key);
  return joinParagraphs(preset?.description || builtDescription, capturesLine(proposal.credits, preset), sourcesLine(proposal));
}

/** The pack's title and description, falling back to the research summary for older results. */
export function packText(proposal: ToneProposal, fallbackTitle: string, summary: string): { title: string; description: string } {
  return {
    title: proposal.packTitle?.trim() || fallbackTitle,
    description: joinParagraphs(proposal.packDescription?.trim() || summary, sourcesLine(proposal)),
  };
}
