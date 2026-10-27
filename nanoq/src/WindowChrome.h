#pragma once

// The standalone's own window, on Windows: its frame and its caption's colours. Inside a host
// the window is the host's, and these do nothing; elsewhere too.

#include <cstdint>

namespace nanoq
{
/// Gives the standalone's window a sizing border and a maximise box. clap-wrapper drops them
/// when the editor says it cannot resize, and QPlug says so on Windows because it builds the
/// view only once the window exists, so the question comes before there are limits to answer
/// it. `viewHandle` is the editor's own window (Elements' view host).
void MakeStandaloneWindowResizable(void* viewHandle);

/// Colours the standalone's caption, its text and its border (0xRRGGBB), as Soundshed Guitar
/// does from its header (juce/source/WindowChrome.cpp), with dark-mode caption buttons on a
/// dark caption. Windows 11; older Windows keeps its own caption.
void ThemeStandaloneWindow(void* viewHandle, std::uint32_t caption, std::uint32_t text, std::uint32_t border);
} // namespace nanoq
