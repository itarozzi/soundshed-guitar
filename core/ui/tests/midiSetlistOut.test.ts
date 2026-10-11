import { readFileSync } from "node:fs";
import { join } from "node:path";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type * as MidiSetlistOutModule from "../ts/midiSetlistOut.js";
import type * as StateModule from "../ts/state.js";

type Sent = { type: string; key?: string; value?: unknown };

const MIDI_MODAL = readFileSync(join(__dirname, "..", "ui-components", "modals", "midi-modal.html"), "utf8");

let setlistOut: typeof MidiSetlistOutModule;
let state: typeof StateModule;
let sent: Sent[];

function el<T extends HTMLElement>(id: string): T {
  const element = document.getElementById(id);
  if (!element) throw new Error(`missing #${id}`);
  return element as T;
}

function change(element: HTMLInputElement | HTMLSelectElement, value?: string | boolean): void {
  if (typeof value === "boolean") (element as HTMLInputElement).checked = value;
  else if (value !== undefined) element.value = value;
  element.dispatchEvent(new Event("change", { bubbles: true }));
}

beforeEach(async () => {
  document.body.innerHTML = MIDI_MODAL;
  sent = [];
  window.IPlugSendMsg = (payload: string) => {
    sent.push(JSON.parse(payload) as Sent);
  };
  vi.resetModules();
  state = await import("../ts/state.js");
  setlistOut = await import("../ts/midiSetlistOut.js");
});

afterEach(() => {
  delete window.IPlugSendMsg;
});

describe("readSetlistMidiOutSettings", () => {
  it("uses the engine's defaults when nothing is stored", () => {
    state.uiState.appSettings = {};
    expect(setlistOut.readSetlistMidiOutSettings()).toEqual({ enabled: false, channel: 1, bankSelect: "msbLsb" });
  });

  it("reads stored values, and falls back for ones out of range", () => {
    state.uiState.appSettings = {
      [setlistOut.SETLIST_MIDI_OUT_ENABLED_SETTING]: true,
      [setlistOut.SETLIST_MIDI_OUT_CHANNEL_SETTING]: 10,
      [setlistOut.SETLIST_MIDI_OUT_BANK_SELECT_SETTING]: "lsb",
    };
    expect(setlistOut.readSetlistMidiOutSettings()).toEqual({ enabled: true, channel: 10, bankSelect: "lsb" });

    state.uiState.appSettings = {
      [setlistOut.SETLIST_MIDI_OUT_CHANNEL_SETTING]: 17,
      [setlistOut.SETLIST_MIDI_OUT_BANK_SELECT_SETTING]: "cc99",
    };
    expect(setlistOut.readSetlistMidiOutSettings()).toEqual({ enabled: false, channel: 1, bankSelect: "msbLsb" });
  });
});

describe("the MIDI panel's controls", () => {
  it("offers sixteen channels and shows the stored settings, greyed out while off", () => {
    state.uiState.appSettings = { [setlistOut.SETLIST_MIDI_OUT_CHANNEL_SETTING]: 4 };
    setlistOut.wireSetlistMidiOutControls();

    const channel = el<HTMLSelectElement>("midi-setlist-out-channel");
    expect(channel.options.length).toBe(16);
    expect(channel.value).toBe("4");
    expect(el<HTMLInputElement>("midi-setlist-out-enabled").checked).toBe(false);
    expect(channel.disabled).toBe(true);
    expect(el<HTMLSelectElement>("midi-setlist-out-bank-select").disabled).toBe(true);
  });

  it("sends each change as an app setting", () => {
    state.uiState.appSettings = {};
    setlistOut.wireSetlistMidiOutControls();

    change(el<HTMLInputElement>("midi-setlist-out-enabled"), true);
    change(el<HTMLSelectElement>("midi-setlist-out-channel"), "12");
    change(el<HTMLSelectElement>("midi-setlist-out-bank-select"), "msb");

    expect(sent).toEqual([
      { type: "setSetting", key: "midi.setlistOut.enabled", value: true },
      { type: "setSetting", key: "midi.setlistOut.channel", value: 12 },
      { type: "setSetting", key: "midi.setlistOut.bankSelect", value: "msb" },
    ]);
    expect(el<HTMLSelectElement>("midi-setlist-out-channel").disabled).toBe(false);
  });
});
