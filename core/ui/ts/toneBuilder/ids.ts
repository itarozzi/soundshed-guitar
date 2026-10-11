/**
 * Deterministic ids for built presets: a synchronous SHA-1 and the UUIDv5 built on it.
 *
 * A proposal rebuilt from the same spec must get the same preset, scene and node ids, so a
 * second build updates the saved preset instead of adding a copy. WebCrypto's digest is async,
 * and the builder has to run synchronously wherever it is used (the app, Node scripts, the API
 * worker), so the hash is implemented here.
 */

/** Namespace for every id the builder makes. Shared with the content pipeline's builds. */
export const PRESET_ID_NAMESPACE = "8c1d3e52-6a4b-4f0e-9b7a-2d5c1e9f4a63";

const utf8 = new TextEncoder();

function rotl(value: number, bits: number): number {
  return (value << bits) | (value >>> (32 - bits));
}

/** SHA-1 of the bytes, as 20 bytes. */
export function sha1(bytes: Uint8Array): Uint8Array {
  const bitLength = bytes.length * 8;
  const paddedLength = (((bytes.length + 8) >> 6) + 1) << 6;
  const data = new Uint8Array(paddedLength);
  data.set(bytes);
  data[bytes.length] = 0x80;
  const view = new DataView(data.buffer);
  // The length goes in the last 8 bytes, big-endian. Inputs here are far below 2^32 bits.
  view.setUint32(paddedLength - 8, Math.floor(bitLength / 0x100000000));
  view.setUint32(paddedLength - 4, bitLength >>> 0);

  let h0 = 0x67452301;
  let h1 = 0xefcdab89;
  let h2 = 0x98badcfe;
  let h3 = 0x10325476;
  let h4 = 0xc3d2e1f0;
  const w = new Uint32Array(80);
  for (let offset = 0; offset < paddedLength; offset += 64) {
    for (let i = 0; i < 16; i += 1) {
      w[i] = view.getUint32(offset + i * 4);
    }
    for (let i = 16; i < 80; i += 1) {
      w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    let a = h0;
    let b = h1;
    let c = h2;
    let d = h3;
    let e = h4;
    for (let i = 0; i < 80; i += 1) {
      let f: number;
      let k: number;
      if (i < 20) {
        f = (b & c) | (~b & d);
        k = 0x5a827999;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ed9eba1;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8f1bbcdc;
      } else {
        f = b ^ c ^ d;
        k = 0xca62c1d6;
      }
      const temp = (rotl(a, 5) + f + e + k + w[i]) >>> 0;
      e = d;
      d = c;
      c = rotl(b, 30) >>> 0;
      b = a;
      a = temp;
    }
    h0 = (h0 + a) >>> 0;
    h1 = (h1 + b) >>> 0;
    h2 = (h2 + c) >>> 0;
    h3 = (h3 + d) >>> 0;
    h4 = (h4 + e) >>> 0;
  }

  const out = new Uint8Array(20);
  const outView = new DataView(out.buffer);
  [h0, h1, h2, h3, h4].forEach((h, i) => outView.setUint32(i * 4, h));
  return out;
}

export function toHex(bytes: Uint8Array): string {
  let hex = "";
  for (const byte of bytes) {
    hex += byte.toString(16).padStart(2, "0");
  }
  return hex;
}

/** SHA-1 of a string's UTF-8 bytes, as hex. */
export function sha1Hex(text: string): string {
  return toHex(sha1(utf8.encode(text)));
}

/** RFC 4122 version-5 UUID of `name` in `namespace`. */
export function uuidv5(name: string, namespace: string = PRESET_ID_NAMESPACE): string {
  const nsHex = namespace.replace(/-/g, "");
  const ns = new Uint8Array(16);
  for (let i = 0; i < 16; i += 1) {
    ns[i] = Number.parseInt(nsHex.slice(i * 2, i * 2 + 2), 16);
  }
  const nameBytes = utf8.encode(name);
  const input = new Uint8Array(16 + nameBytes.length);
  input.set(ns);
  input.set(nameBytes, 16);
  const bytes = sha1(input).slice(0, 16);
  bytes[6] = (bytes[6] & 0x0f) | 0x50;
  bytes[8] = (bytes[8] & 0x3f) | 0x80;
  const h = toHex(bytes);
  return `${h.slice(0, 8)}-${h.slice(8, 12)}-${h.slice(12, 16)}-${h.slice(16, 20)}-${h.slice(20)}`;
}
