import { isCompositeEffectType } from "./compositeTypes.js";

/**
 * Which resource slot on a node an exposed resource's picker reads and writes.
 *
 * Effects that declare slots on themselves — the IR cab's A/B pair, a WASM
 * module's blobs — number those slots, and the numbering is not always the
 * declaration order: a WASM module reserves slot 0 for the module itself. Their
 * declared index is the slot to use.
 *
 * A composite is the exception. Its node carries one slot per exposed resource
 * in declaration order — `CompositeEffectProcessor::LoadResources` maps the
 * node's `resources[i]` onto `exposedResources[i]` — while the index in a
 * composite definition means the slot on the *inner* node it forwards to, which
 * is 0 for most of them. Copying that through put both pickers of "Supercharged
 * Neural Amp" (amp model, cab IR) on slot 0: each pick overwrote the other, and
 * the picker that lost rendered the surviving id, of the wrong type, as a
 * missing resource.
 */
export function resolveExposedResourceSlot(
  effectType: string,
  declaredResourceIndex: number | undefined,
  declarationIndex: number,
): number {
  if (isCompositeEffectType(effectType)) {
    return declarationIndex;
  }

  return typeof declaredResourceIndex === "number" ? declaredResourceIndex : declarationIndex;
}
