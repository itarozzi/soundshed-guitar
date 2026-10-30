/**
 * Where a layout image came from: the licence it was shared under and the credit it asks for.
 * Knob artwork shared online often carries a Creative Commons licence that wants its author
 * credited, so the credit travels with the image: the engine keeps it
 * (image-credits.json), an exported layout carries it, and an imported one brings it back. The
 * designer records one in Images in this Layout (layoutAssets.ts).
 */

/** The licences the designer offers; the engine takes the same ids (IsLayoutImageLicense). */
export const LAYOUT_IMAGE_LICENSES: ReadonlyArray<{ id: string; label: string }> = [
  { id: "CC0-1.0", label: "CC0 (public domain)" },
  { id: "CC-BY-4.0", label: "CC BY 4.0" },
  { id: "CC-BY-SA-4.0", label: "CC BY-SA 4.0" },
  { id: "CC-BY-ND-4.0", label: "CC BY-ND 4.0" },
  { id: "CC-BY-NC-4.0", label: "CC BY-NC 4.0" },
  { id: "CC-BY-NC-SA-4.0", label: "CC BY-NC-SA 4.0" },
  { id: "CC-BY-NC-ND-4.0", label: "CC BY-NC-ND 4.0" },
  { id: "own", label: "My own work" },
  { id: "other", label: "Other" },
];

/** The longest credit line the engine keeps, in UTF-8 bytes. */
const MAX_CREDIT_BYTES = 300;

export function isLayoutImageLicense(value: unknown): value is string {
  return LAYOUT_IMAGE_LICENSES.some((license) => license.id === value);
}

export function layoutImageLicenseLabel(license: string | undefined): string {
  return LAYOUT_IMAGE_LICENSES.find((entry) => entry.id === license)?.label ?? "";
}

/** `value` as a credit line the engine keeps: one line, trimmed, cut to 300 bytes. */
export function cleanCredit(value: unknown): string {
  if (typeof value !== "string") return "";
  // Control characters (a pasted newline, say) become spaces.
  let credit = Array.from(value, (ch) => (ch.charCodeAt(0) < 0x20 || ch.charCodeAt(0) === 0x7f ? " " : ch))
    .join("")
    .replace(/ {2,}/g, " ")
    .trim();
  const encoder = new TextEncoder();
  while (encoder.encode(credit).length > MAX_CREDIT_BYTES) credit = credit.slice(0, -1);
  return credit;
}
