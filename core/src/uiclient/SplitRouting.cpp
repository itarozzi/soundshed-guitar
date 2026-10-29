/**
 * SplitRouting.cpp - A splitter's routing in words (see SplitRouting.h).
 */

#include "uiclient/SplitRouting.h"

#include "dsp/effects/SplitterSupport.h"

#include <algorithm>
#include <cmath>

namespace guitarfx::uiclient
{
namespace
{
namespace sp = guitarfx::splitter;

/// "; branches 3 and 4 get everything" (or "both"), for the modes that route only the first two.
std::string RestGet(int branches, const std::string& what)
{
    if (branches <= 2)
    {
        return "";
    }

    return (branches == 3 ? "; branch 3 gets " : "; branches 3 and 4 get ") + what;
}
} // namespace

int CountSplitBranches(const SignalGraph& graph, const std::string& splitterId)
{
    return static_cast<int>(std::count_if(graph.edges.begin(), graph.edges.end(),
                                          [&](const GraphEdge& e) { return e.from == splitterId; }));
}

bool IsSplitterParamInUse(const std::string& key, int branches)
{
    if (key == sp::kParams[sp::kSplitNote2].id)
    {
        return branches >= 3;
    }

    if (key == sp::kParams[sp::kSplitNote3].id)
    {
        return branches >= 4;
    }

    return true;
}

std::string DescribeSplitRouting(const GraphNode& splitter, int branches)
{
    const int count = std::clamp(branches, 1, sp::kMaxBranches);
    const auto& modeSpec = sp::kParams[sp::kMode];
    const auto it = splitter.params.find(modeSpec.id);
    const auto mode = static_cast<sp::Mode>(
        std::lround(NormaliseParamValue(modeSpec, it != splitter.params.end() ? it->second : modeSpec.defaultValue)));
    const std::string last = "branch " + std::to_string(count);

    switch (mode)
    {
    case sp::Mode::Blend:
        return "Position moves the signal from branch 1 to " + last;
    case sp::Mode::Switch:
        return "Only the chosen branch plays; the others ring out";
    case sp::Mode::Dynamics:
        return "Soft playing to branch 1, hard to branch 2" + RestGet(count, "everything");
    case sp::Mode::Pitch:
        return count == 2 ? "Notes below the split note to branch 1, the rest to branch 2"
                          : "Notes to branches 1 to " + std::to_string(count) + " by range, at the split notes";
    case sp::Mode::Stereo:
        return "Left to branch 1, right to branch 2" + RestGet(count, "both");
    default:
        return "";
    }
}
} // namespace guitarfx::uiclient
