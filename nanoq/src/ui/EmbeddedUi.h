#pragma once

// EmbeddedUi - the native UI in a window of its own, made a child of a window another framework
// owns. For a host that can give a native parent window but knows nothing of Elements:
// Soundshed Guitar's JUCE editor (juce/source/NativeUiEditor.cpp). The host keeps the child
// window where it wants it (its size and position, in physical pixels) and Elements does the
// rest through the platform's own message loop, so nothing has to be pumped.
//
// Nothing here names Elements, so a host's code never includes it. Windows only for now.

#include "ui/UiLink.h"

#include <memory>

namespace nanoq::ui
{
class EmbeddedUi
{
  public:
    /// `parentWindow`: the native window to be a child of (an HWND).
    EmbeddedUi(UiLink& link, void* parentWindow);
    ~EmbeddedUi();

    EmbeddedUi(const EmbeddedUi&) = delete;
    EmbeddedUi& operator=(const EmbeddedUi&) = delete;

    /// The child window the UI draws in (an HWND).
    [[nodiscard]] void* NativeWindow() const;

    /// Places the child window in its parent, in the parent's physical pixels, and shows or
    /// hides it. The UI lays itself out again at the new size.
    void Place(int x, int y, int width, int height, bool visible);

  private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};
} // namespace nanoq::ui
