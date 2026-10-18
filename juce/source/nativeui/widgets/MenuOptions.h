#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace soundshed::nano
{
/// The options every Nano menu opens with: rows sized for touch, placed by `target` when there is
/// one and at the mouse otherwise.
///
/// A menu's actions capture the control that opened it, and that control can go while the menu
/// is up: a footswitch that changes the preset rebuilds the effect page under an open value menu.
/// JUCE keeps the menu open then (both of its references to the target go null together), so the
/// deletion check is what stops a pick from running its action on a freed control.
[[nodiscard]] inline juce::PopupMenu::Options menuOptions (juce::Component* target, bool touch)
{
    const auto options = juce::PopupMenu::Options().withStandardItemHeight (touch ? 40 : 28);

    if (target == nullptr)
        return options.withMousePosition();

    return options.withTargetComponent (target).withDeletionCheck (*target);
}
} // namespace soundshed::nano
