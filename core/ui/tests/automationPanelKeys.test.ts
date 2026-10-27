import { beforeEach, describe, expect, it, vi } from "vitest";

const { postMessage } = vi.hoisted(() => ({ postMessage: vi.fn() }));
vi.mock("../ts/bridge.js", () => ({ postMessage, setAppSetting: vi.fn() }));
vi.mock("../ts/logging.js", () => ({ appendLog: vi.fn() }));

// The keydown listener is registered when the module loads; the panel it reads is here.
document.body.innerHTML = `
  <div id="midi-modal" style="display: flex;"></div>
  <div id="keyboard-slots-list"></div>`;

const { applyAutomationState } = await import("../ts/automationPanel.js");

function press(key: string, init: KeyboardEventInit = {}): KeyboardEvent {
  const event = new KeyboardEvent("keydown", { key, bubbles: true, cancelable: true, ...init });
  document.dispatchEvent(event);
  return event;
}

function sent(type: string): Array<Record<string, unknown>> {
  return postMessage.mock.calls
    .map(([message]) => message as Record<string, unknown>)
    .filter((message) => message.type === type);
}

const learnButton = (): HTMLButtonElement => document.querySelector(".automation-key-learn-btn") as HTMLButtonElement;
const learnArmed = (): boolean => learnButton().classList.contains("active");

beforeEach(() => {
  postMessage.mockClear();
  applyAutomationState({
    slots: [{ slotId: "s1", label: "Bank Up", address: "setlist.bankUp", keyMap: [{ key: "z", mode: 0, value: 1 }] }] as never,
    registry: [],
    maxCustomSlots: 8,
    keyLearnSlotId: "",
  });
});

// The engine runs the mappings and the learn (automationKey, armKeyLearn); the panel only
// decides which keys to pass on.
describe("keyboard learn", () => {
  it("is armed in the engine, and takes every key there until the engine says it is done", () => {
    learnButton().click();
    expect(sent("armKeyLearn")).toEqual([{ type: "armKeyLearn", slotId: "s1" }]);
    expect(learnArmed()).toBe(true);

    // A bare modifier and Escape go to the engine, which waits through the one and backs out on the other.
    const shift = press("Shift", { shiftKey: true });
    const escape = press("Escape");
    expect(sent("automationKey").map((message) => message.key)).toEqual(["Shift", "Escape"]);
    expect(shift.defaultPrevented && escape.defaultPrevented).toBe(true);
    expect(sent("setAutomationSlot")).toEqual([]);

    applyAutomationState({ keyLearnSlotId: "" });
    expect(learnArmed()).toBe(false);
  });

  it("passes an ordinary key to the engine to store, and stores nothing itself", () => {
    learnButton().click();
    press("x");

    expect(sent("automationKey")).toEqual([{ type: "automationKey", key: "x", repeat: false }]);
    expect(sent("setAutomationSlot")).toEqual([]);
  });

  it("is cancelled by pressing Learn again", () => {
    learnButton().click();
    learnButton().click();
    expect(sent("armKeyLearn")).toEqual([{ type: "armKeyLearn", slotId: "s1" }, { type: "armKeyLearn", slotId: "" }]);
  });
});

describe("mapped keys", () => {
  it("leave a chord to its shortcut: Ctrl+Z is undo, not the slot on z", () => {
    const event = press("z", { ctrlKey: true });

    expect(sent("automationKey")).toEqual([]);
    expect(event.defaultPrevented).toBe(false);
  });

  it("go to the engine to fire, held back from the page", () => {
    const event = press("Z", { repeat: true });

    expect(sent("automationKey")).toEqual([{ type: "automationKey", key: "Z", repeat: true }]);
    expect(event.defaultPrevented).toBe(true);
  });

  it("leave a key no slot maps, and Space, to the page", () => {
    const q = press("q");
    const space = press(" ");

    expect(sent("automationKey")).toEqual([]);
    expect(q.defaultPrevented || space.defaultPrevented).toBe(false);
  });
});
