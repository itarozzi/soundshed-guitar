/**
 * Turning a tone builder proposal into presets in the library: fetch the captures it chose that
 * the library does not have yet, build each preset with the shared builder against the live
 * effect catalogue and library, credit the captures in its description, save it, and file the
 * lot in a folder.
 *
 * Captures are fetched through a per-provider fetcher, so a proposal is not tied to where its
 * captures come from; a provider this app cannot fetch from yet is reported, not guessed at.
 */

import { postMessage } from "../bridge.js";
import { cachePresetInMemory } from "../presets/cache.js";
import { assignImportedPresetsToTopLevelFolder } from "../presets/folderArchive.js";
import { sanitizeImportedPreset } from "../presets/sanitize.js";
import type { SavePresetMessage } from "../presets/savePresetMessage.js";
import { cachePreset, putLibraryPresetFirst } from "../presetLibraryStore.js";
import { importRemoteResourceAndWait } from "../remoteImport.js";
import { remoteResourceMetadata } from "../remoteResources.js";
import { getLibraryResource } from "../resourceLibrary.js";
import { downloadTone3000ResourceByReference } from "../tone3000.js";
import type { Preset } from "../types.js";
import { arrayBufferToBase64 } from "../utils.js";
import { readAppBuildInventory } from "./appInventory.js";
import { BuildRegistry, buildPreset } from "./build.js";
import { balancePresetSpec } from "./levels.js";
import { presetDescription } from "./text.js";
import type { SceneBalance, SceneMeasure } from "./levels.js";
import type { PresetCollection, PresetSpec } from "./types.js";
import type { ProposalFallback, ProposalNode, ProposalResourceRef, ProposalSplit, ToneProposal } from "./jobTypes.js";

/** Fetches a capture's file from its provider. */
type CaptureFetcher = (ref: ProposalResourceRef) => Promise<{ data: ArrayBuffer; metadata: Record<string, string> }>;

const FETCHERS: Record<string, CaptureFetcher> = {
  tone3000: async (ref) => {
    const modelId = ref.providerData.modelId ?? ref.key;
    const data = await downloadTone3000ResourceByReference({ toneId: ref.providerData.toneId, modelId, modelUrl: ref.providerData.modelUrl });
    // The tone and model ids let archives and installs find this file again.
    return { data, metadata: { toneId: ref.providerData.toneId ?? "", modelId, authorUsername: ref.creator.name ?? "" } };
  },
};

export function canFetchFrom(provider: string): boolean {
  return provider in FETCHERS;
}

function isSplit(item: ProposalNode | ProposalSplit): item is ProposalSplit {
  return Array.isArray((item as ProposalSplit).split);
}

function isCaptureRef(resource: ProposalNode["resource"]): resource is ProposalResourceRef & { fallback: ProposalFallback | null } {
  return Boolean(resource && "ref" in resource && typeof resource.ref === "string");
}

/** Every capture the proposal's presets use, once each, in order of first use. */
export function collectProposalCaptures(proposal: ToneProposal): ProposalResourceRef[] {
  const found = new Map<string, ProposalResourceRef>();
  const visit = (items: Array<ProposalNode | ProposalSplit>): void => {
    for (const item of items) {
      if (isSplit(item)) {
        item.split.forEach(visit);
      } else if (isCaptureRef(item.resource) && !found.has(item.resource.ref)) {
        found.set(item.resource.ref, item.resource);
      }
    }
  };
  proposal.presets.forEach((preset) => preset.scenes.forEach((scene) => visit(scene.chain)));
  return [...found.values()];
}

export function isCaptureInstalled(ref: ProposalResourceRef): boolean {
  const existing = getLibraryResource(ref.type, ref.ref);
  return Boolean(existing && !existing.fileMissing);
}

/** The captures still to download, and those whose provider this app cannot fetch from. */
export function planCaptureDownloads(proposal: ToneProposal): { toFetch: ProposalResourceRef[]; unfetchable: ProposalResourceRef[] } {
  const missing = collectProposalCaptures(proposal).filter((ref) => !isCaptureInstalled(ref));
  return {
    toFetch: missing.filter((ref) => canFetchFrom(ref.provider)),
    unfetchable: missing.filter((ref) => !canFetchFrom(ref.provider)),
  };
}

/** Longest title part of a capture's file name; the key keeps it unique. */
const FILE_TITLE_MAX = 60;

/**
 * A short, safe file name for a capture: the start of its title and its provider key. Provider
 * titles run to 170 characters, which under the settings folder passes Windows' 260-character
 * path limit and the write fails.
 */
export function captureFileName(ref: Pick<ProposalResourceRef, "title" | "key">, extension: string): string {
  const slug = (text: string): string => text.replace(/[^a-z0-9-_]+/gi, "-").replace(/-{2,}/g, "-").replace(/^-+|-+$/g, "");
  const title = slug(ref.title).slice(0, FILE_TITLE_MAX).replace(/-+$/, "") || "capture";
  const key = slug(ref.key).slice(0, 24);
  return `${title}${key ? `-${key}` : ""}${extension}`;
}

async function fetchCapture(ref: ProposalResourceRef): Promise<void> {
  const fetcher = FETCHERS[ref.provider];
  if (!fetcher) {
    throw new Error(`Captures from ${ref.provider} cannot be downloaded by this version of the app`);
  }
  const { data, metadata } = await fetcher(ref);
  const extension = ref.type === "ir" ? ".wav" : ".nam";
  await importRemoteResourceAndWait({
    provider: ref.provider,
    resourceType: ref.type,
    resourceId: ref.ref,
    name: ref.title,
    description: ref.licence.attribution,
    category: ref.type === "ir" ? "cab" : ref.captureScope === "pedal" ? "pedal" : "amp",
    // The engine files it under resources/content/<provider>/ already.
    subfolder: "tone-builder",
    fileName: captureFileName(ref, extension),
    metadata: {
      ...metadata,
      ...remoteResourceMetadata({
        provider: ref.provider,
        providerKey: ref.key,
        licence: ref.licence.id,
        licenceUrl: ref.licence.url,
        creatorName: ref.creator.name,
        creatorUrl: ref.creator.url,
        sourceUrl: ref.sourceUrl,
        captureScope: ref.captureScope,
      }),
      ...(ref.gearId ? { gearId: ref.gearId } : {}),
      ...(ref.sha256 ? { sha256: ref.sha256 } : {}),
    },
    data: arrayBufferToBase64(data),
  }, "tone-builder-import");
}

/** Downloads the captures in turn, reporting each; stops at the first failure. */
export async function downloadProposalCaptures(refs: ProposalResourceRef[], onProgress: (done: number, total: number, ref: ProposalResourceRef) => void): Promise<void> {
  for (let i = 0; i < refs.length; i += 1) {
    onProgress(i, refs.length, refs[i]);
    await fetchCapture(refs[i]);
  }
  if (refs.length) onProgress(refs.length, refs.length, refs[refs.length - 1]);
}

export interface BuiltToneBuilderPresets {
  presets: Preset[];
  folderName: string;
}

/**
 * Builds and saves every preset of the proposal, filed under `folderName`. Every capture must be
 * in the library first (downloadProposalCaptures); the builder refuses one that is not.
 */
export function proposalCollection(jobId: string): PresetCollection {
  return { idScope: `soundshed-tone-builder/${jobId}`, category: "Tone Builder", tags: ["tone-builder"] };
}

/**
 * Balances every preset's scenes to the packs' level (levels.ts), measuring each in the engine.
 * Every capture must be in the library first. Returns the corrected specs to build and save.
 */
export async function balanceProposal(
  jobId: string,
  proposal: ToneProposal,
  measure: SceneMeasure,
  onProgress: (presetName: string) => void,
): Promise<{ specs: PresetSpec[]; report: SceneBalance[] }> {
  const registry = new BuildRegistry(readAppBuildInventory());
  const specs: PresetSpec[] = [];
  const report: SceneBalance[] = [];
  for (const spec of proposal.builderPresets) {
    onProgress(spec.name);
    const balanced = await balancePresetSpec(proposalCollection(jobId), spec, registry, measure);
    specs.push(balanced.spec);
    report.push(...balanced.report);
  }
  return { specs, report };
}

export function buildAndSaveProposal(jobId: string, proposal: ToneProposal, folderName: string, specs: PresetSpec[] = proposal.builderPresets): BuiltToneBuilderPresets {
  const registry = new BuildRegistry(readAppBuildInventory());
  const collection = proposalCollection(jobId);
  const presets = specs.map((spec) => {
    const { preset } = buildPreset(collection, spec, registry);
    const description = presetDescription(proposal, spec.key, preset.description);
    return sanitizeImportedPreset({ ...(preset as unknown as Preset), description });
  });

  for (const preset of presets) {
    postMessage({
      type: "savePreset",
      saveMode: "import",
      presetId: preset.id,
      name: preset.name,
      category: preset.category,
      description: preset.description,
      includeGlobalSignalChain: false,
      preset,
    } satisfies SavePresetMessage);
    cachePresetInMemory(preset);
    putLibraryPresetFirst(preset);
    cachePreset(preset, preset.id);
  }
  assignImportedPresetsToTopLevelFolder(folderName, presets.map((preset) => preset.id));
  return { presets, folderName };
}
