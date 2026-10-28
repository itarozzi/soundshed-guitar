#include "PathSanitizer.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace
{
std::string ToUpperAscii(std::string value)
{
    for (char& c : value)
    {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }

    return value;
}

bool IsWindowsReservedName(const std::string& name)
{
    if (name.empty())
    {
        return false;
    }

    const auto dotPos = name.find('.');
    const std::string upper = ToUpperAscii(dotPos == std::string::npos ? name : name.substr(0, dotPos));
    static const std::array<const char*, 22> kReserved = {
        "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
        "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    return std::any_of(kReserved.begin(), kReserved.end(), [&](const char* r) { return upper == r; });
}
} // namespace

namespace guitarfx::util
{
std::string SanitizePathSegment(const std::string& raw, bool allowDots)
{
    std::string result;
    result.reserve(raw.size());

    for (unsigned char c : raw)
    {
        if (std::isalnum(c) || c == '-' || c == '_')
        {
            result.push_back(static_cast<char>(c));
        }
        else if (allowDots && c == '.')
        {
            result.push_back('.');
        }
        else if (std::isspace(c))
        {
            result.push_back('_');
        }
    }

    while (!result.empty() && result.front() == '.')
    {
        result.erase(result.begin());
    }

    while (!result.empty() && result.back() == '.')
    {
        result.pop_back();
    }

    if (result.empty() || result == "." || result == "..")
    {
        result = "resource";
    }

    if (IsWindowsReservedName(result))
    {
        result = "_" + result;
    }

    return result;
}

std::filesystem::path SanitizeSubfolderPath(const std::string& raw)
{
    std::filesystem::path result;
    std::string segment;
    auto push = [&]() {
        if (segment.empty())
        {
            return;
        }

        std::string s = SanitizePathSegment(segment, true);

        if (!s.empty() && s != "." && s != "..")
        {
            result /= s;
        }

        segment.clear();
    };

    for (char c : raw)
    {
        if (c == '/' || c == '\\')
        {
            push();
        }
        else
        {
            segment.push_back(c);
        }
    }

    push();
    return result;
}

std::string SanitizeFilename(const std::string& raw)
{
    return SanitizePathSegment(raw, true);
}

std::string LimitFilenameLength(const std::string& name, std::size_t maxLength)
{
    if (name.size() <= maxLength)
    {
        return name;
    }

    // Keep a short extension (".nam", ".wav", ".json"); a long "extension" is part of the title.
    const auto dot = name.rfind('.');
    const std::string extension =
        dot != std::string::npos && dot > 0 && name.size() - dot <= 8 ? name.substr(dot) : std::string{};
    const std::size_t stemLimit = maxLength > extension.size() ? maxLength - extension.size() : 1;
    std::string stem = name.substr(0, std::min(stemLimit, name.size() - extension.size()));

    while (!stem.empty() && (stem.back() == '.' || stem.back() == '-' || stem.back() == '_'))
    {
        stem.pop_back();
    }

    return (stem.empty() ? std::string{"resource"} : stem) + extension;
}

std::string CollapseRepeatedHashPrefix(const std::string& name)
{
    // A SHA-256 in hex, then a dash.
    constexpr std::size_t kPrefixLength = 65;

    const auto isHashPrefixAt = [&](std::size_t offset) {
        if (name.size() < offset + kPrefixLength || name[offset + kPrefixLength - 1] != '-')
        {
            return false;
        }

        return std::all_of(name.begin() + static_cast<std::ptrdiff_t>(offset),
                           name.begin() + static_cast<std::ptrdiff_t>(offset + kPrefixLength - 1),
                           [](unsigned char c) { return std::isxdigit(c) != 0; });
    };
    const auto samePrefixAt = [&](std::size_t offset) {
        for (std::size_t i = 0; i < kPrefixLength; ++i)
        {
            if (std::tolower(static_cast<unsigned char>(name[offset + i])) !=
                std::tolower(static_cast<unsigned char>(name[i])))
            {
                return false;
            }
        }

        return true;
    };

    if (!isHashPrefixAt(0))
    {
        return name;
    }

    std::size_t rest = kPrefixLength;

    while (isHashPrefixAt(rest) && samePrefixAt(rest))
    {
        rest += kPrefixLength;
    }

    return rest == kPrefixLength ? name : name.substr(0, kPrefixLength) + name.substr(rest);
}

std::filesystem::path LimitSubfolderSegments(const std::filesystem::path& subfolder, std::size_t maxSegmentLength)
{
    std::filesystem::path result;

    for (const auto& part : subfolder)
    {
        std::string segment = part.string();

        if (segment.size() > maxSegmentLength)
        {
            segment.resize(maxSegmentLength);

            while (!segment.empty() && (segment.back() == '.' || segment.back() == '-' || segment.back() == '_'))
            {
                segment.pop_back();
            }
        }

        if (!segment.empty())
        {
            result /= segment;
        }
    }

    return result;
}
} // namespace guitarfx::util
