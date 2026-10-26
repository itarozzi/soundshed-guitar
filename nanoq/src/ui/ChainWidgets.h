#pragma once

// The pieces the chain strip (ChainStrip.cpp) is drawn from: a node's card, the cable between two
// cards with its +, the rails where a parallel block splits and joins, and the cable that runs
// back under a wrapped line. Each is an Elements element that paints from the theme; the strip
// decides where they go.

#include "ui/Graphics.h"
#include "ui/Layout.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>

namespace nanoq::ui
{
namespace chain
{
constexpr float kCardHeight = 80.0f;
constexpr float kBoundaryWidth = 84.0f;
constexpr float kConnectorWidth = 30.0f;
constexpr float kRailWidth = 30.0f;
constexpr float kLaneGap = 10.0f;
constexpr float kWireEndWidth = 14.0f; // where a line's cable turns down, or the next one's comes in
constexpr float kRouteHeight = 18.0f;  // the cable back under a line
constexpr float kSidePad = 12.0f;
constexpr float kPowerSize = 18.0f;

/// A card's width for its text, estimated from its length so the wrap can be worked out before
/// anything is measured: Inter at these sizes runs about 7.4 px a capital and 6 px a small letter.
inline float CardWidthFor(const std::string& name, const std::string& detail)
{
    const auto chars = [](const std::string& s) {
        return static_cast<float>(std::count_if(s.begin(), s.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
    };
    const float wanted = std::max(chars(name) * 7.4f, chars(detail) * 6.0f) + 30.0f;
    return std::clamp(wanted, 100.0f, 190.0f);
}

/// The cable's colour.
inline el::color CableColour(const Theme& theme)
{
    return theme.TextMuted().opacity(0.55f);
}

/// The card's bounds inside whatever height the row gives it: centred, so every card and cable
/// in a row shares one centre line.
inline el::rect CardRect(const el::rect& bounds, float height = kCardHeight)
{
    const float top = bounds.top + std::max(0.0f, (bounds.height() - height) * 0.5f);
    return el::rect{bounds.left, top, bounds.right, top + height}.inset(1.5f, 1.5f);
}
} // namespace chain

using namespace chain;

/// One node in the chain.
class NodeCard final : public el::element
{
public:
    enum class Light
    {
        None,  // the card is off, or it is the splitter
        Idle,  // on, nothing passing
        Live,  // passing signal
        Clip
    };

    enum class Boundary
    {
        None,
        Input,
        Output
    };

    struct Look
    {
        std::string name;
        std::string detail;
        std::string badge;
        std::string icon;
        std::shared_ptr<artist::image> thumbnail;
        el::color tint;
        float width = 120.0f;
        bool enabled = true;
        bool selected = false;
        bool missing = false;
        bool canBypass = true;
        Boundary boundary = Boundary::None;
    };

    NodeCard(const Theme& theme, Icons& icons, Look look) : mTheme(theme), mIcons(icons), mLook(std::move(look)) {}

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{mLook.width, kCardHeight}, {mLook.width, el::full_extent}};
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto r = CardRect(ctx.bounds);
        const auto tint = mLook.tint;
        const bool on = mLook.enabled;

        // The face: the card colour with the category's tint washed down from the top, and its
        // border in the tint; the selected card in the accent, with a glow.
        {
            auto state = cnv.new_state();
            if (mLook.selected)
                cnv.shadow_style(14.0f, mTheme.Accent().opacity(0.45f));
            cnv.fill_style(mLook.selected ? mTheme.Elevated() : mTheme.Card());
            cnv.fill_round_rect(r, 10.0f);
        }
        {
            auto state = cnv.new_state();
            artist::canvas::linear_gradient wash{r.top_left(), r.bottom_left()};
            wash.add_color_stop(0.0f, tint.opacity(on ? 0.22f : 0.08f));
            wash.add_color_stop(1.0f, tint.opacity(0.0f));
            cnv.fill_style(wash);
            cnv.fill_round_rect(r, 10.0f);
        }
        cnv.stroke_style(mLook.selected ? mTheme.Accent() : (on ? tint.opacity(0.65f) : mTheme.Border()));
        cnv.line_width(mLook.selected ? 2.0f : 1.0f);
        cnv.stroke_round_rect(r, 10.0f);

        const el::color ink = on ? mTheme.Text() : mTheme.TextMuted();

        // The picture: the effect's artwork when it has some, else its icon.
        const el::rect picture{r.left + r.width() * 0.5f - 17.0f, r.top + 8.0f, r.left + r.width() * 0.5f + 17.0f, r.top + 32.0f};
        if (mLook.thumbnail)
        {
            auto state = cnv.new_state();
            cnv.begin_path();
            cnv.add_round_rect(picture, 4.0f);
            cnv.clip();
            cnv.fill_style(mTheme.Background());
            cnv.fill_rect(picture);
            DrawImageCovering(cnv, *mLook.thumbnail, picture);
        }
        else
        {
            const el::rect iconBox{picture.left + 6.0f, picture.top, picture.right - 6.0f, picture.bottom};
            mIcons.Draw(cnv, mLook.icon, iconBox, on ? tint : mTheme.TextMuted());
        }

        // The name, in capitals as the web UI's are, the effect under it, and the model's badge.
        const auto nameFont = Font(Weight::SemiBold).size(11.5f);
        const auto detailFont = Font(Weight::Regular).size(10.5f);
        const float textWidth = r.width() - 14.0f;
        float y = r.top + 44.0f;
        FillText(cnv, Ellipsize(cnv, Upper(mLook.name), nameFont, textWidth), nameFont, ink,
                 el::rect{r.left + 7.0f, y - 7.0f, r.right - 7.0f, y + 7.0f}, HAlign::Centre);
        y += 14.0f;
        if (!mLook.detail.empty())
        {
            FillText(cnv, Ellipsize(cnv, mLook.detail, detailFont, textWidth), detailFont, mTheme.TextMuted(),
                     el::rect{r.left + 7.0f, y - 6.0f, r.right - 7.0f, y + 6.0f}, HAlign::Centre);
            y += 13.0f;
        }
        if (!mLook.badge.empty())
        {
            const auto badgeFont = Font(Weight::SemiBold).size(9.5f);
            const float w = el::measure_text(cnv, mLook.badge, badgeFont).x + 10.0f;
            const el::rect pill{r.left + (r.width() - w) * 0.5f, y - 6.0f, r.left + (r.width() + w) * 0.5f, y + 6.0f};
            cnv.fill_style(mTheme.Success().opacity(0.16f));
            cnv.fill_round_rect(pill, 6.0f);
            FillText(cnv, mLook.badge, badgeFont, mTheme.Success(), pill, HAlign::Centre);
        }

        // Off: a pill saying so. On: the signal light.
        if (!on && mLook.canBypass)
        {
            const auto font = Font(Weight::SemiBold).size(9.0f);
            const el::rect pill{r.right - 30.0f, r.top + 6.0f, r.right - 6.0f, r.top + 18.0f};
            cnv.fill_style(mTheme.TextMuted().opacity(0.25f));
            cnv.fill_round_rect(pill, 6.0f);
            FillText(cnv, "OFF", font, mTheme.TextSecondary(), pill, HAlign::Centre);
        }
        else if (mLight != Light::None)
        {
            const el::color colour = mLight == Light::Clip ? mTheme.Error()
                                   : mLight == Light::Live ? mTheme.Success()
                                                           : mTheme.Success().opacity(0.3f);
            const el::point c{r.right - 9.0f, r.bottom - 9.0f};
            if (mLight == Light::Live || mLight == Light::Clip)
            {
                cnv.fill_style(colour.opacity(0.25f));
                cnv.add_circle(artist::circle(c, 6.0f));
                cnv.fill();
            }
            cnv.fill_style(colour);
            cnv.add_circle(artist::circle(c, 3.2f));
            cnv.fill();
        }

        if (mLook.missing)
        {
            const auto font = Font(Weight::SemiBold).size(13.0f);
            FillText(cnv, "\xE2\x9A\xA0", font, mTheme.Warning(), el::rect{r.right - 24.0f, r.top + 20.0f, r.right - 6.0f, r.top + 36.0f},
                     HAlign::Centre);
        }

        // The power button: shown while the card is hovered, and while it is off.
        if (mLook.canBypass && (mHover || !on))
        {
            const auto p = PowerRect(r);
            cnv.fill_style(mHoverPower ? mTheme.Elevated() : mTheme.Background().opacity(0.6f));
            cnv.add_circle(artist::circle(artist::center_point(p), kPowerSize * 0.5f));
            cnv.fill();
            mIcons.Draw(cnv, "output", p.inset(3.0f, 3.0f), on ? mTheme.Success() : mTheme.TextSecondary());
        }
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (btn.state == el::mouse_button::right)
        {
            if (btn.down && onMenu)
                onMenu();
            return true;
        }
        if (btn.state != el::mouse_button::left)
            return false;

        const auto r = CardRect(ctx.bounds);
        if (btn.down)
        {
            mDown = true;
            mDownOnPower = mLook.canBypass && PowerRect(r).inset(-4.0f, -4.0f).includes(btn.pos);
            return true;
        }

        const bool wasDown = mDown;
        mDown = false;
        if (!wasDown || !r.includes(btn.pos))
            return true;

        if (mDownOnPower)
        {
            if (onToggle)
                onToggle();
        }
        else if (btn.num_clicks >= 2 && onReplace)
        {
            onReplace();
        }
        else if (onSelect)
        {
            onSelect();
        }
        return true;
    }

    bool cursor(const el::context& ctx, el::point p, el::cursor_tracking status) override
    {
        const bool inside = status != el::cursor_tracking::leaving;
        const bool power = inside && mLook.canBypass && PowerRect(CardRect(ctx.bounds)).inset(-4.0f, -4.0f).includes(p);
        if (inside != mHover || power != mHoverPower)
        {
            mHover = inside;
            mHoverPower = power;
            ctx.view.refresh(ctx);
        }
        return false;
    }

    bool wants_control() const override
    {
        return true;
    }

    /// Returns true if the light changed, so the card is worth redrawing.
    bool SetLight(Light light)
    {
        if (light == mLight)
            return false;
        mLight = light;
        return true;
    }

    [[nodiscard]] bool Enabled() const
    {
        return mLook.enabled;
    }

    [[nodiscard]] Boundary BoundaryKind() const
    {
        return mLook.boundary;
    }

    std::function<void()> onSelect, onToggle, onMenu, onReplace;

private:
    static el::rect PowerRect(const el::rect& card)
    {
        return el::rect{card.left + 6.0f, card.top + 6.0f, card.left + 6.0f + kPowerSize, card.top + 6.0f + kPowerSize};
    }

    const Theme& mTheme;
    Icons& mIcons;
    Look mLook;
    Light mLight = Light::Idle;
    bool mDown = false, mDownOnPower = false, mHover = false, mHoverPower = false;
};

/// A cable between two things in a row. With `onAdd` it carries a + in its middle that inserts
/// an effect there; `stretch` lets it run on to fill a short branch.
class Connector final : public el::element
{
public:
    Connector(const Theme& theme, std::function<void()> onAdd, bool stretch = false)
        : mTheme(theme), mOnAdd(std::move(onAdd)), mStretch(stretch)
    {
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{kConnectorWidth, kCardHeight}, {mStretch ? el::full_extent : kConnectorWidth, el::full_extent}};
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const float y = b.top + b.height() * 0.5f;

        cnv.stroke_style(CableColour(mTheme));
        cnv.line_width(2.0f);
        cnv.begin_path();
        cnv.move_to(b.left, y);
        cnv.line_to(b.right, y);
        cnv.stroke();

        if (!mOnAdd)
            return;

        const el::point c{b.left + kConnectorWidth * 0.5f, y};
        cnv.fill_style(mHover ? mTheme.AccentFill() : mTheme.Elevated());
        cnv.add_circle(artist::circle(c, mHover ? 9.0f : 7.0f));
        cnv.fill();
        cnv.stroke_style(mHover ? mTheme.Accent() : mTheme.BorderStrong());
        cnv.line_width(1.0f);
        cnv.add_circle(artist::circle(c, mHover ? 9.0f : 7.0f));
        cnv.stroke();

        const float arm = mHover ? 4.5f : 3.5f;
        cnv.stroke_style(mHover ? mTheme.OnAccent() : mTheme.TextSecondary());
        cnv.line_width(1.5f);
        cnv.begin_path();
        cnv.move_to(c.x - arm, c.y);
        cnv.line_to(c.x + arm, c.y);
        cnv.move_to(c.x, c.y - arm);
        cnv.line_to(c.x, c.y + arm);
        cnv.stroke();
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (!mOnAdd || btn.state != el::mouse_button::left)
            return false;
        if (!btn.down && HitsPlus(ctx, btn.pos))
            mOnAdd();
        return HitsPlus(ctx, btn.pos);
    }

    bool cursor(const el::context& ctx, el::point p, el::cursor_tracking status) override
    {
        const bool hover = mOnAdd && status != el::cursor_tracking::leaving && HitsPlus(ctx, p);
        if (hover != mHover)
        {
            mHover = hover;
            ctx.view.refresh(ctx);
        }
        return false;
    }

    bool wants_control() const override
    {
        return static_cast<bool>(mOnAdd);
    }

private:
    bool HitsPlus(const el::context& ctx, el::point p) const
    {
        const float y = ctx.bounds.top + ctx.bounds.height() * 0.5f;
        return std::abs(p.x - (ctx.bounds.left + kConnectorWidth * 0.5f)) <= 12.0f && std::abs(p.y - y) <= 12.0f;
    }

    const Theme& mTheme;
    std::function<void()> mOnAdd;
    bool mStretch;
    bool mHover = false;
};

/// Where a parallel block splits (or joins): the cable from the main line meets a rail that
/// reaches each branch, with the split (join) icon on it. The split's icon selects the splitter,
/// which is where its Branches control is.
class Rail final : public el::element
{
public:
    Rail(const Theme& theme, Icons& icons, int lanes, bool join, bool selected, std::function<void()> onSelect)
        : mTheme(theme), mIcons(icons), mLanes(lanes), mJoin(join), mSelected(selected), mOnSelect(std::move(onSelect))
    {
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        const float h = Height();
        return {{kRailWidth, h}, {kRailWidth, el::full_extent}};
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const float top = b.top + std::max(0.0f, (b.height() - Height()) * 0.5f);
        const float mid = b.top + b.height() * 0.5f;
        const float x = mJoin ? b.left + 8.0f : b.right - 8.0f; // the rail
        const float outside = mJoin ? b.right : b.left;
        const float inside = mJoin ? b.left : b.right;

        cnv.stroke_style(CableColour(mTheme));
        cnv.line_width(2.0f);
        cnv.line_join(artist::canvas::round_join);
        cnv.begin_path();
        cnv.move_to(outside, mid);
        cnv.line_to(x, mid);
        const float first = top + kCardHeight * 0.5f;
        const float last = top + Height() - kCardHeight * 0.5f;
        cnv.move_to(x, first);
        cnv.line_to(x, last);
        for (int i = 0; i < mLanes; ++i)
        {
            const float y = first + static_cast<float>(i) * (kCardHeight + kLaneGap);
            cnv.move_to(x, y);
            cnv.line_to(inside, y);
        }
        cnv.stroke();

        const el::point c{x, mid};
        cnv.fill_style(mSelected ? mTheme.Elevated() : mTheme.Card());
        cnv.add_circle(artist::circle(c, 11.0f));
        cnv.fill();
        cnv.stroke_style(mSelected ? mTheme.Accent() : (mHover && mOnSelect ? mTheme.Text() : mTheme.BorderStrong()));
        cnv.line_width(mSelected ? 2.0f : 1.0f);
        cnv.add_circle(artist::circle(c, 11.0f));
        cnv.stroke();
        mIcons.Draw(cnv, mJoin ? "parallel-join" : "parallel-split", el::rect{c.x - 7.0f, c.y - 7.0f, c.x + 7.0f, c.y + 7.0f},
                    mTheme.TextSecondary());
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (!mOnSelect || btn.state != el::mouse_button::left)
            return false;
        if (!btn.down)
            mOnSelect();
        return true;
    }

    bool cursor(const el::context& ctx, el::point, el::cursor_tracking status) override
    {
        const bool hover = status != el::cursor_tracking::leaving;
        if (hover != mHover)
        {
            mHover = hover;
            ctx.view.refresh(ctx);
        }
        return false;
    }

    bool wants_control() const override
    {
        return static_cast<bool>(mOnSelect);
    }

private:
    [[nodiscard]] float Height() const
    {
        return static_cast<float>(mLanes) * kCardHeight + static_cast<float>(std::max(0, mLanes - 1)) * kLaneGap;
    }

    const Theme& mTheme;
    Icons& mIcons;
    int mLanes;
    bool mJoin, mSelected;
    std::function<void()> mOnSelect;
    bool mHover = false;
};

/// The end of a wrapped line's cable: in from the left and down (`out`), or in from above and
/// on to the right (the next line's start).
class WireEnd final : public el::element
{
public:
    WireEnd(const Theme& theme, bool out) : mTheme(theme), mOut(out) {}

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{kWireEndWidth, 0.0f}, {kWireEndWidth, el::full_extent}};
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const float mid = b.top + b.height() * 0.5f;
        const float x = b.left + kWireEndWidth * 0.5f;
        cnv.stroke_style(CableColour(mTheme));
        cnv.line_width(2.0f);
        cnv.begin_path();
        if (mOut)
        {
            cnv.move_to(b.left, mid);
            cnv.arc_to(el::point{x, mid}, el::point{x, b.bottom}, 6.0f);
            cnv.line_to(x, b.bottom);
        }
        else
        {
            cnv.move_to(x, b.top);
            cnv.arc_to(el::point{x, mid}, el::point{b.right, mid}, 6.0f);
            cnv.line_to(b.right, mid);
        }
        cnv.stroke();
    }

private:
    const Theme& mTheme;
    bool mOut;
};

/// The cable back under a line: down from where the line ended, across, and down into the next.
class ReturnRoute final : public el::element
{
public:
    ReturnRoute(const Theme& theme, float fromX, float toX) : mTheme(theme), mFrom(fromX), mTo(toX) {}

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{0.0f, kRouteHeight}, {el::full_extent, kRouteHeight}};
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const float mid = b.top + b.height() * 0.5f;
        const float from = b.left + mFrom, to = b.left + mTo;
        cnv.stroke_style(CableColour(mTheme));
        cnv.line_width(2.0f);
        cnv.begin_path();
        cnv.move_to(from, b.top);
        cnv.arc_to(el::point{from, mid}, el::point{to, mid}, 6.0f);
        cnv.arc_to(el::point{to, mid}, el::point{to, b.bottom}, 6.0f);
        cnv.line_to(to, b.bottom);
        cnv.stroke();
    }

private:
    const Theme& mTheme;
    float mFrom, mTo;
};

} // namespace nanoq::ui
