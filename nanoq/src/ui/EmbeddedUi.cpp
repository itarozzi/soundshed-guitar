#include "ui/EmbeddedUi.h"

#include "ui/UiRoot.h"

#include <elements.hpp>

namespace nanoq::ui
{
struct EmbeddedUi::Impl
{
    // The view first: the UI is dropped before the window it draws in.
    std::unique_ptr<cycfi::elements::view> view;
    std::unique_ptr<UiRoot> root;
};

EmbeddedUi::EmbeddedUi(UiLink& link, void* parentWindow) : mImpl(std::make_unique<Impl>())
{
    // Elements makes a view on a window its child, sized to the parent's client area until the
    // host places it.
    mImpl->view = std::make_unique<cycfi::elements::view>(static_cast<cycfi::elements::host_view_handle>(parentWindow));
    mImpl->root = std::make_unique<UiRoot>(link, *mImpl->view);
}

EmbeddedUi::~EmbeddedUi()
{
    mImpl->root.reset();
    mImpl->view.reset();
}

void* EmbeddedUi::NativeWindow() const
{
    return static_cast<void*>(mImpl->view->host());
}

void EmbeddedUi::Place(int x, int y, int width, int height, bool visible)
{
    const auto hwnd = mImpl->view->host();
    SetWindowPos(hwnd, nullptr, x, y, width, height, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    ShowWindow(hwnd, visible ? SW_SHOWNA : SW_HIDE);
}
} // namespace nanoq::ui
