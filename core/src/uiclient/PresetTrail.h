#pragma once

/**
 * PresetTrail.h - The presets loaded, in order, for a preset bar's back and forward.
 *
 * The web UI's preset history (core/ui/ts/presets/history.ts): each preset loaded goes after the
 * current place, dropping anything forward of it; a load of the current entry adds nothing; the
 * oldest falls off past kPresetTrailLimit. Stepping back or forward moves the place without
 * adding to the trail. Unlike the web UI's, a step onto a preset deleted since drops it instead.
 */

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace guitarfx::uiclient
{
inline constexpr std::size_t kPresetTrailLimit = 10;

class PresetTrail
{
  public:
    /// A preset was loaded. One the trail stepped to is already where the place is.
    void Note(const std::string& presetId);

    /// The preset one step back (-1) or forward (+1), with the place moved to it; nullopt at an
    /// end. `exists` says whether a preset is still there: one that is not is dropped, and the
    /// step stops.
    std::optional<std::string> Step(int direction, const std::function<bool(const std::string&)>& exists);

    [[nodiscard]] bool CanGoBack() const
    {
        return mAt > 0 && !mIds.empty();
    }

    [[nodiscard]] bool CanGoForward() const
    {
        return mAt + 1 < mIds.size();
    }

  private:
    std::vector<std::string> mIds;
    std::size_t mAt = 0;
};
} // namespace guitarfx::uiclient
