/**
 * An enum parameter's label for a value.
 *
 * Labels are listed in value order from the parameter's minimum, one per step, as the engine
 * declares them (EffectParamSpec.h: "an enum's choices, in value order"). Indexing them by the
 * value itself only works for an enum that starts at 0: the Auto Arpeggiator's Steps runs 2 to 8,
 * and showed "6" at 4. The Nano UI does the same in EnumLabelIndex (uiclient/ParamFormat.h).
 */
export function enumLabel(value: number, labels: readonly string[], min = 0, step?: number): string | undefined {
  const index = Math.round((value - min) / (step !== undefined && step > 0 ? step : 1));
  return index >= 0 ? labels[index] : undefined;
}
