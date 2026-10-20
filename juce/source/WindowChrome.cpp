#include "WindowChrome.h"

#if JUCE_WINDOWS
    #include <windows.h>
    #include <dwmapi.h>

    #pragma comment(lib, "dwmapi.lib")
#endif

namespace soundshed
{
    namespace
    {
        std::optional<juce::Colour> parseHexColour (const juce::var& value)
        {
            const auto text = value.toString().trim();
            if (text.length() != 7 || !text.startsWithChar ('#') || !text.substring (1).containsOnly ("0123456789abcdefABCDEF"))
                return std::nullopt;

            return juce::Colour (0xff000000 | static_cast<juce::uint32> (text.substring (1).getHexValue32()));
        }

        juce::String toHex (juce::Colour colour)
        {
            return "#" + colour.toDisplayString (false).toLowerCase();
        }
    } // namespace

    std::optional<WindowChromeColours> WindowChromeColours::fromVar (const juce::var& value)
    {
        const auto caption = parseHexColour (value.getProperty ("caption", {}));
        const auto text = parseHexColour (value.getProperty ("text", {}));
        const auto border = parseHexColour (value.getProperty ("border", {}));

        if (!caption || !text || !border)
            return std::nullopt;

        return WindowChromeColours { *caption, *text, *border };
    }

    juce::var WindowChromeColours::toVar() const
    {
        juce::DynamicObject::Ptr object (new juce::DynamicObject());
        object->setProperty ("caption", toHex (caption));
        object->setProperty ("text", toHex (text));
        object->setProperty ("border", toHex (border));
        return juce::var (object.get());
    }

    bool applyWindowChrome (juce::Component& window, const WindowChromeColours& colours)
    {
#if JUCE_WINDOWS
        auto* hwnd = static_cast<HWND> (window.getWindowHandle());
        if (hwnd == nullptr)
            return false;

        // By number rather than by name: the names for these need a Windows 11 SDK.
        constexpr DWORD useImmersiveDarkMode = 20; // Windows 10 20H1 and later
        constexpr DWORD useImmersiveDarkModeBefore20H1 = 19; // Windows 10 1809 to 1909
        constexpr DWORD borderColour = 34; // Windows 11 from here down
        constexpr DWORD captionColour = 35;
        constexpr DWORD textColour = 36;

        // Also sets the caption buttons' glyphs, which take no colour of their own, so it
        // matters on Windows 11 as well.
        const BOOL dark = colours.caption.getPerceivedBrightness() < 0.5f ? TRUE : FALSE;
        if (FAILED (DwmSetWindowAttribute (hwnd, useImmersiveDarkMode, &dark, sizeof (dark))))
            DwmSetWindowAttribute (hwnd, useImmersiveDarkModeBefore20H1, &dark, sizeof (dark));

        // Windows 10 refuses these (E_INVALIDARG), leaving it the light/dark switch above.
        const auto setColour = [hwnd] (DWORD attribute, juce::Colour colour) {
            const COLORREF ref = RGB (colour.getRed(), colour.getGreen(), colour.getBlue());
            return SUCCEEDED (DwmSetWindowAttribute (hwnd, attribute, &ref, sizeof (ref)));
        };

        const auto captionTaken = setColour (captionColour, colours.caption);
        setColour (textColour, colours.text);
        setColour (borderColour, colours.border);

        // Windows 10 repaints a shown caption in its new mode only once the frame changes.
        SetWindowPos (hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        return captionTaken;
#else
        juce::ignoreUnused (window, colours);
        return false;
#endif
    }
} // namespace soundshed
