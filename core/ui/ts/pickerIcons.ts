/**
 * Pictures for picker choices, by the name an effect gives in a parameter's labelInfo (see
 * enumPicker.ts). Drawn as a set, looking down on the guitar's top: the cover, then the pole
 * pieces that tell one pickup from another at a glance.
 *
 * - Single coil: a narrow rounded bar with one row of six poles.
 * - P-90: a wider "soapbar" with one row of poles.
 * - Humbucker: two coils side by side, a row of poles on each.
 * - Active: a closed cover with no poles showing, and a bolt for the battery.
 * - Other: an outline with no detail.
 * - Custom: an outline with its winding drawn through it, for a pickup built from its parts.
 *
 * Wider than tall (32 x 20), like the pickups themselves. Static markup, so it is safe to assign
 * to innerHTML.
 */

const OPEN =
  '<svg viewBox="0 0 32 20" fill="none" stroke="currentColor" stroke-width="1.4" ' +
  'stroke-linecap="round" stroke-linejoin="round" aria-hidden="true" focusable="false">';
const CLOSE = "</svg>";

/** Six pole pieces across the pickup at height `y`. */
function poles(y: number, radius = 1.15): string {
  return [6.5, 10.3, 14.1, 17.9, 21.7, 25.5]
    .map((x) => `<circle cx="${x}" cy="${y}" r="${radius}" fill="currentColor" stroke="none"/>`)
    .join("");
}

const ICONS: Record<string, string> = {
  "pickup-single": `<rect x="2.5" y="6" width="27" height="8" rx="4"/>${poles(10)}`,
  "pickup-p90": `<rect x="3" y="3.5" width="26" height="13" rx="2.6"/>${poles(10, 1.3)}`,
  "pickup-humbucker":
    `<rect x="2" y="2.5" width="28" height="15" rx="2"/>` +
    `<rect x="3.6" y="4" width="24.8" height="5.2" rx="2.6" stroke-width="1"/>` +
    `<rect x="3.6" y="10.8" width="24.8" height="5.2" rx="2.6" stroke-width="1"/>` +
    `${poles(6.6, 0.95)}${poles(13.4, 0.95)}`,
  "pickup-active":
    `<rect x="2" y="2.5" width="28" height="15" rx="2" fill="currentColor" fill-opacity="0.18"/>` +
    `<path d="M17 4.8l-4.2 5.8h3.6l-1.4 4.6 4.2-5.8h-3.6z" fill="currentColor" stroke="none"/>`,
  "pickup-other": `<rect x="3" y="4" width="26" height="12" rx="3" stroke-dasharray="2.5 2"/>`,
  "pickup-custom":
    `<rect x="2.5" y="4" width="27" height="12" rx="3"/>` +
    `<path d="M5.5 10c1.4-3.2 2.8-3.2 4.2 0s2.8 3.2 4.2 0 2.8-3.2 4.2 0 2.8 3.2 4.2 0 2.8-3.2 4.2 0" stroke-width="1.2"/>`,
};

/** The named picture, or a plain outline for a name this table does not know. */
export function pickerIcon(name: string | undefined): string {
  return `${OPEN}${ICONS[name ?? ""] ?? ICONS["pickup-other"]}${CLOSE}`;
}

export function hasPickerIcon(name: string | undefined): boolean {
  return name !== undefined && name in ICONS;
}
