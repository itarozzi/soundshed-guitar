#pragma once

/**
 * Sha256.h - SHA-256 (FIPS 180-4) of a byte buffer, as lowercase hex.
 *
 * The engine's own content hash is FNV-1a (models/ModelHasher.h). This one exists for the
 * preset archive format the web UI defined, which names and identifies each model or IR it
 * carries by the SHA-256 of its bytes (crypto.subtle.digest in core/ui/ts/utils.ts).
 */

#include <cstdint>
#include <span>
#include <string>

namespace guitarfx::util
{
/// 64 lowercase hex digits.
[[nodiscard]] std::string Sha256Hex(std::span<const std::uint8_t> bytes);
} // namespace guitarfx::util
