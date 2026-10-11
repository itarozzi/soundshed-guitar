/**
 * Importing a downloaded model or IR into the library and waiting until the engine has it.
 *
 * `importRemoteResource` is answered by "resourceImported" or "resourceImportFailed" carrying the
 * request's id (messages.ts re-dispatches both as document events), so a caller can go on to use
 * the resource — select it in a node, build a preset on it — once it is really there.
 */

import { postMessage } from "./bridge.js";

const IMPORT_TIMEOUT_MS = 120_000;
let nextImportRequestId = 0;

/** Posts `importRemoteResource` with this payload and resolves once the engine has stored it. */
export function importRemoteResourceAndWait(payload: Record<string, unknown>, requestPrefix = "remote-import"): Promise<void> {
  const requestId = `${requestPrefix}-${++nextImportRequestId}`;
  return new Promise((resolve, reject) => {
    const cleanup = (): void => {
      clearTimeout(timeout);
      document.removeEventListener("resource-browser:resource-imported", onImported);
      document.removeEventListener("resource-browser:resource-import-failed", onFailed);
    };
    const onImported = (event: Event): void => {
      const detail = (event as CustomEvent<{ requestId?: string }>).detail;
      if (detail?.requestId !== requestId) return;
      cleanup();
      resolve();
    };
    const onFailed = (event: Event): void => {
      const detail = (event as CustomEvent<{ requestId?: string; message?: string }>).detail;
      if (detail?.requestId !== requestId) return;
      cleanup();
      reject(new Error(detail.message || "Resource import failed"));
    };
    const timeout = setTimeout(() => {
      cleanup();
      reject(new Error("Timed out waiting for resource import"));
    }, IMPORT_TIMEOUT_MS);
    document.addEventListener("resource-browser:resource-imported", onImported);
    document.addEventListener("resource-browser:resource-import-failed", onFailed);
    try {
      postMessage({ ...payload, type: "importRemoteResource", requestId });
    } catch (error) {
      cleanup();
      reject(error instanceof Error ? error : new Error(String(error)));
    }
  });
}
