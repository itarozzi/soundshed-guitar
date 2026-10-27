/**
 * UpdateCheck.cpp - version comparison for the update check (see UpdateCheck.h).
 */

#include "uiclient/UpdateCheck.h"

#include <cctype>

namespace guitarfx::uiclient
{
std::optional<std::array<int, 3>> ParseVersion(const std::string& version)
{
    // As updateCheck.ts's toNumericSemver: leading non-digits skipped, then up to three numbers
    // joined by dots, then optionally a -prerelease or +build suffix and nothing else.
    std::size_t i = 0;
    const auto n = version.size();
    while (i < n && std::isspace(static_cast<unsigned char>(version[i])))
        ++i;
    while (i < n && !std::isdigit(static_cast<unsigned char>(version[i])))
        ++i;
    if (i == n)
        return std::nullopt;

    std::array<int, 3> parts{0, 0, 0};
    for (int part = 0; part < 3; ++part)
    {
        if (i >= n || !std::isdigit(static_cast<unsigned char>(version[i])))
        {
            if (part == 0)
                return std::nullopt;
            break;
        }
        long value = 0;
        while (i < n && std::isdigit(static_cast<unsigned char>(version[i])))
        {
            value = value * 10 + (version[i] - '0');
            if (value > 1000000000L)
                return std::nullopt;
            ++i;
        }
        parts[static_cast<std::size_t>(part)] = static_cast<int>(value);
        if (part < 2 && i < n && version[i] == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(version[i + 1])))
            ++i;
        else
            break;
    }

    // What is left must be a suffix or trailing space.
    if (i < n && version[i] != '-' && version[i] != '+')
    {
        while (i < n && std::isspace(static_cast<unsigned char>(version[i])))
            ++i;
        if (i < n)
            return std::nullopt;
    }
    return parts;
}

bool IsVersionNewer(const std::string& candidate, const std::string& current)
{
    const auto a = ParseVersion(candidate);
    const auto b = ParseVersion(current);
    return a && b && *a > *b;
}
} // namespace guitarfx::uiclient
