import { afterEach, describe, expect, it } from "vitest";
import { onResourceImported, onResourceImportFailed } from "../ts/messages/resourceHandlers.js";
import { Tone3000Navigator } from "../ts/resourceBrowser/tone3000Navigation.js";
import { uiState } from "../ts/state.js";
import type { Tone3000Tone } from "../ts/tone3000ApiTypes.js";

type ImportMessage = { type: string; requestId: string; resourceType: string; resourceId: string; data: string };

function makeNavigator(): Tone3000Navigator {
  return new Tone3000Navigator({
    getOptions: () => null,
    getSelectedArchitecture: () => null,
    getContextKey: () => "test",
    rememberNavigationView: () => {},
    getNavigationView: () => undefined,
    getTones: () => [],
    getModelsCache: () => new Map(),
  });
}

const tone: Tone3000Tone = { id: 42, title: "Test tone", gear: "amp" };
const bytes = new Uint8Array([1, 2, 3]).buffer;
const originalResourceLibrary = uiState.resourceLibrary;

describe("Tone3000 resource import", () => {
  afterEach(() => {
    delete window.IPlugSendMsg;
    uiState.resourceLibrary = originalResourceLibrary;
  });

  it.each(["nam", "ir"] as const)("sends %s through the native bridge and waits for its import acknowledgment", async (resourceType) => {
    const sent: ImportMessage[] = [];
    window.IPlugSendMsg = (message: string) => sent.push(JSON.parse(message) as ImportMessage);

    let settled = false;
    const importing = makeNavigator().importTone3000Resource(tone, "7", "Model", "2", bytes, false, resourceType)
      .then((id) => { settled = true; return id; });

    expect(sent).toHaveLength(1);
    expect(sent[0]).toMatchObject({ type: "importRemoteResource", resourceType, resourceId: "tone3000:7", data: "AQID" });
    expect(sent[0].requestId).toMatch(/^tone3000-import-/);
    await Promise.resolve();
    expect(settled).toBe(false);

    onResourceImported({
      type: "resourceImported",
      requestId: sent[0].requestId,
      resourceType,
      id: sent[0].resourceId,
      name: "Model",
      filePath: `C:/test/Model.${resourceType === "ir" ? "wav" : "nam"}`,
    });
    expect(await importing).toBe("tone3000:7");
  });

  it("rejects a failed host import instead of selecting an absent resource", async () => {
    let sent: ImportMessage | undefined;
    window.IPlugSendMsg = (message: string) => { sent = JSON.parse(message) as ImportMessage; };

    const importing = makeNavigator().importTone3000Resource(tone, "8", "Broken", "2", bytes, false, "nam");
    expect(sent?.requestId).toBeTruthy();
    onResourceImportFailed({
      type: "resourceImportFailed",
      requestId: sent?.requestId,
      message: "Import failed",
      detail: "Failed to write file",
    });

    await expect(importing).rejects.toThrow("Failed to write file");
  });
});
