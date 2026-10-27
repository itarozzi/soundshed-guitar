/**
 * Exporting the library as a portable archive: the engine builds it (exportLibraryArchive),
 * with only the models and IRs the presets use or the whole library, and saves it.
 */

import { postMessage } from "../bridge.js";
import { libraryExportButton, libraryExportResourcesSelect } from "./dom.js";

export function initLibraryExport(): void {
  if (!libraryExportButton) {
    return;
  }

  if ((libraryExportButton as HTMLButtonElement).dataset.bound === "true") {
    return;
  }

  (libraryExportButton as HTMLButtonElement).dataset.bound = "true";
  libraryExportButton.addEventListener("click", () => exportLibraryArchive());
}

export function exportLibraryArchive(): void {
  postMessage({
    type: "exportLibraryArchive",
    resources: libraryExportResourcesSelect?.value === "all" ? "all" : "used",
  });
}
