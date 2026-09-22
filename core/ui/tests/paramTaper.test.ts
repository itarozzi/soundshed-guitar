import { afterEach, beforeEach, describe, expect, it } from "vitest";
import { GenericKnob } from "../ts/knob.js";
import { formatParamValue, renderCustomLayoutPreviewLayers } from "../ts/layoutRenderer.js";
import type { EffectLayout } from "../ts/layoutTypes.js";
import { onEffectCatalog } from "../ts/messages/effectHandlers.js";
import { buildDefaultParamControlsHtml } from "../ts/parameterControlMarkup.js";
import {
  effectiveTaper,
  formatTaperedValue,
  parseParamTaper,
  taperPositionToValue,
  valueToTaperPosition,
} from "../ts/paramTaper.js";
import { EffectTypeRegistry } from "../ts/presetV2.js";
import type { GraphNode } from "../ts/types.js";

/** jsdom does not implement PointerEvent, and only the geometry matters here. */
function pointerEvent(type: string, y: number): PointerEvent {
  const event = new MouseEvent(type, { bubbles: true, clientX: 0, clientY: y, button: 0 });
  Object.defineProperty(event, "pointerId", { value: 1 });
  return event as PointerEvent;
}

function wheelUp(): WheelEvent {
  return new WheelEvent("wheel", { deltaY: -100, bubbles: true, cancelable: true });
}

/** A 1-2000 Hz knob set up the way bindNodeParamControls sets one up. */
function mountKnob(value: number, options: { taper?: "log" | "linear"; step?: number; min?: number; max?: number } = {}) {
  const min = options.min ?? 1;
  const max = options.max ?? 2000;
  document.body.innerHTML = `<div class="knob" data-value="${value}"><div class="knob-indicator"></div></div>`;
  const element = document.querySelector<HTMLElement>(".knob")!;
  const changes: number[] = [];
  const knob = new GenericKnob({
    knobElement: element,
    paramId: "ring_frequency",
    minValue: min,
    maxValue: max,
    defaultValue: 440,
    displayFormat: (v) => `${v}`,
    sensitivity: (max - min) / 200,
    stepValue: options.step,
    taper: options.taper,
    sendParameter: false,
    onValueChange: (v) => changes.push(v),
  });
  const drag = (pixelsUp: number) => {
    element.dispatchEvent(pointerEvent("pointerdown", 500));
    element.dispatchEvent(pointerEvent("pointermove", 500 - pixelsUp));
    element.dispatchEvent(pointerEvent("pointerup", 500 - pixelsUp));
  };
  return { knob, element, changes, drag };
}

describe("taper maths", () => {
  it("maps a log taper by ratio, exactly at both ends", () => {
    expect(taperPositionToValue(0, 1, 2000, "log")).toBe(1);
    expect(taperPositionToValue(1, 1, 2000, "log")).toBe(2000);
    expect(taperPositionToValue(0.5, 1, 2000, "log")).toBeCloseTo(Math.sqrt(2000), 9);
    // 1-100 Hz, the growl region, is 60% of the travel rather than 5%
    expect(valueToTaperPosition(100, 1, 2000, "log")).toBeCloseTo(0.606, 3);
    expect(valueToTaperPosition(100, 1, 2000, "linear")).toBeCloseTo(0.0495, 4);
  });

  it("round-trips value and position", () => {
    for (const position of [0, 0.1, 0.33, 0.5, 0.9, 1]) {
      const value = taperPositionToValue(position, 20, 20000, "log");
      expect(valueToTaperPosition(value, 20, 20000, "log")).toBeCloseTo(position, 12);
    }
  });

  it("is linear when absent, and falls back to linear over a range a log taper cannot map", () => {
    expect(taperPositionToValue(0.5, 1, 2000)).toBe(1000.5);
    expect(effectiveTaper("log", 0, 1)).toBe("linear");
    expect(effectiveTaper("log", -12, 12)).toBe("linear");
    expect(effectiveTaper("log", 2000, 1)).toBe("linear");
    expect(effectiveTaper(undefined, 1, 2000)).toBe("linear");
    expect(taperPositionToValue(0.5, 0, 1, "log")).toBe(0.5);
  });

  it("clamps positions and treats a non-finite one as the bottom", () => {
    expect(taperPositionToValue(1.5, 1, 2000, "log")).toBe(2000);
    expect(taperPositionToValue(-1, 1, 2000, "log")).toBe(1);
    expect(taperPositionToValue(Number.NaN, 1, 2000, "log")).toBe(1);
  });

  it("parses only the names the engine sends", () => {
    expect(parseParamTaper("log")).toBe("log");
    expect(parseParamTaper("linear")).toBe("linear");
    expect(parseParamTaper("exp")).toBeUndefined();
    expect(parseParamTaper(undefined)).toBeUndefined();
  });

  it("formats a tapered value to about three figures, in kHz from 1 kHz", () => {
    expect(formatTaperedValue(1.234, "Hz")).toBe("1.23Hz");
    expect(formatTaperedValue(44.72, "Hz")).toBe("44.7Hz");
    expect(formatTaperedValue(440, "Hz")).toBe("440Hz");
    expect(formatTaperedValue(999.7, "Hz")).toBe("1.00kHz");
    expect(formatTaperedValue(1850, "Hz")).toBe("1.85kHz");
    expect(formatTaperedValue(12000, "Hz")).toBe("12.0kHz");
    expect(formatTaperedValue(0.5, "amount")).toBe("0.50");
    expect(formatParamValue(440, "Hz", undefined, "log")).toBe("440Hz");
    expect(formatParamValue(440, "Hz")).toBe("440.0Hz");
  });
});

describe("GenericKnob on a log taper", () => {
  beforeEach(() => {
    document.body.innerHTML = "";
  });

  it("drags through the taper: half the sweep lands on the geometric mean", () => {
    const { knob, drag } = mountKnob(1, { taper: "log" });
    drag(100);
    expect(knob.getValue()).toBeCloseTo(Math.sqrt(2000), 6);
  });

  it("takes the same drag end to end as a linear knob", () => {
    const tapered = mountKnob(1, { taper: "log" });
    tapered.drag(200);
    expect(tapered.knob.getValue()).toBeCloseTo(2000, 9);
    tapered.drag(1);
    expect(tapered.knob.getValue()).toBe(2000);

    const linear = mountKnob(1);
    linear.drag(200);
    expect(linear.knob.getValue()).toBeCloseTo(2000, 9);
  });

  it("leaves a linear knob as it was", () => {
    const { knob, drag } = mountKnob(1);
    drag(100);
    expect(knob.getValue()).toBeCloseTo(1 + 100 * (1999 / 200), 9);
  });

  it("turns the indicator by travel, not by value", () => {
    const { element } = mountKnob(Math.sqrt(2000), { taper: "log" });
    expect(Number(element.style.getPropertyValue("--knob-pct"))).toBeCloseTo(0.5, 9);
    const indicator = element.querySelector<HTMLElement>(".knob-indicator")!;
    const degrees = Number(/rotate\((-?[\d.e+-]+)deg\)/.exec(indicator.style.transform)?.[1]);
    expect(degrees).toBeCloseTo(0, 9);
  });

  it("moves a wheel notch by an equal ratio", () => {
    const { knob, element } = mountKnob(440, { taper: "log" });
    element.dispatchEvent(wheelUp());
    expect(knob.getValue()).toBeCloseTo(440 * Math.pow(2000, 0.01), 6);
  });

  it("moves at least one declared step, so a snap cannot hold it at the bottom", () => {
    const { knob, element } = mountKnob(1, { taper: "log", step: 1 });
    element.dispatchEvent(wheelUp());
    expect(knob.getValue()).toBe(2);
  });

  it("falls back to linear when a new range cannot be log-mapped", () => {
    const { knob, drag } = mountKnob(1, { taper: "log" });
    knob.setRange(0, 1);
    knob.setValue(0);
    drag(100);
    expect(knob.getValue()).toBeCloseTo(0.5, 9);
  });
});

describe("taper in markup and the catalog", () => {
  afterEach(() => {
    document.body.innerHTML = "";
  });

  it("marks a log-taper knob and formats its value", () => {
    const html = buildDefaultParamControlsHtml([
      { key: "frequency", name: "Frequency", default: 440, min: 1, max: 2000, unit: "Hz", taper: "log" },
      { key: "mix", name: "Mix", default: 1, min: 0, max: 1, unit: "amount" },
    ]);
    const host = document.createElement("div");
    host.innerHTML = html;
    const frequency = host.querySelector<HTMLElement>('[data-param-key="frequency"]')!;
    const mix = host.querySelector<HTMLElement>('[data-param-key="mix"]')!;
    expect(frequency.dataset.taper).toBe("log");
    expect(mix.dataset.taper).toBeUndefined();
    expect(html).toContain(">440Hz<");
  });

  it("lays out a log-taper knob and slider in a custom layout, the slider over its travel", () => {
    const frequency = { key: "frequency", name: "Frequency", default: 440, min: 1, max: 2000, unit: "Hz", taper: "log" as const };
    const control = (type: "knob" | "slider", x: number) => ({ paramKey: "frequency", type, position: { x, y: 0 } });
    const host = document.createElement("div");
    host.innerHTML = renderCustomLayoutPreviewLayers(
      { id: "r1", type: "ring_mod", params: { frequency: 100 } } as unknown as GraphNode,
      { controls: [control("knob", 0), control("slider", 100)], textLabels: [] } as unknown as EffectLayout,
      [frequency],
    );
    const knob = host.querySelector<HTMLElement>(".node-param-knob")!;
    const slider = host.querySelector<HTMLInputElement>(".node-param-slider")!;
    expect(knob.dataset.taper).toBe("log");
    expect(knob.dataset.value).toBe("100");
    expect(slider.dataset.taper).toBe("log");
    expect([slider.min, slider.max, slider.step]).toEqual(["0", "1", "any"]);
    expect([slider.dataset.min, slider.dataset.max]).toEqual(["1", "2000"]);
    expect(Number(slider.value)).toBeCloseTo(Math.log(100) / Math.log(2000), 6);
    expect(host.querySelector(".node-param-value")?.textContent).toBe("100Hz");
  });

  it("reads the taper from the effect catalog, and absent as linear", () => {
    const type = "test-taper-effect";
    onEffectCatalog({
      type: "effectCatalog",
      catalog: [{
        type,
        name: "Taper Test",
        category: "modulation",
        parameters: [
          { key: "frequency", name: "Frequency", min: 1, max: 2000, default: 440, unit: "Hz", taper: "log" },
          { key: "mix", name: "Mix", min: 0, max: 1, default: 1, unit: "amount" },
          { key: "shape", name: "Shape", min: 1, max: 10, default: 1, unit: "", taper: "exp" },
        ],
      }],
    });
    const params = EffectTypeRegistry.get(type)?.parameters ?? [];
    expect(params.map((p) => [p.key, p.taper])).toEqual([
      ["frequency", "log"],
      ["mix", undefined],
      ["shape", undefined],
    ]);
  });
});
