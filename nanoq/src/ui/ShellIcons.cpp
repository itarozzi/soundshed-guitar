// The icons the web UI writes inline in its markup (ui-components/*.html) rather than keeping in
// images/icons: the compact rail, the preset bar, the footer and the chain's wrap switch. Copied
// path for path, so the shell draws the same glyphs; the stroke widths are the ones its CSS sets.

#include "ui/ShellIcons.h"

namespace nanoq::ui
{
namespace
{
constexpr const char* kStroke = R"( fill="none" stroke="currentColor" stroke-linecap="round" stroke-linejoin="round" )";

std::string Svg(const char* viewBox, float strokeWidth, const char* body)
{
    return std::string("<svg viewBox=\"") + viewBox + "\"" + kStroke + "stroke-width=\"" + std::to_string(strokeWidth) + "\">" +
           body + "</svg>";
}
} // namespace

void AddShellIcons(Icons& icons)
{
    // The rail (header-icon-bar.html).
    icons.Add("ui-chain", Svg("0 0 24 24", 1.8f,
                              R"(<rect x="2" y="9" width="5" height="6" rx="1.2"/><rect x="9.5" y="7" width="5" height="10" rx="1.2"/>)"
                              R"(<rect x="17" y="9" width="5" height="6" rx="1.2"/><path d="M7 12h2.5M14.5 12H17"/>)"));
    icons.Add("ui-effect", Svg("0 0 24 24", 1.8f,
                               R"(<circle cx="12" cy="13.5" r="7"/><path d="M12 13.5 16 9.5"/>)"
                               R"(<path d="M12 3v1.5M4.2 6.2l1.1 1.1M19.8 6.2l-1.1 1.1"/>)"));
    icons.Add("ui-presets", Svg("0 0 24 24", 1.8f,
                                R"(<path d="M4 6h16M4 12h16M4 18h10"/>)"));
    icons.Add("ui-gear", Svg("0 0 24 24", 1.5f,
                             R"(<path d="M12 15.5a3.5 3.5 0 1 0 0-7 3.5 3.5 0 0 0 0 7z"/>)"
                             R"(<path d="M19.4 15a1.7 1.7 0 0 0 .34 1.86l.03.03a2 2 0 0 1-2.83 2.83l-.03-.03A1.7 1.7 0 0 0 15 19.4a1.7 1.7 0 0 0-1 1.53V21a2 2 0 0 1-4 0v-.07a1.7 1.7 0 0 0-1-1.53 1.7 1.7 0 0 0-1.86.34l-.03.03a2 2 0 1 1-2.83-2.83l.03-.03A1.7 1.7 0 0 0 4.6 15a1.7 1.7 0 0 0-1.53-1H3a2 2 0 0 1 0-4h.07a1.7 1.7 0 0 0 1.53-1 1.7 1.7 0 0 0-.34-1.86l-.03-.03a2 2 0 1 1 2.83-2.83l.03.03A1.7 1.7 0 0 0 9 4.6 1.7 1.7 0 0 0 10 3.07V3a2 2 0 0 1 4 0v.07a1.7 1.7 0 0 0 1 1.53 1.7 1.7 0 0 0 1.86-.34l.03-.03a2 2 0 1 1 2.83 2.83l-.03.03A1.7 1.7 0 0 0 19.4 9c0 .46.17.9.46 1.24.3.33.74.53 1.21.56H21a2 2 0 0 1 0 4h-.07a1.7 1.7 0 0 0-1.53 1z"/>)"));

    // The preset bar (preset-selector-row.html, preset-toolbar-row.html).
    icons.Add("ui-prev", Svg("0 0 24 24", 2.0f, R"(<line x1="19" y1="12" x2="5" y2="12"/><polyline points="12 19 5 12 12 5"/>)"));
    icons.Add("ui-next", Svg("0 0 24 24", 2.0f, R"(<line x1="5" y1="12" x2="19" y2="12"/><polyline points="12 5 19 12 12 19"/>)"));
    icons.Add("ui-new", Svg("0 0 24 24", 2.0f,
                            R"(<rect x="3" y="3" width="18" height="18" rx="4"/><line x1="12" y1="7" x2="12" y2="17"/><line x1="7" y1="12" x2="17" y2="12"/>)"));
    icons.Add("ui-edit", Svg("0 0 24 24", 2.0f,
                             R"(<path d="M11 4H4a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2v-7"/>)"
                             R"(<path d="M18.5 2.5a2.121 2.121 0 0 1 3 3L12 15l-4 1 1-4 9.5-9.5z"/>)"));
    icons.Add("ui-save", Svg("0 0 24 24", 2.0f,
                             R"(<path d="M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2z"/>)"
                             R"(<polyline points="17 21 17 13 7 13 7 21"/><polyline points="7 3 7 8 15 8"/>)"));
    icons.Add("ui-random", Svg("0 0 24 24", 2.0f,
                               R"(<path d="M16 3h5v5"/><path d="M4 20L21 3"/><path d="M21 16v5h-5"/><path d="M15 15l6 6"/><path d="M4 4l5 5"/>)"));
    icons.Add("ui-more", R"(<svg viewBox="0 0 24 24" fill="currentColor" stroke="none"><circle cx="5" cy="12" r="1.75"/><circle cx="12" cy="12" r="1.75"/><circle cx="19" cy="12" r="1.75"/></svg>)");
    icons.Add("ui-caret", Svg("0 0 24 24", 2.0f, R"(<polyline points="6 9 12 15 18 9"/>)"));

    // The footer (footer-bar.html).
    icons.Add("ui-pads", Svg("0 0 24 24", 1.8f,
                             R"(<rect x="3.5" y="3.5" width="7" height="7" rx="1.5"/><rect x="13.5" y="3.5" width="7" height="7" rx="1.5"/>)"
                             R"(<rect x="3.5" y="13.5" width="7" height="7" rx="1.5"/><rect x="13.5" y="13.5" width="7" height="7" rx="1.5"/>)"));
    icons.Add("ui-tuner", Svg("0 0 24 24", 1.8f, R"(<path d="M9 3v7a3 3 0 0 0 6 0V3"/><path d="M12 13v8"/>)"));
    icons.Add("ui-undo", Svg("0 0 24 24", 1.8f, R"(<path d="M9 7 4 12l5 5"/><path d="M4 12h9a6 6 0 0 1 0 12h-1"/>)"));
    icons.Add("ui-redo", Svg("0 0 24 24", 1.8f, R"(<path d="M15 7l5 5-5 5"/><path d="M20 12h-9a6 6 0 0 0 0 12h1"/>)"));
    icons.Add("ui-swap", Svg("0 0 24 24", 1.8f, R"(<path d="M4 8h13l-3.5-3.5"/><path d="M20 16H7l3.5 3.5"/>)"));
    icons.Add("ui-metronome", Svg("0 0 24 24", 1.8f,
                                  R"(<path d="M9 3.5h6L19 20.5H5L9 3.5Z"/><path d="M10 7.5h4"/><path d="M12 8.5v7"/><path d="M12 10.5l2.75-1.75"/>)"));
    icons.Add("ui-play", R"(<svg viewBox="0 0 24 24" fill="currentColor" stroke="none"><polygon points="5 3 19 12 5 21 5 3"/></svg>)");
    icons.Add("ui-stop", R"(<svg viewBox="0 0 24 24" fill="currentColor" stroke="none"><rect x="6" y="6" width="12" height="12" rx="1.5"/></svg>)");
    icons.Add("ui-loop", Svg("0 0 24 24", 1.8f, R"(<path d="M18.5 12a6.5 6.5 0 1 1-2-4.7"/><path d="M17 3.5v4h-4"/>)"));
    icons.Add("ui-io", Svg("0 0 24 24", 1.8f, R"(<path d="M4 7h10M4 17h6M18 7h2M14 17h6"/><circle cx="16" cy="7" r="2"/><circle cx="12" cy="17" r="2"/>)"));
    icons.Add("ui-mute", Svg("0 0 24 24", 1.8f, R"(<path d="M11 5 6 9H3v6h3l5 4V5z"/><path d="M22 9l-6 6M16 9l6 6"/>)"));
    icons.Add("ui-speaker", Svg("0 0 24 24", 1.8f, R"(<path d="M11 5 6 9H3v6h3l5 4V5z"/><path d="M15.5 8.5a5 5 0 0 1 0 7"/>)"));

    // The chain (signal-path-bar.html).
    icons.Add("ui-wrap", Svg("0 0 16 16", 1.5f, R"(<path d="M2 3.5h12M2 8h9.25a2.25 2.25 0 0 1 0 4.5H7.5M9.25 10.75 7.5 12.5l1.75 1.75M2 12.5h3"/>)"));
}
} // namespace nanoq::ui
