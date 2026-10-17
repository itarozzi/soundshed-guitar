/**
 * The Tone3000 details dialog loads a tone's models by walking every page, which can be slow. A
 * list that arrived after the dialog had moved on to another tone used to replace that tone's
 * list, so Import took the first tone's models under the second tone's name.
 */
// @vitest-environment jsdom
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { describe, expect, it, vi } from "vitest";
import type { Tone3000Model, Tone3000Tone } from "../ts/tone3000ApiTypes";

vi.mock("../ts/notifications.js", () => ({ showNotification: vi.fn() }));

const { Tone3000DetailsView } = await import("../ts/tone3000DetailsView.js");

document.body.innerHTML = readFileSync(join(__dirname, "..", "ui-components", "modals", "tone3000-details-modal.html"), "utf8");

const tone = (id: number, title: string): Tone3000Tone => ({ id, title }) as unknown as Tone3000Tone;
const model = (id: number, name: string): Tone3000Model => ({ id, name }) as unknown as Tone3000Model;

describe("Tone3000DetailsView", () => {
  it("keeps a slow model list for an earlier tone off the tone now shown", async () => {
    let finishA: (models: Tone3000Model[]) => void = () => undefined;
    const fetchModels = vi.fn((which: Tone3000Tone) =>
      which.id === 1
        ? new Promise<Tone3000Model[]>((resolve) => { finishA = resolve; })
        : Promise.resolve([model(20, "B model")]));
    const onImport = vi.fn(async () => undefined);
    const view = new Tone3000DetailsView({ fetchModels, onImport });
    const options = { allowMultipleSelection: true, allowBlendCreation: false };

    view.open(tone(1, "Tone A"), options);
    view.close();
    view.open(tone(2, "Tone B"), options);
    await vi.waitFor(() => expect(document.getElementById("tone3000-details-models")?.textContent).toContain("B model"));

    finishA([model(10, "A model one"), model(11, "A model two")]);
    await new Promise((resolve) => setTimeout(resolve, 0));
    expect(document.getElementById("tone3000-details-models")?.textContent).not.toContain("A model");

    (document.getElementById("tone3000-details-import-selected") as HTMLButtonElement).click();
    await vi.waitFor(() => expect(onImport).toHaveBeenCalled());
    const [importedTone, importedModels] = onImport.mock.calls[0] as unknown as [Tone3000Tone, Tone3000Model[]];
    expect(importedTone.id).toBe(2);
    expect(importedModels.map((m) => m.id)).toEqual([20]);
  });
});
