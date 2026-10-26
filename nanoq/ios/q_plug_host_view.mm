// QPlug's platform view glue for iOS: q_plug/src/macos/host_view.mm with UIView for NSView.
// QPlug asks for CLAP_WINDOW_API_COCOA on every Apple platform; on iOS the "cocoa" handle the
// app passes to clap.gui's set_parent is a UIView. See docs/plans/nano-q-ios.md.

#import <UIKit/UIKit.h>
#include <q_plug/host_view.hpp>

namespace cycfi::q_plug::detail
{
// A UIView needs no parent to exist, so the content is built and measured as soon as the
// editor is created, as on macOS.
extern bool const unparented_view_ok = true;

plugin_view* make_view(void*, elements::extent size)
{
    return new plugin_view(size);
}

// The parent arrives: fill it and follow it.
void add_subview(void* parent, void* child)
{
    UIView* parent_ = (__bridge UIView*)parent;
    UIView* child_ = (__bridge UIView*)child;
    child_.frame = parent_.bounds;
    child_.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [parent_ addSubview:child_];
}

// The app deals in points, as UIKit does.
float pixel_scale(plugin_view const*)
{
    return 1.0f;
}

// Events arrive through the app's own run loop.
int event_fd()
{
    return -1;
}

void pump_events() {}
} // namespace cycfi::q_plug::detail
