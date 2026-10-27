/**
 * The EQ bands' parameter keys, labels and limits, as ParametricEQEffect and
 * GraphicEQEffect take them. A leaf module: the practice tool reads these while
 * the state modules load, before the curve's own imports (eqCurve.ts) could be.
 */
/** Canonical param keys for each EQ band (matches ParametricEQEffect). */
export const EQ_BAND_KEYS: ReadonlyArray<{ gain: string; freq: string; q: string | null; shape?: string }> = [
  { gain: "lowGain", freq: "lowFreq", q: "lowQ", shape: "lowShape" },
  { gain: "lowMidGain", freq: "lowMidFreq", q: "lowMidQ" },
  { gain: "highMidGain", freq: "highMidFreq", q: "highMidQ" },
  { gain: "highGain", freq: "highFreq", q: "highQ", shape: "highShape" },
];

export const EQ_BAND_LABELS: ReadonlyArray<string> = ["Low", "Low Mid", "High Mid", "High"];

/** Each band's limits as ParametricEQEffect clamps them: the curve draws what the engine plays. */
export const EQ_BAND_RANGES: ReadonlyArray<{
  freqMin: number; freqMax: number;
  hasQ: boolean; qMin: number; qMax: number; qDefault: number;
  shelfType: 'low' | 'high' | undefined;
}> = [
  { freqMin: 20, freqMax: 500, hasQ: true, qMin: 0.1, qMax: 10, qDefault: 0.707, shelfType: 'low' },
  { freqMin: 100, freqMax: 2000, hasQ: true, qMin: 0.1, qMax: 10, qDefault: 1.0, shelfType: undefined },
  { freqMin: 500, freqMax: 8000, hasQ: true, qMin: 0.1, qMax: 10, qDefault: 1.0, shelfType: undefined },
  { freqMin: 2000, freqMax: 16000, hasQ: true, qMin: 0.1, qMax: 10, qDefault: 0.707, shelfType: 'high' },
];

/** The gain ParametricEQEffect clamps every band to. */
export const EQ_BAND_GAIN_DB = 12;

export const EQ_FREQ_DEFAULTS: ReadonlyArray<number> = [100, 400, 2000, 8000];

export const GRAPHIC_EQ_FREQUENCIES = [31.25, 62.5, 125, 250, 500, 1000, 2000, 4000, 8000, 16000] as const;
