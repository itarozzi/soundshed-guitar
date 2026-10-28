#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <optional>

namespace soundshed
{
/// The colours the page asks the standalone window's native title bar to take, sent as
/// a "setWindowChrome" message whenever the theme changes. Each is "#rrggbb".
struct WindowChromeColours
{
    juce::Colour caption;
    juce::Colour text;
    juce::Colour border;

    /// From the message, or the copy kept in the window state file. Nothing if any
    /// colour is missing or not "#rrggbb".
    static std::optional<WindowChromeColours> fromVar(const juce::var& value);
    [[nodiscard]] juce::var toVar() const;

    bool operator==(const WindowChromeColours&) const = default;
};

/// Recolours the window's native title bar. Windows 11 takes all three colours; Windows
/// 10 can only switch its caption between light and dark, which follows the caption's
/// brightness. Elsewhere this does nothing: macOS and Linux keep the system's bar, and
/// Android has none. True if the caption took the colour itself, which is when the page
/// drops its own title ("windowChromeState").
bool applyWindowChrome(juce::Component& window, const WindowChromeColours& colours);
} // namespace soundshed
