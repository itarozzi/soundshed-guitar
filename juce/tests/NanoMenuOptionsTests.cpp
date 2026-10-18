// The options Nano's menus open with (source/nativeui/widgets/MenuOptions.h).
//
// A menu's actions capture the control that opened it, and a rebuild can destroy that control
// while the menu is up: a footswitch changes the preset while a parameter's value menu is open,
// and the effect page builds new controls. JUCE keeps the menu open then, so the options have to
// tell it the target has gone, or the pick runs its action on a freed control.

#include "nativeui/widgets/MenuOptions.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <iostream>
#include <memory>

namespace
{
    int failures = 0;

    void fail (const char* message)
    {
        std::cerr << "[NanoMenuOptionsTests] " << message << std::endl;
        ++failures;
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    {
        auto target = std::make_unique<juce::Component>();
        const auto options = soundshed::nano::menuOptions (target.get(), false);

        if (options.getTargetComponent() != target.get())
            fail ("a menu with a target is placed by it");

        if (options.hasWatchedComponentBeenDeleted())
            fail ("the target is alive, so nothing has been deleted");

        // The control goes while the menu is up.
        target.reset();

        if (! options.hasWatchedComponentBeenDeleted())
            fail ("the target was deleted while the menu was up: a pick must not run its action");
    }

    {
        const auto options = soundshed::nano::menuOptions (nullptr, true);

        if (options.getTargetComponent() != nullptr || options.hasWatchedComponentBeenDeleted())
            fail ("a menu with no target is placed at the mouse and watches nothing");
    }

    if (failures == 0)
        std::cout << "[NanoMenuOptionsTests] passed" << std::endl;

    return failures == 0 ? 0 : 1;
}
