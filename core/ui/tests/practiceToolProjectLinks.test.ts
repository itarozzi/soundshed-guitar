/**
 * The Practice Tool's Links section, on the real Jam panel markup: it follows the project
 * bar's selection, adds and removes links on the saved project straight away, and draws
 * each link as a plain http(s) anchor for main.ts's click handler to open in the browser.
 */
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { beforeEach, describe, expect, it, vi } from "vitest";
import type { PracticeToolState } from "../ts/types.js";

vi.mock("../ts/notifications.js", () => ({ clearNotification: vi.fn(), showNotification: vi.fn() }));
vi.mock("../ts/dialogs.js", () => ({ showConfirm: vi.fn(async () => true) }));

document.body.innerHTML = readFileSync(join(__dirname, "..", "ui-components", "panels", "jam-panel.html"), "utf8");
window.IPlugSendMsg = () => {};

const { uiState } = await import("../ts/state.js");
const { createDefaultPracticeToolEq } = await import("../ts/practiceTool/eq.js");
const { capturePracticeToolProject, findPracticeToolProject, storePracticeToolProject } = await import("../ts/practiceTool/projects.js");
const { bindPracticeToolProjectActions, renderPracticeToolProjects, suggestPracticeToolProjectName } = await import("../ts/practiceTool/projectsPanel.js");
const { showNotification } = await import("../ts/notifications.js");

const byId = <T extends HTMLElement>(id: string): T => document.getElementById(id) as T;
const rows = (): HTMLElement[] => Array.from(document.querySelectorAll<HTMLElement>(".practice-tool-link-row"));

function loadedTrack(): PracticeToolState {
  return {
    filePath: "C:\\Music\\backing.wav",
    title: "backing.wav",
    durationSec: 180.5,
    positionSec: 0,
    waveformPeaksL: [],
    waveformPeaksR: [],
    loops: [],
    activeLoopId: null,
    looping: false,
    playing: false,
    speed: 1,
    pitchSemitones: 0,
    gain: 1,
    balance: 0,
    eq: createDefaultPracticeToolEq(),
  };
}

function selectProject(projectId: string): void {
  const select = byId<HTMLSelectElement>("practice-tool-project-select");
  select.value = projectId;
  select.dispatchEvent(new Event("change"));
}

function typeLink(title: string, url: string): void {
  byId<HTMLInputElement>("practice-tool-link-title").value = title;
  const urlInput = byId<HTMLInputElement>("practice-tool-link-url");
  urlInput.value = url;
  urlInput.dispatchEvent(new Event("input"));
}

let projectId = "";

beforeEach(() => {
  vi.mocked(showNotification).mockClear();
  uiState.appSettings = {};
  uiState.practiceTool = loadedTrack();
  uiState.activePresetId = null;
  const project = capturePracticeToolProject("Song A", { includePreset: false })!;
  storePracticeToolProject(project);
  projectId = project.id;
  suggestPracticeToolProjectName("backing.wav");
  bindPracticeToolProjectActions();
  renderPracticeToolProjects();
});

describe("Practice Tool links", () => {
  it("waits for a saved project before it can take a link", () => {
    expect(byId<HTMLInputElement>("practice-tool-link-url").disabled).toBe(true);
    expect(byId<HTMLButtonElement>("practice-tool-link-add").disabled).toBe(true);
    expect(byId("practice-tool-links-hint").textContent).toContain("Save the track as a project");
  });

  it("adds a link to the selected project and draws it as a browser link", () => {
    selectProject(projectId);
    expect(byId<HTMLInputElement>("practice-tool-link-url").disabled).toBe(false);
    typeLink("Tab", "songsterr.com/a/wsa/song-tab");
    expect(byId<HTMLButtonElement>("practice-tool-link-add").disabled).toBe(false);
    byId<HTMLButtonElement>("practice-tool-link-add").click();

    expect(findPracticeToolProject(projectId)?.links?.map((link) => link.url)).toEqual(["https://songsterr.com/a/wsa/song-tab"]);
    expect(rows()).toHaveLength(1);
    const anchor = rows()[0].querySelector("a")!;
    expect(anchor.getAttribute("href")).toBe("https://songsterr.com/a/wsa/song-tab");
    expect(anchor.textContent).toBe("Tab");
    expect(byId<HTMLInputElement>("practice-tool-link-url").value).toBe("");
  });

  it("adds on Enter in the address field, and refuses what isn't a web address", () => {
    selectProject(projectId);
    typeLink("", "javascript:alert(1)");
    byId("practice-tool-link-url").dispatchEvent(new KeyboardEvent("keydown", { key: "Enter" }));
    expect(showNotification).toHaveBeenCalledWith("Not a web address", expect.any(String));
    expect(rows()).toHaveLength(0);

    typeLink("", "https://www.youtube.com/watch?v=abc");
    byId("practice-tool-link-url").dispatchEvent(new KeyboardEvent("keydown", { key: "Enter" }));
    expect(rows().map((row) => row.querySelector("a")!.textContent)).toEqual(["youtube.com"]);
  });

  it("removes a link from the saved project", async () => {
    selectProject(projectId);
    typeLink("Tab", "https://example.com/tab");
    byId<HTMLButtonElement>("practice-tool-link-add").click();
    rows()[0].querySelector<HTMLButtonElement>(".practice-tool-link-delete-btn")!.click();
    await vi.waitFor(() => expect(rows()).toHaveLength(0));
    expect(findPracticeToolProject(projectId)?.links).toBeUndefined();
  });

  it("follows the selection, and clears when a fresh track lets go of it", () => {
    const other = capturePracticeToolProject("Song B", { includePreset: false })!;
    storePracticeToolProject(other);
    renderPracticeToolProjects();
    selectProject(projectId);
    typeLink("Tab", "https://example.com/a");
    byId<HTMLButtonElement>("practice-tool-link-add").click();

    selectProject(other.id);
    expect(rows()).toHaveLength(0);
    selectProject(projectId);
    expect(rows()).toHaveLength(1);

    suggestPracticeToolProjectName("another.wav");
    expect(rows()).toHaveLength(0);
    expect(byId<HTMLInputElement>("practice-tool-link-url").disabled).toBe(true);
  });
});
