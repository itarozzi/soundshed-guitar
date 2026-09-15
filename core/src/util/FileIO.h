#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace guitarfx::util
{
[[nodiscard]] std::vector<std::uint8_t> ReadFileBytes(const std::filesystem::path& path);

/// Writes `data` to `target` so that anyone opening `target` sees either what was
/// there before (or no file) or every one of the new bytes — never a partial write.
///
/// The bytes go to a uniquely named sibling in the same directory, which is then
/// renamed over `target`. Several plugin instances can therefore extract the same
/// file at once while others parse it.
///
/// A `target` that already holds exactly these bytes — there beforehand, or put
/// there by another instance meanwhile — is left alone rather than replaced: on
/// Windows a reader opening a file at the instant it is replaced fails to open it.
/// A reader holding `target` open blocks a replace there, so it is retried for a
/// short while.
///
/// On failure `target` is left untouched, the temporary file is removed, and
/// `error` (when given) says what went wrong.
[[nodiscard]] bool WriteFileAtomic(const std::filesystem::path& target, std::span<const std::uint8_t> data,
                                   std::string* error = nullptr);

/// WriteFileAtomic for text, written byte for byte. There is no newline
/// translation, so a file written on Windows is identical to one written elsewhere.
[[nodiscard]] bool WriteTextFileAtomic(const std::filesystem::path& target, std::string_view text,
                                       std::string* error = nullptr);

/// True when `path` is a file holding exactly `data`. A size mismatch is decided
/// by a stat, without reading the file.
[[nodiscard]] bool FileContentEquals(const std::filesystem::path& path, std::span<const std::uint8_t> data);
} // namespace guitarfx::util
