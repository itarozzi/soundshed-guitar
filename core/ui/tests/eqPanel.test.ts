import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { EQ_BAND_KEYS, EQ_BAND_RANGES, EQ_FREQ_DEFAULTS, buildEqBandConfigsFromParams, graphicEqResponseQuery, parametricEqResponseQuery } from "../ts/eqCurve.js";
import { EqPanel, type EqPanelBinding } from "../ts/eqPanel.js";

/** A binding backed by a plain dict, recording what the panel asks of it. */
function makeBinding(initial: Record<string, number> = {}) {
  const params: Record<string, number> = { ...initial };
  let enabled = false;
  const writes: Array<{ changed: Record<string, number>; commit: boolean }> = [];
  const binding: EqPanelBinding = {
    label: "test eq",
    readParams: () => params,
    writeParams: (changed, commit) => {
      Object.assign(params, changed);
      writes.push({ changed, commit });
    },
    readEnabled: () => enabled,
    writeEnabled: (next) => { enabled = next; },
  };
  return { binding, params, writes, isEnabled: () => enabled, setEnabled: (v: boolean) => { enabled = v; } };
}

function mountHost(): { bandsHost: HTMLElement; toggle: HTMLInputElement; resetButton: HTMLElement } {
  document.body.innerHTML = `
    <div id="bands"></div>
    <input type="checkbox" id="toggle" />
    <button id="reset"></button>`;
  return {
    bandsHost: document.getElementById("bands")!,
    toggle: document.getElementById("toggle") as HTMLInputElement,
    resetButton: document.getElementById("reset")!,
  };
}

beforeEach(() => {
  document.body.innerHTML = "";
});

describe("EqPanel rendering", () => {
  it("emits one band row per band in the shared topology", () => {
    const { bandsHost, toggle, resetButton } = mountHost();
    const { binding } = makeBinding();
    new EqPanel({ bandsHost, canvas: null, toggle, resetButton, idPrefix: "test" }, binding);

    expect(bandsHost.querySelectorAll(".eq-band")).toHaveLength(EQ_BAND_KEYS.length);
    // Every band offers gain and freq; Q only where the topology declares one.
    const expectedKnobs = EQ_BAND_KEYS.reduce((n, keys) => n + (keys.q ? 3 : 2), 0);
    expect(bandsHost.querySelectorAll(".knob")).toHaveLength(expectedKnobs);
  });

  it("marks itself as an .eq-bands host so the shared band styling applies", () => {
    const { bandsHost, toggle, resetButton } = mountHost();
    new EqPanel({ bandsHost, canvas: null, toggle, resetButton, idPrefix: "test" }, makeBinding().binding);
    expect(bandsHost.classList.contains("eq-bands")).toBe(true);
  });

  it("namespaces its knob ids, so two panels on one page never collide", () => {
    document.body.innerHTML = `<div id="a"></div><div id="b"></div>`;
    const hostA = document.getElementById("a")!;
    const hostB = document.getElementById("b")!;
    new EqPanel({ bandsHost: hostA, canvas: null, idPrefix: "global_eq" }, makeBinding().binding);
    new EqPanel({ bandsHost: hostB, canvas: null, idPrefix: "practice_tool_eq" }, makeBinding().binding);

    const idsOf = (host: HTMLElement) =>
      Array.from(host.querySelectorAll(".knob")).map((el) => (el as HTMLElement).dataset.param);
    const a = idsOf(hostA);
    const b = idsOf(hostB);

    expect(a.every((id) => id?.startsWith("global_eq_"))).toBe(true);
    expect(b.every((id) => id?.startsWith("practice_tool_eq_"))).toBe(true);
    expect(a.filter((id) => b.includes(id))).toEqual([]);
  });

  it("reflects the binding's enabled flag onto the toggle and the dim class", () => {
    const { bandsHost, toggle, resetButton } = mountHost();
    const backing = makeBinding();
    const panel = new EqPanel({ bandsHost, canvas: null, toggle, resetButton, idPrefix: "test" }, backing.binding);

    expect(toggle.checked).toBe(false);
    expect(bandsHost.classList.contains("is-eq-enabled")).toBe(false);

    backing.setEnabled(true);
    panel.render();
    expect(toggle.checked).toBe(true);
    expect(bandsHost.classList.contains("is-eq-enabled")).toBe(true);
  });

  it("never writes back to the binding while syncing from it", () => {
    // Pushing values into the knobs fires their own change handlers; without
    // the guard, every render would echo the whole curve back to the engine.
    const { bandsHost, toggle, resetButton } = mountHost();
    const backing = makeBinding({ lowGain: 4, highGain: -3 });
    const panel = new EqPanel({ bandsHost, canvas: null, toggle, resetButton, idPrefix: "test" }, backing.binding);

    backing.writes.length = 0;
    panel.render();
    panel.render();
    expect(backing.writes).toEqual([]);
  });
});

describe("EqPanel wiring", () => {
  it("selects shapes independently, reflects loaded values and resets to shelves", () => {
    const { bandsHost } = mountHost();
    const backing = makeBinding({ lowShape: 1 });
    const panel = new EqPanel({ bandsHost, canvas: null, idPrefix: "test" }, backing.binding);
    const low = bandsHost.querySelector<HTMLSelectElement>('[data-eq-shape="lowShape"]')!;
    const high = bandsHost.querySelector<HTMLSelectElement>('[data-eq-shape="highShape"]')!;
    expect(low.value).toBe("1");
    expect(high.value).toBe("0");
    high.value = "1";
    high.dispatchEvent(new Event("change"));
    expect(backing.writes.at(-1)).toEqual({ changed: { highShape: 1 }, commit: true });
    expect(backing.params.lowShape).toBe(1);
    panel.resetToDefaults();
    expect(low.value).toBe("0");
    expect(high.value).toBe("0");
  });

  it("uses whole-Hz steps in frequency inline editors", () => {
    const { bandsHost } = mountHost();
    new EqPanel({ bandsHost, canvas: null, idPrefix: "test" }, makeBinding().binding);
    bandsHost.querySelector<HTMLElement>('[data-knob-value="test_0_freq"]')!
      .dispatchEvent(new MouseEvent("dblclick", { bubbles: true }));
    expect(bandsHost.querySelector<HTMLInputElement>(".knob-inline-editor")!.step).toBe("1");
  });

  it("routes the toggle through the binding and tells the host", () => {
    const { bandsHost, toggle, resetButton } = mountHost();
    const backing = makeBinding();
    const onChanged = vi.fn();
    new EqPanel({ bandsHost, canvas: null, toggle, resetButton, idPrefix: "test", onChanged }, backing.binding);

    toggle.checked = true;
    toggle.dispatchEvent(new Event("change"));

    expect(backing.isEnabled()).toBe(true);
    expect(onChanged).toHaveBeenCalled();
  });

  it("undims the bands the moment the toggle flips, without waiting for a render", () => {
    // A host that only refreshes its own chrome on change would otherwise
    // leave the panel showing a disabled EQ that is actually running.
    const { bandsHost, toggle, resetButton } = mountHost();
    new EqPanel({ bandsHost, canvas: null, toggle, resetButton, idPrefix: "test" }, makeBinding().binding);

    toggle.checked = true;
    toggle.dispatchEvent(new Event("change"));
    expect(bandsHost.classList.contains("is-eq-enabled")).toBe(true);

    toggle.checked = false;
    toggle.dispatchEvent(new Event("change"));
    expect(bandsHost.classList.contains("is-eq-enabled")).toBe(false);
  });

  it("resets every band to the topology's defaults in a single committed write", () => {
    const { bandsHost, toggle, resetButton } = mountHost();
    const backing = makeBinding({ lowGain: 9, lowMidFreq: 1500 });
    new EqPanel({ bandsHost, canvas: null, toggle, resetButton, idPrefix: "test" }, backing.binding);

    backing.writes.length = 0;
    resetButton.dispatchEvent(new Event("click"));

    expect(backing.writes).toHaveLength(1);
    expect(backing.writes[0].commit).toBe(true);

    const changed = backing.writes[0].changed;
    EQ_BAND_KEYS.forEach((keys, index) => {
      expect(changed[keys.gain]).toBe(0);
      expect(changed[keys.freq]).toBe(EQ_FREQ_DEFAULTS[index]);
      if (keys.q) {
        expect(changed[keys.q]).toBe(EQ_BAND_RANGES[index].qDefault);
      }
    });
  });

  it("tolerates a host with no toggle, reset button or canvas", () => {
    document.body.innerHTML = `<div id="bands"></div>`;
    const bandsHost = document.getElementById("bands")!;
    expect(() => {
      const panel = new EqPanel({ bandsHost, canvas: null, idPrefix: "test" }, makeBinding().binding);
      panel.render();
      panel.destroy();
    }).not.toThrow();
  });

  it("does nothing at all without a bands host, rather than throwing", () => {
    expect(() => {
      new EqPanel({ bandsHost: null, canvas: null, idPrefix: "test" }, makeBinding().binding).render();
    }).not.toThrow();
  });
});

describe("EqPanel spectrum", () => {
  let sent: Array<{ type: string; scope?: string; nodeId?: string }>;

  beforeEach(() => {
    sent = [];
    window.IPlugSendMsg = (payload: string) => {
      sent.push(JSON.parse(payload) as { type: string });
    };
  });

  afterEach(() => {
    delete window.IPlugSendMsg;
  });

  it("taps its source's node while the curve is up, and lets go when destroyed", () => {
    vi.useFakeTimers();
    try {
      const { bandsHost } = mountHost();
      const canvas = document.createElement("canvas");
      // jsdom has no IntersectionObserver, so the panel treats its curve as always on screen.
      const panel = new EqPanel(
        { bandsHost, canvas, idPrefix: "test", spectrumSource: () => ({ scope: "post", nodeId: "global_eq" }) },
        makeBinding().binding,
      );
      expect(sent).toEqual([{ type: "setSpectrumWatch", scope: "post", nodeId: "global_eq" }]);

      panel.destroy();
      vi.advanceTimersByTime(1000);
      expect(sent.at(-1)).toEqual({ type: "setSpectrumWatch" });
    } finally {
      vi.useRealTimers();
    }
  });

  it("asks for nothing without a source", () => {
    const { bandsHost } = mountHost();
    new EqPanel({ bandsHost, canvas: document.createElement("canvas"), idPrefix: "test" }, makeBinding().binding);
    expect(sent).toEqual([]);
  });
});

describe("the curve's engine query", () => {
  it("describes the bands as the parametric EQ's parameters, a bell shape as 1", () => {
    const bands = buildEqBandConfigsFromParams({ lowGain: 4, lowMidFreq: 600, highShape: 1, highQ: 2 });
    const query = parametricEqResponseQuery(bands);
    expect(query.effectType).toBe("eq_parametric");
    expect(query.params).toMatchObject({ lowGain: 4, lowShape: 0, lowMidFreq: 600, highShape: 1, highQ: 2 });
    expect(buildEqBandConfigsFromParams(query.params)).toEqual(bands);
  });

  it("passes the graphic EQ's parameters through, leaving out any that are not numbers", () => {
    expect(graphicEqResponseQuery({ bandCount: 5, band1Gain: 3, band2Gain: undefined, band3Gain: Number.NaN }))
      .toEqual({ effectType: "eq_graphic", params: { bandCount: 5, band1Gain: 3 } });
  });
});

describe("parametric EQ curve limits", () => {
  it("defaults to shelves and draws independently selected bells, matching the DSP", () => {
    expect(buildEqBandConfigsFromParams({}).map((band) => band.shelfType))
      .toEqual(["low", undefined, undefined, "high"]);
    expect(buildEqBandConfigsFromParams({ lowShape: 1 }).map((band) => band.shelfType))
      .toEqual([undefined, undefined, undefined, "high"]);
    expect(buildEqBandConfigsFromParams({ highShape: 1 }).map((band) => band.shelfType))
      .toEqual(["low", undefined, undefined, undefined]);
    expect(buildEqBandConfigsFromParams({ lowShape: 1, highShape: 1 }).every((band) => band.shelfType === undefined)).toBe(true);
    expect(buildEqBandConfigsFromParams({ lowShape: NaN, highShape: Infinity }).map((band) => band.shelfType))
      .toEqual(["low", undefined, undefined, "high"]);
  });

  // ParametricEQEffect clamps each band to ±12 dB and to its own frequency range; a curve
  // allowing more drew a +18 that played at +12, and a saved 3 kHz high shelf at 4 kHz.
  it("are the engine's: ±12 dB and its band frequency ranges", () => {
    expect(EQ_BAND_RANGES.map((range) => [range.freqMin, range.freqMax]))
      .toEqual([[20, 500], [100, 2000], [500, 8000], [2000, 16000]]);

    const configs = buildEqBandConfigsFromParams({ lowGain: 18, lowMidFreq: 150, highMidFreq: 600, highFreq: 3000 });
    expect(configs.map((band) => [band.gainMin, band.gainMax])).toEqual([[-12, 12], [-12, 12], [-12, 12], [-12, 12]]);
    expect(configs[0].gainDb).toBe(12);
    expect(configs.slice(1).map((band) => band.freq)).toEqual([150, 600, 3000]);
  });
});
