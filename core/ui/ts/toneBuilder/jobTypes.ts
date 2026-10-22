/**
 * What the tone builder service sends back: a job's status while it runs, and once it is done the
 * research dossier and the proposal. Types only, mirroring the service's result schema
 * ("soundshed.tone-builder-result/1", "soundshed.tone-proposal/2").
 */

import type { PresetSpec } from "./types.js";

export type ToneBuilderJobStatus = "queued" | "researching" | "selecting" | "proposing" | "done" | "failed";

export interface ToneBrief {
  subject: { artist?: string | null; song?: string | null; album?: string | null; era?: string | null; style?: string | null; freeText?: string | null };
  player?: { instrument?: "guitar" | "bass"; tuning?: string | null; pickups?: "humbucker" | "single" | "any" };
  constraints?: {
    maxPresets?: number;
    maxScenesPerPreset?: number;
    providers?: string[];
    licencePolicy?: "any" | "redistributable-only";
    installedOnly?: boolean;
  };
  client: { app?: string | null; version?: string | null; effectCatalogHash: string };
  /** How hard the model works: its effort and how much it searches. */
  options?: { thinking?: "quick" | "balanced" | "thorough" | "deep" };
}

export type LicencePolicyId = "redistributable" | "redistributable-unmodified" | "noncommercial" | "reference-only";

/** A capture a proposal chose, named the same way whichever provider it comes from. */
export interface ProposalResourceRef {
  ref: string;
  provider: string;
  key: string;
  type: "nam" | "ir";
  sha256: string | null;
  gearId: string | null;
  captureScope: string | null;
  licence: { id: string | null; url: string | null; policy: LicencePolicyId; attribution: string };
  creator: { name: string | null; url: string | null };
  sourceUrl: string | null;
  title: string;
  providerData: Record<string, string>;
}

export interface ProposalFallback {
  fx: string;
  preset: string | null;
  set: Record<string, number | string | boolean>;
}

export type ProposalNodeResource =
  | (ProposalResourceRef & { fallback: ProposalFallback | null })
  | { gearId: string; scope: string | null; fallback: ProposalFallback };

export interface ProposalNode {
  fx: string;
  preset: string | null;
  set: Record<string, number | string | boolean>;
  label: string | null;
  off: boolean;
  resource: ProposalNodeResource | null;
}

export interface ProposalSplit {
  split: ProposalNode[][];
  gains: number[] | null;
  masterLevel: number | null;
  label: string | null;
}

export interface ProposalPreset {
  key: string;
  name: string;
  description: string;
  tags: string[];
  tuning: string | null;
  gearIds: string[];
  scenes: Array<{ key: string; title: string; chain: Array<ProposalNode | ProposalSplit> }>;
}

export interface ProposalCredit {
  ref: string;
  provider: string;
  title: string;
  creator: string | null;
  creatorUrl: string | null;
  sourceUrl: string | null;
  licence: string | null;
  licenceUrl: string | null;
  licencePolicy: LicencePolicyId;
  attribution: string;
}

export interface ToneProposal {
  schema: "soundshed.tone-proposal/2";
  /** Published text, in a tone designer's voice; absent on results from before 2026-10-11. */
  packTitle?: string;
  packDescription?: string;
  /** The research's most cited sources. */
  sources?: Array<{ title: string; url: string }>;
  artworkPrompt?: string;
  effectCatalogHash: string;
  dossierId: string | null;
  presets: ProposalPreset[];
  credits: ProposalCredit[];
  notes: string;
  validation: { ok: boolean; problems: string[]; presets: Array<{ key: string; ok: boolean; problems: string[]; warnings: string[] }> };
  /** The presets as the shared builder takes them, fallbacks in place where no capture was chosen. */
  builderPresets: PresetSpec[];
}

export interface DossierGearItem {
  role: string;
  description: string;
  gearId: string | null;
  settings: string | null;
  usedFor: string | null;
  confidence: "high" | "medium" | "low";
  citations: Array<{ url: string; title: string; quote: string }>;
}

export interface RigDossier {
  summary: string;
  gear: DossierGearItem[];
  signalChain: string[];
  evidenceLimits: string;
  gaps: string[];
}

export interface ToneBuilderJob {
  jobId: string;
  status: ToneBuilderJobStatus;
  progress: Array<{ at: string; status: ToneBuilderJobStatus; message: string }>;
  cachedDossier: boolean;
  usage?: { costUsdEstimate?: number; webSearches?: number };
  /** The pack artwork drawn for this job, once it is done. */
  artwork?: { available: boolean; regenerations: number; remaining: number } | null;
  error: { code: string; message: string } | null;
  /** Where its presets were published on Tone Sharing, as Soundshed; null until they are. */
  published?: { itemIds: string[]; packId: string | null; publishedAt: string } | null;
  /** Present once the job is done. */
  brief?: ToneBrief;
  dossier?: RigDossier;
  proposal?: ToneProposal;
}
