#pragma once

// KeyRouter - the keyboard shortcuts' way in.
//
// Elements hands a key only along the focus chain, and a click on anything that does not want
// focus (every button, card and knob here) clears it, so with nothing focused a key reaches no
// element at all. The router sits at the root and wants focus itself; Retake(), called each tick,
// gives it back after a click took it away. A key goes to the focused element first (a text box
// that is being typed in keeps every key), and only what nothing used reaches `onKey`; Esc goes
// to `onKey` first, so it closes a dialog from inside its text box.

#include <elements.hpp>

#include <functional>
#include <memory>

namespace nanoq::ui
{
class KeyRouter final : public cycfi::elements::proxy_base
{
public:
    /// `focusContent`: a dialog's router passes focus on to what it holds (its text box gets the
    /// caret as it opens); the root's keeps it, so taking focus back puts no caret anywhere.
    explicit KeyRouter(cycfi::elements::element_ptr subject, bool focusContent = false)
        : mSubject(std::move(subject)), mFocusContent(focusContent)
    {
    }

    cycfi::elements::element& subject() override
    {
        return *mSubject;
    }
    const cycfi::elements::element& subject() const override
    {
        return *mSubject;
    }

    bool wants_focus() const override
    {
        return true;
    }

    // Taking focus back for the shortcuts must not move it into the content (it would put a caret
    // in the first text box); a window regaining focus restores what was focused before.
    void begin_focus(focus_request req) override
    {
        if (mFocusContent || req == focus_request::restore_previous)
            proxy_base::begin_focus(req);
    }

    bool key(const cycfi::elements::context& ctx, cycfi::elements::key_info k) override
    {
        // Esc closes the menu or dialog on top first, even from its text box (which would
        // otherwise take it to drop its own focus), as Esc closes a modal in the web UI.
        if (k.key == cycfi::elements::key_code::escape && onKey && onKey(k))
            return true;
        if (proxy_base::key(ctx, k))
            return true;
        // A text box being typed in keeps every other key.
        if (Typing())
            return false;
        return onKey && onKey(k);
    }

    bool click(const cycfi::elements::context& ctx, cycfi::elements::mouse_button btn) override
    {
        if (btn.down && onPointerDown)
            onPointerDown();
        return proxy_base::click(ctx, btn);
    }

    /// Gives the router focus again if a click took it. UI thread.
    static void Retake(cycfi::elements::view& view)
    {
        auto& content = view.content();
        if (content.focus_index() == -1)
            content.begin_focus(focus_request::from_top);
    }

    /// Whether a text box holds focus with its caret showing.
    [[nodiscard]] bool Typing() const
    {
        const cycfi::elements::element* e = &subject();
        for (int depth = 0; depth < 64; ++depth)
        {
            const auto* next = e->focus();
            if (next == nullptr || next == e)
                break;
            e = next;
        }
        const auto* box = dynamic_cast<const cycfi::elements::basic_text_box*>(e);
        return box != nullptr && box->select_start() != -1;
    }

    /// A key no focused element took. Returns whether it was used.
    std::function<bool(const cycfi::elements::key_info&)> onKey;
    /// Any press of a mouse button, before the element under it hears it.
    std::function<void()> onPointerDown;

private:
    cycfi::elements::element_ptr mSubject;
    bool mFocusContent;
};
} // namespace nanoq::ui
