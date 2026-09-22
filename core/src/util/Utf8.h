#pragma once

// Utf8.h — decoding UTF-8 that did not come from us.
//
// Text read raw from a file, a device or a legacy code page is not guaranteed to
// be UTF-8, and nlohmann::json refuses to serialise a string that is not: dump()
// throws. Anything like that has to pass through here before it goes into JSON.

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace guitarfx::util
{
inline constexpr char32_t kReplacementCharacter = 0xFFFD;

/// Decodes the code point starting at utf8[index] and moves `index` past it. A malformed,
/// truncated, overlong or surrogate sequence yields U+FFFD and moves on by one byte, so
/// decoding picks up again at the next lead byte.
[[nodiscard]] inline char32_t DecodeUtf8(std::string_view utf8, std::size_t& index)
{
    const auto lead = static_cast<unsigned char>(utf8[index]);

    if (lead < 0x80)
    {
        ++index;
        return lead;
    }

    std::size_t length = 0;
    char32_t codePoint = 0;

    if ((lead & 0xE0) == 0xC0)
    {
        length = 2;
        codePoint = lead & 0x1F;
    }
    else if ((lead & 0xF0) == 0xE0)
    {
        length = 3;
        codePoint = lead & 0x0F;
    }
    else if ((lead & 0xF8) == 0xF0)
    {
        length = 4;
        codePoint = lead & 0x07;
    }
    else
    {
        ++index;
        return kReplacementCharacter;
    }

    if (index + length > utf8.size())
    {
        ++index;
        return kReplacementCharacter;
    }

    for (std::size_t offset = 1; offset < length; ++offset)
    {
        const auto continuation = static_cast<unsigned char>(utf8[index + offset]);

        if ((continuation & 0xC0) != 0x80)
        {
            ++index;
            return kReplacementCharacter;
        }

        codePoint = (codePoint << 6) | (continuation & 0x3F);
    }

    static constexpr std::array<char32_t, 5> kSmallestForLength = {0, 0, 0x80, 0x800, 0x10000};

    if (codePoint < kSmallestForLength[length] || codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF))
    {
        ++index;
        return kReplacementCharacter;
    }

    index += length;
    return codePoint;
}

/// `text` with each byte that does not start a valid sequence replaced by U+FFFD, so the
/// result is always valid UTF-8. Valid text comes back unchanged.
[[nodiscard]] inline std::string ReplaceInvalidUtf8(std::string_view text)
{
    std::string out;
    out.reserve(text.size());

    for (std::size_t index = 0; index < text.size();)
    {
        const std::size_t start = index;

        // A real U+FFFD in the input is three bytes long; one byte consumed means a bad byte.
        if (DecodeUtf8(text, index) == kReplacementCharacter && index - start == 1)
        {
            out += "\xEF\xBF\xBD";
        }
        else
        {
            out.append(text.substr(start, index - start));
        }
    }

    return out;
}
} // namespace guitarfx::util
