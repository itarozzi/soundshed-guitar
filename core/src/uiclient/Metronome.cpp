/**
 * Metronome.cpp - the meter catalogue and grouping helpers (see Metronome.h).
 */

#include "uiclient/Metronome.h"

#include <algorithm>
#include <cctype>
#include <numeric>

namespace guitarfx::uiclient
{
namespace
{
constexpr int kMaxBeatsPerBar = 16;

int ClampBeats(int beats)
{
    return std::clamp(beats, 1, kMaxBeatsPerBar);
}

TimeSignatureOption Meter(int num, int den, const char* grouping, bool compound)
{
    const std::string g = grouping;
    return {g.empty() ? std::to_string(num) + "/" + std::to_string(den) : std::to_string(num) + "/" + std::to_string(den) + ":" + g,
            DescribeTimeSignature(num, den, g), num, den, g, compound};
}
} // namespace

const std::vector<TimeSignatureOption>& TimeSignatures()
{
    static const std::vector<TimeSignatureOption> meters = {
        Meter(2, 4, "", false),       Meter(3, 4, "", false),       Meter(4, 4, "", false),       Meter(5, 4, "", false),
        Meter(6, 4, "", false),       Meter(7, 4, "", false),       Meter(8, 4, "", false),       Meter(9, 4, "", false),
        Meter(10, 4, "", false),      Meter(11, 4, "", false),      Meter(12, 4, "", false),      Meter(13, 4, "", false),
        Meter(3, 8, "", true),        Meter(6, 8, "", true),        Meter(9, 8, "", true),        Meter(12, 8, "", true),
        Meter(5, 8, "3+2", true),     Meter(5, 8, "2+3", true),     Meter(7, 8, "3+2+2", true),   Meter(7, 8, "2+3+2", true),
        Meter(7, 8, "2+2+3", true),
    };
    return meters;
}

std::string DescribeTimeSignature(int num, int den, const std::string& grouping)
{
    const auto base = std::to_string(num) + "/" + std::to_string(den);
    return grouping.empty() ? base : base + " (" + grouping + ")";
}

std::vector<int> ParseGrouping(const std::string& grouping, int beatsPerBar)
{
    std::vector<int> groups;
    std::string part;
    const auto flush = [&]() {
        if (part.empty())
            return true;
        if (!std::all_of(part.begin(), part.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }))
            return false;
        const int value = std::stoi(part);
        part.clear();
        if (value <= 0)
            return false;
        groups.push_back(value);
        return true;
    };
    for (const char c : grouping)
    {
        if (c == '+' || c == ',' || c == ' ' || c == '-')
        {
            if (!flush())
                return {};
        }
        else
        {
            part += c;
        }
    }
    if (!flush() || groups.empty())
        return {};
    return std::accumulate(groups.begin(), groups.end(), 0) == ClampBeats(beatsPerBar) ? groups : std::vector<int>{};
}

std::vector<bool> GroupHeads(int num, int den, const std::string& grouping)
{
    const int beats = ClampBeats(num);
    auto groups = ParseGrouping(grouping, beats);
    if (groups.empty() && den == 8 && beats > 3 && beats % 3 == 0)
        groups.assign(static_cast<std::size_t>(beats / 3), 3);
    std::vector<bool> heads(static_cast<std::size_t>(beats), false);
    heads[0] = true;
    int at = 0;
    for (const int g : groups)
    {
        if (at < beats)
            heads[static_cast<std::size_t>(at)] = true;
        at += g;
    }
    return heads;
}
} // namespace guitarfx::uiclient
