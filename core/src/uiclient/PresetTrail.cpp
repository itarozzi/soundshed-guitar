#include "uiclient/PresetTrail.h"

namespace guitarfx::uiclient
{
void PresetTrail::Note(const std::string& presetId)
{
    if (presetId.empty() || (mAt < mIds.size() && mIds[mAt] == presetId))
        return;

    if (!mIds.empty())
        mIds.resize(mAt + 1);
    mIds.push_back(presetId);
    if (mIds.size() > kPresetTrailLimit)
        mIds.erase(mIds.begin(), mIds.begin() + static_cast<std::ptrdiff_t>(mIds.size() - kPresetTrailLimit));
    mAt = mIds.size() - 1;
}

std::optional<std::string> PresetTrail::Step(int direction, const std::function<bool(const std::string&)>& exists)
{
    const auto target = static_cast<std::ptrdiff_t>(mAt) + direction;
    if (target < 0 || target >= static_cast<std::ptrdiff_t>(mIds.size()))
        return std::nullopt;

    const auto id = mIds[static_cast<std::size_t>(target)];
    if (exists && !exists(id))
    {
        mIds.erase(mIds.begin() + target);
        if (mAt >= mIds.size())
            mAt = mIds.empty() ? 0 : mIds.size() - 1;
        else if (target < static_cast<std::ptrdiff_t>(mAt))
            --mAt;
        return std::nullopt;
    }
    mAt = static_cast<std::size_t>(target);
    return id;
}
} // namespace guitarfx::uiclient
