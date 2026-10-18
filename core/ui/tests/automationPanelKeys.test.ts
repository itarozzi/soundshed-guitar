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
  });
});

describe("keyboard learn", () => {
  it("waits through a bare modifier and backs out on Escape, storing neither", () => {
    learnButton().click();
    expect(learnArmed()).toBe(true);

    press("Shift", { shiftKey: true });
    expect(learnArmed()).toBe(true);
    press("Control", { ctrlKey: true });
    expect(learnArmed()).toBe(true);

    press("Escape");
    expect(learnArmed()).toBe(false);
    expect(sent("setAutomationSlot")).toEqual([]);
  });

  it("still learns an ordinary key, and only that one", () => {
    learnButton().click();
    press("x");
    press("y");

    expect(sent("setAutomationSlot")).toEqual([
      { type: "setAutomationSlot", slotId: "s1", keyMap: [{ key: "z", mode: 0, value: 1 }, { key: "x", mode: 0, value: 1 }] },
    ]);
  });
});

describe("mapped keys", () => {
  it("leave a chord to its shortcut: Ctrl+Z is undo, not the slot on z", () => {
    const event = press("z", { ctrlKey: true });

    expect(sent("setAutomationValue")).toEqual([]);
    expect(event.defaultPrevented).toBe(false);
  });

  it("fire on the bare key", () => {
    const event = press("z");

    expect(sent("setAutomationValue")).toEqual([{ type: "setAutomationValue", slotId: "s1", value: 1, source: "keyboard" }]);
    expect(event.defaultPrevented).toBe(true);
  });
});
