import { readFileSync } from "node:fs";
import { join } from "node:path";
import { beforeEach, describe, expect, it, vi } from "vitest";

const postMessage = vi.fn();
vi.mock("../ts/bridge.js", () => ({
  setAppSetting: (key: string, value: unknown) => postMessage({ type: "setSetting", key, value }),
}));

const PERFORMANCE_PANEL = readFileSync(
  join(__dirname, "..", "ui-components", "panels", "settings-performance.html"),
  "utf8",
);
const KEY = "audio.nam.fusedEngine";

const { uiState } = await import("../ts/state.js");
const toggleModule = await import("../ts/settings/namEngine.js");

function toggle(): HTMLInputElement {
  return document.getElementById("nam-fused-engine-toggle") as HTMLInputElement;
}

beforeEach(() => {
  postMessage.mockClear();
  document.body.innerHTML = PERFORMANCE_PANEL;
  uiState.appSettings = {};
});

describe("Fast NAM Engine", () => {
  it("is on when nothing is stored", () => {
    toggleModule.initNamEngineToggle();
    expect(toggle().checked).toBe(true);
    expect(postMessage).not.toHaveBeenCalled();
  });

  it("shows a stored off and sends a change", () => {
    uiState.appSettings = { [KEY]: false };
    toggleModule.initNamEngineToggle();
    expect(toggle().checked).toBe(false);

    toggle().checked = true;
    toggle().dispatchEvent(new Event("change"));
    expect(postMessage).toHaveBeenCalledWith({ type: "setSetting", key: KEY, value: true });
  });

  it("follows settings that arrive after the panel binds", () => {
    toggleModule.initNamEngineToggle();
    uiState.appSettings = { [KEY]: false };
    toggleModule.refreshNamEngineToggle();
    expect(toggle().checked).toBe(false);
  });
});
