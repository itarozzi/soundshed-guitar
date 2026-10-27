#pragma once

// WebView - a web page in a native child window over the editor, for the few things Nano Q shows
// that are web content: the Jam tab's YouTube player (through the Soundshed API's wrapper page)
// and the scale explorer. Windows has it (WebView2, WebView_win in WebView.cpp); elsewhere Create
// gives nothing and the Jam tab opens those pages in the browser.
//
// The window sits above everything Elements draws, so its owner places it over an empty area of
// the layout and hides it while a dialog or a drop-down is open. UI thread only.

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace nanoq
{
class WebView
{
public:
    struct Options
    {
        /// Where the browser keeps its cache and cookies.
        std::filesystem::path dataFolder;
        /// Whether a page may be shown in place; anything else goes to `openExternal`.
        std::function<bool(const std::string& url)> allowNavigation;
        /// A link that leaves the page (or opens a new window): the system browser shows it.
        std::function<void(const std::string& url)> openExternal;
    };

    /// A web view in `parentWindow` (the editor's native view), hidden until placed; null when
    /// this platform or machine has none (no WebView2 Runtime installed, say).
    [[nodiscard]] static std::unique_ptr<WebView> Create(void* parentWindow, Options options);

    /// The window's pixels per layout unit before any zoom: its DPI over 96 on Windows, else 1.
    [[nodiscard]] static float PixelScale(void* window);

    virtual ~WebView() = default;

    /// Loads `url`, once the view is ready if it is not yet. The same URL again does nothing.
    virtual void Navigate(const std::string& url) = 0;
    /// Where it sits, in the parent window's client pixels.
    virtual void Place(int x, int y, int width, int height) = 0;
    virtual void Show(bool visible) = 0;
    /// The page is making sound: a hidden player that still plays.
    [[nodiscard]] virtual bool PlayingAudio() const = 0;
    /// The view could not be made (the runtime is missing or failed): show the page elsewhere.
    [[nodiscard]] virtual bool Failed() const = 0;
};
} // namespace nanoq
