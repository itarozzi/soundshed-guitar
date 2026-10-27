/**
 * Installing a Tone Sharing download goes through the engine (toneSharingPanel/install.ts):
 * the UI fetches the remote models the archives name, sends the archives whole with
 * installPresetArchives, and acts on the answer. Removing one is deleteInstalledPresetArchive.
 */
import JSZip from "jszip";
import { beforeEach, describe, expect, it, vi } from "vitest";

const showAlert = vi.fn(async () => undefined);
const showConfirm = vi.fn(async () => true);
vi.mock("../ts/notifications.js", () => ({ clearNotification: vi.fn(), showNotification: vi.fn() }));
vi.mock("../ts/dialogs.js", async (importOriginal) => ({ ...(await importOriginal<Record<string, unknown>>()), showAlert, showConfirm }));

const sent: Array<Record<string, unknown>> = [];
window.IPlugSendMsg = (message: string) => sent.push(JSON.parse(message) as Record<string, unknown>);
(window as unknown as { JSZip: typeof JSZip }).JSZip = JSZip;

const { configureToneSharingHostActions } = await import("../ts/toneSharingPanel/hostActions.js");
const { readArchiveRemoteRefs } = await import("../ts/presets/remoteArchiveResources.js");
const { uiState } = await import("../ts/state.js");
const install = await import("../ts/toneSharingPanel/install.js");

const importArchiveRemoteResources = vi.fn(async (refs: Array<{ id: string; type: string }>, _idMap: Map<string, string>, imported: Array<{ type: string; id: string }>) => {
  // The first is fetched; the rest cannot be had.
  imported.push({ type: refs[0].type, id: refs[0].id });
  return refs.slice(1).map((ref) => ref.id);
});

configureToneSharingHostActions({
  buildImportSummaryMessage: () => "Import?",
  buildToneSharingPresetArchiveBlobs: vi.fn(),
  importArchiveRemoteResources,
  importPresetArchive: vi.fn(),
  populatePresetDropdown: vi.fn(),
  readArchiveRemoteRefs,
  renderActivePreset: vi.fn(),
  switchMainPanel: vi.fn(),
} as never);

async function zipOf(root: Record<string, unknown>): Promise<ArrayBuffer> {
  const zip = new JSZip();
  zip.file("preset.json", JSON.stringify(root));
  return (await zip.generateAsync({ type: "uint8array" })).buffer as ArrayBuffer;
}

function sentOfType(type: string): Record<string, unknown> | undefined {
  return sent.find((message) => message.type === type);
}

beforeEach(() => {
  sent.length = 0;
  showAlert.mockClear();
  showConfirm.mockClear();
  importArchiveRemoteResources.mockClear();
  uiState.activePresetId = null;
});

describe("installing through the engine", () => {
  it("fetches the archive's remote models, sends the archives whole and loads the last preset", async () => {
    const data = await zipOf({
      formatVersion: 1,
      preset: { id: "p", name: "P" },
      resources: [],
      remoteResources: [
        { id: "tone3000:1", type: "nam", name: "Fetched", provider: "tone3000", providerKey: "1" },
        { id: "tone3000:2", type: "nam", name: "Gone", provider: "tone3000", providerKey: "2" },
      ],
    });

    const done = install.installDownloadedArchives({
      entry: { id: "tone-sharing-api:item:x", title: "Crunch", source: "toneSharingApi", archiveFileName: "x.zip" },
      downloads: [{ title: "Crunch", data }],
    });

    await vi.waitFor(() => expect(sentOfType("installPresetArchives")).toBeDefined());
    const request = sentOfType("installPresetArchives")!;
    expect(request.entry).toEqual({ id: "tone-sharing-api:item:x", title: "Crunch", source: "toneSharingApi", archiveFileName: "x.zip" });
    expect(request.folder).toBeUndefined();
    expect(request.allowMissingRemote).toBe(true);
    expect(request.fetchedResources).toEqual([{ type: "nam", id: "tone3000:1" }]);
    const archives = request.archives as Array<{ title: string; data: string }>;
    expect(archives).toHaveLength(1);
    expect(archives[0].title).toBe("Crunch");
    expect(Buffer.from(archives[0].data, "base64").equals(Buffer.from(data))).toBe(true);

    install.onPresetArchivesInstalled({ type: "presetArchivesInstalled", requestId: request.requestId, presetIds: ["user-1"] });
    await expect(done).resolves.toEqual(["user-1"]);
    expect(sentOfType("loadPreset")).toMatchObject({ presetId: "user-1" });
    expect(uiState.activePresetId).toBe("user-1");
    expect(showAlert).toHaveBeenCalledWith(expect.stringContaining("tone3000:2"), "Some models are missing");
  });

  it("rejects with the engine's reason", async () => {
    const done = install.installDownloadedArchives({
      entry: { id: "tone-sharing-api:item:bad", title: "Bad", source: "toneSharingApi" },
      downloads: [{ title: "Bad", data: await zipOf({ formatVersion: 1, preset: { id: "p" } }) }],
    });
    await vi.waitFor(() => expect(sentOfType("installPresetArchives")).toBeDefined());
    install.onPresetArchivesInstallFailed({ requestId: sentOfType("installPresetArchives")!.requestId, message: "Install failed", detail: "Could not save" });
    await expect(done).rejects.toThrow("Could not save");
    expect(sentOfType("loadPreset")).toBeUndefined();
  });

  it("sends a bare preset download as a one-preset archive", async () => {
    const bare = new TextEncoder().encode(JSON.stringify({ id: "p", name: "Bare" })).buffer as ArrayBuffer;
    const done = install.installDownloadedArchives({
      entry: { id: "tone-sharing-api:pack-1", title: "Pack", source: "toneSharingApi", packId: "pack-1" },
      folder: "Pack",
      downloads: [{ title: "Bare", data: bare }],
    });
    await vi.waitFor(() => expect(sentOfType("installPresetArchives")).toBeDefined());
    const request = sentOfType("installPresetArchives")!;
    expect(request.folder).toBe("Pack");
    const zip = await JSZip.loadAsync(Buffer.from((request.archives as Array<{ data: string }>)[0].data, "base64"));
    const root = JSON.parse(await zip.file("preset.json")!.async("text")) as { preset: { name: string } };
    expect(root.preset.name).toBe("Bare");
    install.onPresetArchivesInstalled({ requestId: request.requestId, presetIds: ["user-2"] });
    await done;
  });

  it("installs nothing when the pack is not confirmed", async () => {
    showConfirm.mockResolvedValueOnce(false);
    const result = await install.installDownloadedArchives({
      entry: { id: "tone-sharing-api:pack-2", title: "Pack", source: "toneSharingApi", packId: "pack-2" },
      folder: "Pack",
      downloads: [{ title: "One", data: await zipOf({ formatVersion: 1, preset: { id: "p" } }) }],
      confirm: true,
    });
    expect(result).toBeNull();
    expect(sentOfType("installPresetArchives")).toBeUndefined();
  });
});

describe("removing through the engine", () => {
  it("asks the engine and resolves with the presets it removed", async () => {
    const done = install.requestInstalledPackRemoval("tone-sharing-api:pack-1");
    expect(sentOfType("deleteInstalledPresetArchive")).toEqual({ type: "deleteInstalledPresetArchive", id: "tone-sharing-api:pack-1" });
    install.onInstalledPresetArchiveDeleted({ id: "tone-sharing-api:pack-1", presetIds: ["user-1", "user-2"] });
    await expect(done).resolves.toEqual(["user-1", "user-2"]);
  });
});
