/**
 * Talking to the tone builder service: making sure it holds this app's effect catalogue, starting
 * a job from a brief, and following the job until it is done.
 *
 * The service validates proposals against the exact effects this app has, named by a hash of
 * their canonical JSON (keys sorted at every level, no whitespace); it is uploaded once per hash.
 */

import { apiFetch, toneSharingFetch } from "../toneSharingPanel/api.js";
import type { ToneBrief, ToneBuilderJob } from "./jobTypes.js";
import type { InventoryEffect } from "./types.js";

const POLL_INTERVAL_MS = 3000;
const JOB_TIMEOUT_MS = 15 * 60 * 1000;

function sortKeys(value: unknown): unknown {
  if (Array.isArray(value)) return value.map(sortKeys);
  if (value && typeof value === "object") {
    const source = value as Record<string, unknown>;
    const sorted: Record<string, unknown> = {};
    for (const key of Object.keys(source).sort()) {
      if (source[key] !== undefined) sorted[key] = sortKeys(source[key]);
    }
    return sorted;
  }
  return value;
}

export function canonicalJson(value: unknown): string {
  return JSON.stringify(sortKeys(value));
}

export async function sha256Hex(text: string): Promise<string> {
  const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(text));
  return Array.from(new Uint8Array(digest), (byte) => byte.toString(16).padStart(2, "0")).join("");
}

/** Uploads the effect catalogue unless the service already has it; returns its hash. */
export async function ensureEffectCatalogue(effects: InventoryEffect[]): Promise<string> {
  const hash = await sha256Hex(canonicalJson(effects));
  const existing = await toneSharingFetch(`/tone-builder/catalogues/${hash}`, { method: "HEAD" });
  if (existing.ok) {
    return hash;
  }
  if (existing.status !== 404) {
    throw new Error(`Could not check the effect catalogue (${existing.status})`);
  }
  await apiFetch(`/tone-builder/catalogues/${hash}`, {
    method: "PUT",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ effects }),
  });
  return hash;
}

export async function startToneBuilderJob(brief: ToneBrief): Promise<{ jobId: string; cachedDossier: boolean }> {
  return apiFetch<{ jobId: string; cachedDossier: boolean }>("/tone-builder/jobs", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(brief),
  });
}

export async function getToneBuilderJob(jobId: string): Promise<ToneBuilderJob> {
  return apiFetch<ToneBuilderJob>(`/tone-builder/jobs/${encodeURIComponent(jobId)}`);
}

/**
 * Polls the job until it is done or failed, reporting each poll. Stops early, with an error,
 * when `signal` aborts.
 */
export async function waitForToneBuilderJob(
  jobId: string,
  onUpdate: (job: ToneBuilderJob) => void,
  signal?: AbortSignal,
  sleep: (ms: number) => Promise<void> = (ms) => new Promise((resolve) => setTimeout(resolve, ms)),
): Promise<ToneBuilderJob> {
  const started = Date.now();
  for (;;) {
    if (signal?.aborted) throw new Error("Cancelled");
    const job = await getToneBuilderJob(jobId);
    onUpdate(job);
    if (job.status === "done" || job.status === "failed") return job;
    if (Date.now() - started > JOB_TIMEOUT_MS) throw new Error("The tone builder is taking too long; try again later");
    await sleep(POLL_INTERVAL_MS);
  }
}
