import { beforeEach, describe, expect, it } from "vitest";
import { uiState } from "../ts/state.js";
import { createDefaultPracticeToolEq } from "../ts/practiceTool/eq.js";
import type { PracticeToolProject, PracticeToolState } from "../ts/types.js";
import {
  addPracticeToolProjectLink,
  capturePracticeToolProject,
  consumePendingProjectRecall,
  deletePracticeToolProject,
  describePracticeToolLink,
  findPracticeToolProject,
  findPracticeToolProjectByName,
  getProjectTrackAvailability,
  MAX_PRACTICE_TOOL_PROJECT_LINKS,
  normalizePracticeToolLinkUrl,
  parsePracticeToolProject,
  readPracticeToolProjects,
  removePracticeToolProjectLink,
  setPendingProjectRecall,
  storePracticeToolProject,
} from "../ts/practiceTool/projects.js";

const PROJECTS_KEY = "practiceTool.projects";

function loadedTrack(overrides: Partial<PracticeToolState> = {}): PracticeToolState {
  return {
    filePath: "C:\\Music\\backing.wav",
    title: "backing.wav",
    durationSec: 180.5,
    positionSec: 0,
    waveformPeaksL: [],
    waveformPeaksR: [],
    loops: [
      { id: "loop-a", name: "Verse 1", startSec: 4, endSec: 12 },
      { id: "loop-b", name: "Solo 1", startSec: 60, endSec: 75 },
    ],
    activeLoopId: "loop-b",
    looping: true,
    playing: false,
    speed: 0.75,
    pitchSemitones: -2,
    gain: 0.8,
    balance: 0.25,
    eq: createDefaultPracticeToolEq(),
    ...overrides,
  };
}

beforeEach(() => {
  uiState.appSettings = {};
  uiState.practiceTool = loadedTrack();
  uiState.activePresetId = null;
  uiState.presets = [];
  setPendingProjectRecall(null);
});

describe("capturePracticeToolProject", () => {
  it("captures the track, its loops and every fader setting", () => {
    const project = capturePracticeToolProject("Song A", { includePreset: false });
    expect(project).not.toBeNull();
    expect(project).toMatchObject({
      name: "Song A",
      filePath: "C:\\Music\\backing.wav",
      fileTitle: "backing.wav",
      durationSec: 180.5,
      activeLoopId: "loop-b",
      gain: 0.8,
      balance: 0.25,
      speed: 0.75,
      pitchSemitones: -2,
    });
    expect(project?.loops).toHaveLength(2);
    expect(project?.presetId).toBeUndefined();
  });

  it("copies the loops rather than aliasing the live ones", () => {
    const project = capturePracticeToolProject("Song A", { includePreset: false });
    uiState.practiceTool!.loops[0].name = "renamed after saving";
    expect(project?.loops[0].name).toBe("Verse 1");
  });

  it("includes the selected preset — with its display name — only when asked", () => {
    uiState.activePresetId = "preset-7";
    uiState.presets = [{ id: "preset-7", name: "Crunch Rhythm" }] as typeof uiState.presets;

    expect(capturePracticeToolProject("Song A", { includePreset: false })?.presetId).toBeUndefined();
    expect(capturePracticeToolProject("Song A", { includePreset: true })).toMatchObject({
      presetId: "preset-7",
      presetName: "Crunch Rhythm",
    });
  });

  it("refuses to capture without a loaded track or a name", () => {
    expect(capturePracticeToolProject("   ", { includePreset: false })).toBeNull();
    uiState.practiceTool = loadedTrack({ filePath: "" });
    expect(capturePracticeToolProject("Song A", { includePreset: false })).toBeNull();
  });

  it("captures the backing-track EQ, detached from the live state", () => {
    uiState.practiceTool!.eq = { enabled: true, params: { ...createDefaultPracticeToolEq().params, lowGain: 4.5 } };
    const project = capturePracticeToolProject("Song A", { includePreset: false })!;

    expect(project.eq).toMatchObject({ enabled: true });
    expect(project.eq?.params.lowGain).toBe(4.5);

    uiState.practiceTool!.eq.params.lowGain = -9;
    expect(project.eq?.params.lowGain).toBe(4.5);
  });

  it("reuses the given id so saving over a project updates it in place", () => {
    const first = capturePracticeToolProject("Song A", { includePreset: false })!;
    storePracticeToolProject(first);
    const second = capturePracticeToolProject("Song A", { includePreset: false, projectId: first.id })!;
    storePracticeToolProject(second);
    expect(readPracticeToolProjects()).toHaveLength(1);
  });
});

describe("project storage", () => {
  it("round-trips through app settings, newest first", () => {
    const older = { ...capturePracticeToolProject("Older", { includePreset: false })!, savedAt: 1_000 };
    const newer = { ...capturePracticeToolProject("Newer", { includePreset: false })!, savedAt: 2_000 };
    storePracticeToolProject(older);
    storePracticeToolProject(newer);

    expect(readPracticeToolProjects().map((project) => project.name)).toEqual(["Newer", "Older"]);
  });

  it("matches saved names case-insensitively so Save offers to overwrite", () => {
    storePracticeToolProject(capturePracticeToolProject("Song A", { includePreset: false })!);
    expect(findPracticeToolProjectByName("  song a  ")?.name).toBe("Song A");
    expect(findPracticeToolProjectByName("Song B")).toBeNull();
  });

  it("deletes by id and reports whether anything was removed", () => {
    const project = capturePracticeToolProject("Song A", { includePreset: false })!;
    storePracticeToolProject(project);
    expect(deletePracticeToolProject("no-such-project")).toBe(false);
    expect(deletePracticeToolProject(project.id)).toBe(true);
    expect(readPracticeToolProjects()).toEqual([]);
  });

  it("skips entries a hand-edited settings blob has broken, keeping the rest", () => {
    const good = capturePracticeToolProject("Song A", { includePreset: false })!;
    uiState.appSettings[PROJECTS_KEY] = [
      good,
      { id: "no-name", filePath: "C:\\x.wav" },
      "not an object",
      null,
    ] as unknown as typeof uiState.appSettings[string];

    expect(readPracticeToolProjects().map((project) => project.name)).toEqual(["Song A"]);
  });
});

describe("parsePracticeToolProject", () => {
  it("fills in the settings an older entry never recorded", () => {
    const parsed = parsePracticeToolProject({
      id: "p1",
      name: "Song A",
      filePath: "C:\\Music\\backing.wav",
    });
    expect(parsed).toMatchObject({
      fileTitle: "backing.wav",
      durationSec: 0,
      loops: [],
      activeLoopId: null,
      gain: 1,
      balance: 0,
      speed: 1,
      pitchSemitones: 0,
    });
  });

  it("drops an active loop id that no longer names one of the saved loops", () => {
    const parsed = parsePracticeToolProject({
      id: "p1",
      name: "Song A",
      filePath: "C:\\Music\\backing.wav",
      loops: [{ id: "loop-a", name: "Verse 1", startSec: 1, endSec: 2 }],
      activeLoopId: "loop-gone",
    });
    expect(parsed?.activeLoopId).toBeNull();
  });

  it("keeps each loop's own track settings, and drops a set with a gap rather than half-applying it", () => {
    const settings = { gain: 0.6, balance: -0.5, speed: 0.7, pitchSemitones: 2 };
    const parsed = parsePracticeToolProject({
      id: "p1",
      name: "Song A",
      filePath: "C:\\Music\\backing.wav",
      loops: [
        { id: "loop-a", name: "Verse 1", startSec: 1, endSec: 2, settings },
        { id: "loop-b", name: "Solo 1", startSec: 3, endSec: 4, settings: { ...settings, speed: "slow" } },
        { id: "loop-c", name: "Outro 1", startSec: 5, endSec: 6 },
      ],
    });
    expect(parsed?.loops[0].settings).toEqual(settings);
    expect(parsed?.loops[1]).not.toHaveProperty("settings");
    expect(parsed?.loops[2]).not.toHaveProperty("settings");
  });

  it("leaves eq undefined for an entry saved before the EQ existed", () => {
    const parsed = parsePracticeToolProject({ id: "p1", name: "Song A", filePath: "C:\\Music\\backing.wav" });
    expect(parsed?.eq).toBeUndefined();
  });

  it("repairs a stored EQ rather than dropping the whole project", () => {
    const parsed = parsePracticeToolProject({
      id: "p1",
      name: "Song A",
      filePath: "C:\\Music\\backing.wav",
      eq: { enabled: true, params: { lowGain: 6, highFreq: "not a number" } },
    });
    expect(parsed?.eq?.enabled).toBe(true);
    expect(parsed?.eq?.params.lowGain).toBe(6);
    expect(parsed?.eq?.params.highFreq).toBe(8000); // the effect's own default fills the gap
  });

  it("keeps a preset name only alongside the preset id it describes", () => {
    const orphaned = parsePracticeToolProject({
      id: "p1",
      name: "Song A",
      filePath: "C:\\Music\\backing.wav",
      presetName: "Crunch Rhythm",
    });
    expect(orphaned?.presetId).toBeUndefined();
    expect(orphaned?.presetName).toBeUndefined();
  });
});

describe("getProjectTrackAvailability", () => {
  const project = (filePath: string): PracticeToolProject => ({
    ...capturePracticeToolProject("Song A", { includePreset: false })!,
    filePath,
  });

  it("reports the loaded track regardless of path casing", () => {
    expect(getProjectTrackAvailability(project("c:\\music\\BACKING.wav"))).toBe("loaded");
  });

  it("reports a reloadable path for Windows, UNC and POSIX absolutes", () => {
    uiState.practiceTool = loadedTrack({ filePath: "" });
    expect(getProjectTrackAvailability(project("D:/tracks/song.mp3"))).toBe("reload");
    expect(getProjectTrackAvailability(project("\\\\nas\\share\\song.wav"))).toBe("reload");
    expect(getProjectTrackAvailability(project("/home/chris/song.wav"))).toBe("reload");
  });

  it("reports a dropped-in file — recorded as a bare name — as unavailable", () => {
    uiState.practiceTool = loadedTrack({ filePath: "" });
    expect(getProjectTrackAvailability(project("song.wav"))).toBe("unavailable");
  });
});

describe("consumePendingProjectRecall", () => {
  it("hands back the parked project when its own file load lands", () => {
    const project = capturePracticeToolProject("Song A", { includePreset: false })!;
    setPendingProjectRecall(project);
    expect(consumePendingProjectRecall("c:\\music\\backing.wav")?.id).toBe(project.id);
  });

  it("drops the recall when the user loads some other file first", () => {
    setPendingProjectRecall(capturePracticeToolProject("Song A", { includePreset: false })!);
    expect(consumePendingProjectRecall("C:\\Music\\something-else.wav")).toBeNull();
  });

  it("is one-shot, so a later load is not silently reconfigured", () => {
    setPendingProjectRecall(capturePracticeToolProject("Song A", { includePreset: false })!);
    expect(consumePendingProjectRecall("C:\\Music\\backing.wav")).not.toBeNull();
    expect(consumePendingProjectRecall("C:\\Music\\backing.wav")).toBeNull();
  });
});

describe("project links", () => {
  function savedProject(name = "Song A"): PracticeToolProject {
    const project = capturePracticeToolProject(name, { includePreset: false })!;
    storePracticeToolProject(project);
    return project;
  }

  it("normalises pasted addresses and refuses anything the browser can't open", () => {
    expect(normalizePracticeToolLinkUrl("  https://www.songsterr.com/a/wsa/song-tab  ")).toBe("https://www.songsterr.com/a/wsa/song-tab");
    expect(normalizePracticeToolLinkUrl("songsterr.com/a/wsa/song-tab")).toBe("https://songsterr.com/a/wsa/song-tab");
    expect(normalizePracticeToolLinkUrl("http://example.com")).toBe("http://example.com/");
    expect(normalizePracticeToolLinkUrl("")).toBeNull();
    expect(normalizePracticeToolLinkUrl("javascript:alert(1)")).toBeNull();
    expect(normalizePracticeToolLinkUrl("mailto:someone@example.com")).toBeNull();
    expect(normalizePracticeToolLinkUrl("C:\\Tabs\\song.pdf")).toBeNull();
    expect(normalizePracticeToolLinkUrl("file:///C:/Tabs/song.pdf")).toBeNull();
    expect(normalizePracticeToolLinkUrl("not a url")).toBeNull();
    expect(normalizePracticeToolLinkUrl("tab")).toBeNull();
  });

  it("adds a link to the saved project straight away, without touching savedAt", () => {
    const project = savedProject();
    expect(addPracticeToolProjectLink(project.id, " Tab ", "songsterr.com/a/wsa/song-tab")).toBe("added");
    const stored = findPracticeToolProject(project.id)!;
    expect(stored.links).toEqual([{ id: expect.any(String), title: "Tab", url: "https://songsterr.com/a/wsa/song-tab" }]);
    expect(stored.savedAt).toBe(project.savedAt);
  });

  it("reports why a link was not added", () => {
    const project = savedProject();
    expect(addPracticeToolProjectLink("missing", "", "https://example.com")).toBe("no-project");
    expect(addPracticeToolProjectLink(project.id, "", "javascript:alert(1)")).toBe("invalid-url");
    for (let i = 0; i < MAX_PRACTICE_TOOL_PROJECT_LINKS; ++i) {
      expect(addPracticeToolProjectLink(project.id, "", `https://example.com/${i}`)).toBe("added");
    }
    expect(addPracticeToolProjectLink(project.id, "", "https://example.com/one-more")).toBe("too-many");
  });

  it("removes a link, dropping the field once the last one goes", () => {
    const project = savedProject();
    addPracticeToolProjectLink(project.id, "Tab", "https://example.com/tab");
    const linkId = findPracticeToolProject(project.id)!.links![0].id;
    expect(removePracticeToolProjectLink(project.id, "missing")).toBe(false);
    expect(removePracticeToolProjectLink(project.id, linkId)).toBe(true);
    expect(findPracticeToolProject(project.id)!.links).toBeUndefined();
  });

  it("keeps a project's links when Save overwrites it", () => {
    const project = savedProject();
    addPracticeToolProjectLink(project.id, "Tab", "https://example.com/tab");
    const resaved = capturePracticeToolProject("Song A", { includePreset: false, projectId: project.id })!;
    expect(resaved.links?.map((link) => link.url)).toEqual(["https://example.com/tab"]);
    expect(capturePracticeToolProject("Song B", { includePreset: false })!.links).toBeUndefined();
  });

  it("drops stored links that are broken or unopenable when reading projects back", () => {
    const parsed = parsePracticeToolProject({
      id: "p1",
      name: "Song",
      filePath: "C:\\Music\\backing.wav",
      links: [
        { id: "l1", title: "Tab", url: "https://example.com/tab" },
        { id: "l2", title: "Bad", url: "javascript:alert(1)" },
        { id: "", title: "No id", url: "https://example.com" },
        "https://example.com",
      ],
    });
    expect(parsed?.links).toEqual([{ id: "l1", title: "Tab", url: "https://example.com/tab" }]);
  });

  it("labels an untitled link with its host", () => {
    expect(describePracticeToolLink({ id: "l", title: "", url: "https://www.songsterr.com/a/wsa/x" })).toBe("songsterr.com");
    expect(describePracticeToolLink({ id: "l", title: "Lesson", url: "https://youtube.com/watch?v=1" })).toBe("Lesson");
  });
});
