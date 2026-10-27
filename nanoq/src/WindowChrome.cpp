#include "WindowChrome.h"

#include "Paths.h"

#if defined(_WIN32)
#include <windows.h>
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")
#endif

namespace nanoq
{
#if defined(_WIN32)
namespace
{
/// The standalone's top-level window, or null inside a host (whose window is not ours).
HWND StandaloneWindow(void* viewHandle)
{
    if (viewHandle == nullptr || !RunningAsStandalone())
        return nullptr;
    return GetAncestor(static_cast<HWND>(viewHandle), GA_ROOT);
}

COLORREF ToColorRef(std::uint32_t rgb)
{
    return RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}
} // namespace

void MakeStandaloneWindowResizable(void* viewHandle)
{
    const auto window = StandaloneWindow(viewHandle);
    if (window == nullptr)
        return;
    const auto style = GetWindowLongPtrW(window, GWL_STYLE);
    constexpr LONG_PTR kWanted = WS_THICKFRAME | WS_MAXIMIZEBOX;
    if ((style & kWanted) == kWanted)
        return;
    SetWindowLongPtrW(window, GWL_STYLE, style | kWanted);
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void ThemeStandaloneWindow(void* viewHandle, std::uint32_t caption, std::uint32_t text, std::uint32_t border)
{
    const auto window = StandaloneWindow(viewHandle);
    if (window == nullptr)
        return;

    constexpr DWORD kUseImmersiveDarkMode = 20;          // Windows 10 20H1 and later
    constexpr DWORD kUseImmersiveDarkModeBefore20H1 = 19; // Windows 10 1809 to 1909
    constexpr DWORD kBorderColour = 34;                   // Windows 11 from here down
    constexpr DWORD kCaptionColour = 35;
    constexpr DWORD kTextColour = 36;

    const double brightness = (0.299 * ((caption >> 16) & 0xff) + 0.587 * ((caption >> 8) & 0xff) + 0.114 * (caption & 0xff)) / 255.0;
    const BOOL dark = brightness < 0.5 ? TRUE : FALSE;
    if (FAILED(DwmSetWindowAttribute(window, kUseImmersiveDarkMode, &dark, sizeof(dark))))
        DwmSetWindowAttribute(window, kUseImmersiveDarkModeBefore20H1, &dark, sizeof(dark));

    const auto set = [window](DWORD attribute, std::uint32_t rgb) {
        const COLORREF ref = ToColorRef(rgb);
        DwmSetWindowAttribute(window, attribute, &ref, sizeof(ref));
    };
    set(kCaptionColour, caption);
    set(kTextColour, text);
    set(kBorderColour, border);
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}
#else
void MakeStandaloneWindowResizable(void*) {}
void ThemeStandaloneWindow(void*, std::uint32_t, std::uint32_t, std::uint32_t) {}
#endif
} // namespace nanoq
