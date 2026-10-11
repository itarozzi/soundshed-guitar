#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace guitarfx::util
{
[[nodiscard]] std::string SanitizePathSegment(const std::string& raw, bool allowDots);
[[nodiscard]] std::filesystem::path SanitizeSubfolderPath(const std::string& raw);
[[nodiscard]] std::string SanitizeFilename(const std::string& raw);

/// A sanitized file name (ASCII) cut to at most `maxLength` characters, keeping its extension:
/// the stem is shortened. Remote providers name files after titles that, under the settings
/// folder, can pass Windows' 260-character path limit.
[[nodiscard]] std::string LimitFilenameLength(const std::string& name, std::size_t maxLength);

/// A sanitized subfolder path with every segment cut to at most `maxSegmentLength` characters.
[[nodiscard]] std::filesystem::path LimitSubfolderSegments(const std::filesystem::path& subfolder, std::size_t maxSegmentLength);
} // namespace guitarfx::util
