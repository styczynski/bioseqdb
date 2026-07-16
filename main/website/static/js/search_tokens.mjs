// Shared search-token normalizer (§12.5). ONE small algorithm, implemented
// identically here and in main/codegen/website/search.cppm; a golden corpus
// consumed by both proves they agree. Operating on UTF-8 BYTES (not UTF-16 code
// units) is what makes the two implementations byte-for-byte identical:
//
//   - validate/consume UTF-8 bytes;
//   - map ASCII A-Z (0x41-0x5A) to a-z by adding 0x20;
//   - split on every ASCII byte that is not [0-9A-Za-z] (whitespace/punctuation);
//   - preserve every non-ASCII byte (>= 0x80) unchanged, so accented / non-Latin
//     text stays case-sensitive and composed != decomposed; and
//   - join the surviving tokens with a single ASCII space.
//
// Never call String.prototype.toLowerCase: it is locale/Unicode aware and would
// fold Straße==STRASSE and dotted/dotless I, diverging from the C++ side.

const encoder = new TextEncoder();
const decoder = new TextDecoder("utf-8", { fatal: false });

function isAsciiDigit(byte) {
  return byte >= 0x30 && byte <= 0x39;
}
function isAsciiLower(byte) {
  return byte >= 0x61 && byte <= 0x7a;
}
function isAsciiUpper(byte) {
  return byte >= 0x41 && byte <= 0x5a;
}

/**
 * Normalize free text into a single space-joined token string.
 * @param {string} text
 * @returns {string}
 */
export function normalizeSearchText(text) {
  if (typeof text !== "string" || text.length === 0) return "";
  const bytes = encoder.encode(text);
  const out = [];
  let tokenStarted = false;
  for (let i = 0; i < bytes.length; i += 1) {
    const byte = bytes[i];
    if (byte < 0x80) {
      if (isAsciiUpper(byte)) {
        out.push(byte + 0x20);
        tokenStarted = true;
      } else if (isAsciiDigit(byte) || isAsciiLower(byte)) {
        out.push(byte);
        tokenStarted = true;
      } else if (tokenStarted) {
        // ASCII whitespace/punctuation ends the current token.
        out.push(0x20);
        tokenStarted = false;
      }
    } else {
      // Non-ASCII byte: preserved verbatim, part of the current token.
      out.push(byte);
      tokenStarted = true;
    }
  }
  // Trim a single trailing separator if the string ended on punctuation.
  if (out.length > 0 && out[out.length - 1] === 0x20) out.pop();
  return decoder.decode(new Uint8Array(out));
}

/**
 * Split normalized text into its individual tokens.
 * @param {string} text
 * @returns {string[]}
 */
export function tokenize(text) {
  const normalized = normalizeSearchText(text);
  if (normalized.length === 0) return [];
  return normalized.split(" ");
}
