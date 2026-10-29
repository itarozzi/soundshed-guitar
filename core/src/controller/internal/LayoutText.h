#pragma once

/**
 * LayoutText.h - the small text and byte tests LayoutSupport.cpp and LayoutImages.cpp share.
 *
 * In a namespace of their own rather than controller_detail itself: every controller file opens
 * that namespace, and several keep a StringField or LowerAscii of their own.
 */

#include "util/PathEncoding.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace guitarfx::controller_detail::layout_text
{
inline std::string LowerAscii(std::string value)
{
    for (char& c : value)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    return value;
}

inline std::string LowerExtension(const std::filesystem::path& extension)
{
    return LowerAscii(util::PathToUtf8(extension));
}

inline bool StartsWith(const std::vector<std::uint8_t>& bytes, std::size_t offset, std::string_view prefix)
{
    if (bytes.size() < offset + prefix.size())
    {
        return false;
    }

    return std::equal(prefix.begin(), prefix.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                      [](char expected, std::uint8_t actual) { return static_cast<std::uint8_t>(expected) == actual; });
}
} // namespace guitarfx::controller_detail::layout_text
