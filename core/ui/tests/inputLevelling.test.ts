/**
 * Input levelling on the engine's commands: the flyout and the training dialog draw what the
 * engine reports ("inputLevelling", "inputLevellingTraining", "inputLevellingSaved") and send
 * the user's choices as commands. No profile is parsed, stored or trimmed here any more.
 */
import { beforeEach, describe, expect, it, vi } from "vitest";

vi.mock("../ts/notifications.js", () => ({ clearNotification: vi.fn(), showNotification: vi.fn() }));
vi.mock("../ts/dialogs.js", () => ({ showConfirm: vi.fn(async () => true) }));

document.body.innerHTML = `
  <div class="control-bar">
    <button id="user-input-calibration-toolbar-trigger" type="button" aria-expanded="false"></button>
    <div id="user-input-calibration-toolbar-menu" hidden></div>
  </div>
  <div id="user-input-calibration-modal" style="display: none;">
    <span id="user-input-calibration-live-peak">-</span>
    <span id="user-input-calibration-captured-peak">-</span>
    <span id="user-input-calibration-recommended-trim">-</span>
    <span id="user-input-calibration-status"></span>
    <button id="user-input-calibration-reset" type="button"></button>
    <input id="user-input-calibration-name" type="text" />
    <textarea id="user-input-calibration-description"></textarea>
    <button id="user-input-calibration-cancel" type="button"></button>
    <button id="user-input-calibration-save" type="button"></button>
  </div>
`;
type Sent = { type: string; [key: string]: unknown };
let sent: Sent[] = [];
window.IPlugSendMsg = (payload: string) => {
  sent.push(JSON.parse(payload) as Sent);
};

const calibration = await import("../ts/settings/inputCalibration.js");
const { showNotification } = await import("../ts/notifications.js");

const menu = () => document.getElementById("user-input-calibration-toolbar-menu") as HTMLElement;
const modal = () => document.getElementById("user-input-calibration-modal") as HTMLElement;
const text = (id: string) => document.getElementById(id)?.textContent ?? "";
const sentOf = (type: string) => sent.filter((m) => m.type === type);
const flush = () => new Promise((resolve) => setTimeout(resolve, 0));

const STRAT = { id: "strat", name: "Strat", description: "Bridge", capturedPeakDbfs: -20, targetPeakDbfs: -12, gainDb: 8 };
const TELE = { id: "tele", name: "Tele", description: "", capturedPeakDbfs: null, targetPeakDbfs: -12, gainDb: -3 };

calibration.initUserInputCalibrationControls();
const requestsAtStart = sentOf("getInputLevelling").length;

describe("Input levelling on the engine", () => {
  beforeEach(() => {
    calibration.closeUserInputCalibrationTrainingModal();
    calibration.closeUserInputCalibrationToolbarMenu();
    calibration.onInputLevelling({ type: "inputLevelling", profiles: [STRAT, TELE], activeId: "strat", trainingActive: false, targetPeakDbfs: -12 });
    sent = [];
    vi.mocked(showNotification).mockClear();
  });

  it("asks the engine for the profiles when it starts", () => {
    expect(requestsAtStart).toBe(1);
  });

  it("lists the engine's profiles with the active one", () => {
    calibration.openUserInputCalibrationToolbarMenu("calibration");
    const panel = menu().querySelector('[data-tab="calibration"].input-calibration-tab-panel') as HTMLElement;
    expect(panel.textContent).toContain("Strat active");
    expect(panel.textContent).toContain("Applies +8.0 dB toward -12.0 dBFS");
    expect(panel.textContent).toContain("Tele");
    expect(calibration.getActiveUserInputCalibrationProfile()?.id).toBe("strat");
  });

  it("selects a profile with a command", () => {
    calibration.openUserInputCalibrationToolbarMenu("calibration");
    (menu().querySelector('.input-calibration-toolbar-item[data-profile-id="tele"]') as HTMLButtonElement).click();
    expect(sentOf("selectInputLevellingProfile")).toEqual([{ type: "selectInputLevellingProfile", id: "tele" }]);
    expect(calibration.readActiveUserInputCalibrationProfileId()).toBe("tele");
  });

  it("deletes a profile with a command after confirming", async () => {
    calibration.openUserInputCalibrationToolbarMenu("calibration");
    (menu().querySelector('.input-calibration-toolbar-item-delete[data-profile-id="tele"]') as HTMLButtonElement).click();
    await flush();
    expect(sentOf("deleteInputLevellingProfile")).toEqual([{ type: "deleteInputLevellingProfile", id: "tele" }]);
  });

  it("trains on the engine's peak and saves with a name", () => {
    calibration.openUserInputCalibrationTrainingModal();
    expect(sentOf("startInputLevellingTraining")).toHaveLength(1);
    expect(modal().style.display).toBe("flex");
    expect(text("user-input-calibration-status")).toContain("Play your loudest");

    calibration.onInputLevellingTraining({ type: "inputLevellingTraining", active: true, livePeakDbfs: -24.5,
      capturedPeakDbfs: -20, recommendedTrimDb: 8, clipped: false, targetPeakDbfs: -12 });
    expect(text("user-input-calibration-live-peak")).toBe("-24.5 dBFS");
    expect(text("user-input-calibration-captured-peak")).toBe("-20.0 dBFS");
    expect(text("user-input-calibration-recommended-trim")).toBe("+8.0 dB");
    expect(text("user-input-calibration-status")).toBe(
      "Captured -20.0 dBFS. Saving this profile will apply +8.0 dB toward a -12.0 dBFS target.");
    const save = document.getElementById("user-input-calibration-save") as HTMLButtonElement;
    expect(save.disabled).toBe(true); // no name yet

    const name = document.getElementById("user-input-calibration-name") as HTMLInputElement;
    name.value = "Les Paul";
    name.dispatchEvent(new Event("input"));
    expect(save.disabled).toBe(false);
    save.click();
    expect(sentOf("saveInputLevellingProfile")).toEqual([{ type: "saveInputLevellingProfile", name: "Les Paul", description: "" }]);

    calibration.onInputLevellingSaved({ type: "inputLevellingSaved", ok: true,
      profile: { id: "lp", name: "Les Paul", capturedPeakDbfs: -20, targetPeakDbfs: -12, gainDb: 8 } });
    expect(modal().style.display).toBe("none");
    expect(showNotification).toHaveBeenCalledWith("Input levelling profile saved", "Les Paul • +8.0 dB");
  });

  it("reports a clip and keeps Save off; Reset and Cancel are commands", () => {
    calibration.openUserInputCalibrationTrainingModal();
    const name = document.getElementById("user-input-calibration-name") as HTMLInputElement;
    name.value = "Hot";
    calibration.onInputLevellingTraining({ type: "inputLevellingTraining", active: true, livePeakDbfs: 1,
      capturedPeakDbfs: 3.5, recommendedTrimDb: -15.5, clipped: true });
    expect(text("user-input-calibration-status")).toContain("Input clipped during training");
    expect((document.getElementById("user-input-calibration-save") as HTMLButtonElement).disabled).toBe(true);

    (document.getElementById("user-input-calibration-reset") as HTMLButtonElement).click();
    expect(sentOf("resetInputLevellingTraining")).toHaveLength(1);
    (document.getElementById("user-input-calibration-cancel") as HTMLButtonElement).click();
    expect(sentOf("stopInputLevellingTraining")).toHaveLength(1);
    expect(modal().style.display).toBe("none");
  });

  it("tells a failed save without closing the dialog", () => {
    calibration.openUserInputCalibrationTrainingModal();
    calibration.onInputLevellingSaved({ type: "inputLevellingSaved", ok: false, error: "noPeak", message: "No training level captured." });
    expect(modal().style.display).toBe("flex");
    expect(showNotification).toHaveBeenCalledWith("Input levelling profile not saved", "No training level captured.");
  });

  it("shows the training bypass while the engine trains", () => {
    calibration.onInputLevelling({ type: "inputLevelling", profiles: [STRAT], activeId: "strat", trainingActive: true });
    calibration.openUserInputCalibrationToolbarMenu("calibration");
    expect(menu().textContent).toContain("Training bypass active");
  });
});
