#include "ui/Widgets.h"

#include "ui/Graphics.h"
#include "ui/Layout.h"

#include <elements/support/text_utils.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace nanoq::ui
{
namespace
{
using cycfi::artist::canvas;
} // namespace

// ── Text ────────────────────────────────────────────────────────────────────────

Text::Text(const Theme& theme, std::string text, float size, Weight weight)
    : mTheme(theme), mText(std::move(text)), mSize(size), mWeight(weight), mColour(theme.Text())
{
}

el::view_limits Text::limits(const el::basic_context& ctx) const
{
    const auto font = Font(mWeight).size(mSize);
    const auto size = el::measure_text(ctx.canvas, mText, font);
    const float h = std::max(size.y, mSize * 1.3f);
    return mFixed ? el::view_limits{{size.x, h}, {size.x, h}} : el::view_limits{{0.0f, h}, {el::full_extent, h}};
}

void Text::draw(const el::context& ctx)
{
    const auto font = Font(mWeight).size(mSize);
    const auto shown = Ellipsize(ctx.canvas, mText, font, ctx.bounds.width());
    FillText(ctx.canvas, shown, font, mColour, ctx.bounds, mAlign);
}

Text& Text::Set(std::string text)
{
    mText = std::move(text);
    return *this;
}

Text& Text::Colour(el::color c)
{
    mColour = c;
    return *this;
}

Text& Text::Alignment(Align a)
{
    mAlign = a;
    return *this;
}

Text& Text::Fixed(bool fixed)
{
    mFixed = fixed;
    return *this;
}

// ── Button ──────────────────────────────────────────────────────────────────────

Button::Button(const Theme& theme, std::string text, std::function<void()> onClick, Style style)
    : mTheme(theme), mText(std::move(text)), mOnClick(std::move(onClick)), mStyle(style)
{
}

el::view_limits Button::limits(const el::basic_context& ctx) const
{
    const auto font = Font(Weight::Medium).size(mSize);
    const auto size = el::measure_text(ctx.canvas, mText, font);
    const float h = metrics::kRowHeight;
    if (mFlexible)
        return {{mMinWidth, h}, {el::full_extent, h}};
    const float w = std::max(mMinWidth, size.x + 2.0f * metrics::kPad + (mDot ? 14.0f : 0.0f));
    return {{w, h}, {w, h}};
}

void Button::draw(const el::context& ctx)
{
    auto& cnv = ctx.canvas;
    const auto b = ctx.bounds;
    const bool lit = mOn || mSelected;

    el::color fill = mTheme.Input();
    el::color line = mTheme.BorderStrong();
    el::color ink = mTheme.Text();

    switch (mStyle)
    {
    case Style::Primary:
        fill = mTheme.AccentFill();
        line = fill;
        ink = mTheme.OnAccent();
        break;
    case Style::Flat:
        fill = lit ? mTheme.Input() : el::rgba(0, 0, 0, 0);
        line = lit ? mTheme.BorderStrong() : el::rgba(0, 0, 0, 0);
        ink = lit ? mTheme.Text() : mTheme.TextSecondary();
        break;
    case Style::Plain:
        if (lit)
        {
            fill = mTheme.Elevated();
            if (mSelected)
                line = mTheme.Accent(); // a chosen option (theme, scene) is outlined, a switch is not
        }
        break;
    }

    if (mDown)
        fill = fill.opacity(0.7f);
    else if (mHover && mEnabled && fill.alpha > 0.0f)
        fill = fill.level(1.12f);
    else if (mHover && mStyle == Style::Flat)
        fill = mTheme.Elevated().opacity(0.6f);

    if (!mEnabled)
        ink = ink.opacity(0.4f);

    if (fill.alpha > 0.0f)
    {
        cnv.fill_style(fill);
        cnv.fill_round_rect(b, metrics::kRadius);
    }
    if (line.alpha > 0.0f)
    {
        cnv.stroke_style(line);
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(b.inset(0.5f, 0.5f), metrics::kRadius);
    }

    auto textBounds = b;
    if (mDot)
    {
        // The state dot: lit when on.
        const float r = 4.0f;
        const el::point c{b.left + metrics::kPad + r, b.top + b.height() * 0.5f};
        cnv.fill_style(mOn ? mTheme.Success() : mTheme.TextMuted().opacity(0.5f));
        cnv.add_circle(cycfi::artist::circle(c, r));
        cnv.fill();
        textBounds.left += metrics::kPad + 2.0f * r + 6.0f;
        textBounds.right -= metrics::kPad * 0.5f;
        FillText(cnv, mText, Font(Weight::Medium).size(mSize), ink, textBounds, Text::Align::Left);
        return;
    }

    const auto font = Font(Weight::Medium).size(mSize);
    textBounds = textBounds.inset(metrics::kPad * 0.5f, 0.0f);
    FillText(cnv, Ellipsize(cnv, mText, font, textBounds.width()), font, ink, textBounds, mAlign);
}

bool Button::click(const el::context& ctx, el::mouse_button btn)
{
    if (!mEnabled || btn.state != el::mouse_button::left)
        return false;

    if (btn.down)
    {
        mDown = true;
        ctx.view.refresh(ctx);
        return true;
    }

    const bool wasDown = mDown;
    mDown = false;
    ctx.view.refresh(ctx);
    if (wasDown && ctx.bounds.includes(btn.pos) && mOnClick)
        mOnClick();
    return true;
}

bool Button::cursor(const el::context& ctx, el::point, el::cursor_tracking status)
{
    const bool hover = status != el::cursor_tracking::leaving;
    if (hover != mHover)
    {
        mHover = hover;
        ctx.view.refresh(ctx);
    }
    return false;
}

Button& Button::Set(std::string text)
{
    mText = std::move(text);
    return *this;
}

Button& Button::On(bool on)
{
    mOn = on;
    return *this;
}

Button& Button::Selected(bool selected)
{
    mSelected = selected;
    return *this;
}

Button& Button::Enabled(bool enabled)
{
    mEnabled = enabled;
    return *this;
}

Button& Button::MinWidth(float w)
{
    mMinWidth = w;
    return *this;
}

Button& Button::Dot(bool show)
{
    mDot = show;
    return *this;
}

Button& Button::Flexible(bool flexible)
{
    mFlexible = flexible;
    return *this;
}

Button& Button::Alignment(HAlign align)
{
    mAlign = align;
    return *this;
}

Button& Button::TextSize(float size)
{
    mSize = size;
    return *this;
}

// ── ParamSlider ─────────────────────────────────────────────────────────────────

ParamSlider::ParamSlider(const Theme& theme, std::string name) : mTheme(theme), mName(std::move(name)) {}

el::view_limits ParamSlider::limits(const el::basic_context&) const
{
    constexpr float h = 52.0f;
    return {{120.0f, h}, {el::full_extent, h}};
}

void ParamSlider::draw(const el::context& ctx)
{
    auto& cnv = ctx.canvas;
    const auto b = ctx.bounds;

    const auto nameFont = Font(Weight::Medium).size(metrics::kSmallText);
    const float valueWidth = el::measure_text(cnv, mValueText, nameFont).x;

    auto top = el::rect{b.left, b.top, b.right, b.top + 20.0f};
    auto nameBounds = top;
    nameBounds.right = std::max(nameBounds.left, top.right - valueWidth - 8.0f);
    FillText(cnv, Ellipsize(cnv, mName, nameFont, nameBounds.width()), nameFont, mTheme.TextSecondary(), nameBounds,
             Text::Align::Left);
    FillText(cnv, mValueText, nameFont, mTheme.Text(), top, Text::Align::Right);

    // The track, with the travel filled in the accent: the one place an effect page uses it.
    const float trackY = b.bottom - 14.0f;
    const float trackH = 6.0f;
    const auto track = el::rect{b.left + 6.0f, trackY - trackH * 0.5f, b.right - 6.0f, trackY + trackH * 0.5f};
    cnv.fill_style(mTheme.Input());
    cnv.fill_round_rect(track, trackH * 0.5f);

    const float v = static_cast<float>(std::clamp(mValue, 0.0, 1.0));
    const float x = track.left + v * track.width();
    auto travel = track;
    travel.right = std::max(track.left + trackH, x);
    cnv.fill_style(mTheme.AccentFill());
    cnv.fill_round_rect(travel, trackH * 0.5f);

    const float r = (mDragging || mHover) ? 9.0f : 8.0f;
    cnv.fill_style(mTheme.Text());
    cnv.add_circle(cycfi::artist::circle(el::point{x, trackY}, r));
    cnv.fill();
    cnv.stroke_style(mTheme.BorderStrong());
    cnv.line_width(1.0f);
    cnv.add_circle(cycfi::artist::circle(el::point{x, trackY}, r));
    cnv.stroke();
}

void ParamSlider::Move(const el::context& ctx, el::point p)
{
    const float left = ctx.bounds.left + 6.0f;
    const float width = std::max(1.0f, ctx.bounds.width() - 12.0f);
    mValue = std::clamp(static_cast<double>((p.x - left) / width), 0.0, 1.0);
    if (onChange)
        onChange(mValue);
    ctx.view.refresh(ctx);
}

bool ParamSlider::click(const el::context& ctx, el::mouse_button btn)
{
    if (btn.state != el::mouse_button::left)
        return false;

    if (btn.down)
    {
        if (btn.num_clicks == 2 && onReset)
        {
            onReset();
            return true;
        }
        mDragging = true;
        Move(ctx, btn.pos);
    }
    else
    {
        mDragging = false;
        ctx.view.refresh(ctx);
    }
    return true;
}

void ParamSlider::drag(const el::context& ctx, el::mouse_button btn)
{
    if (mDragging)
        Move(ctx, btn.pos);
}

bool ParamSlider::cursor(const el::context& ctx, el::point, el::cursor_tracking status)
{
    const bool hover = status != el::cursor_tracking::leaving;
    if (hover != mHover)
    {
        mHover = hover;
        ctx.view.refresh(ctx);
    }
    return false;
}

bool ParamSlider::scroll(const el::context& ctx, el::point dir, el::point)
{
    // A wheel nudges by 2%; the page's own scroller gets it when the slider is not hovered.
    if (dir.y == 0.0f || !mHover)
        return false;

    mValue = std::clamp(mValue + (dir.y > 0.0f ? 0.02 : -0.02), 0.0, 1.0);
    if (onChange)
        onChange(mValue);
    ctx.view.refresh(ctx);
    return true;
}

void ParamSlider::Update(double normalised, std::string valueText)
{
    if (mDragging)
        return;
    mValue = normalised;
    mValueText = std::move(valueText);
}

// ── ParamKnob ───────────────────────────────────────────────────────────────────

namespace
{
// The web UI's node-param-group: a 9 px capital label, a 34 px knob, a 10 px value.
constexpr float kKnobCellWidth = 60.0f;
constexpr float kKnobNameHeight = 12.0f;
constexpr float kKnobValueHeight = 13.0f;
constexpr float kKnobDiameter = 34.0f;
constexpr float kKnobDragPixels = 200.0f;
// A 270 degree sweep, open at the bottom: 7 o'clock round to 5 o'clock.
constexpr float kKnobStart = 3.14159265f * 0.75f;
constexpr float kKnobSweep = 3.14159265f * 1.5f;
} // namespace

ParamKnob::ParamKnob(const Theme& theme, std::string name) : mTheme(theme), mName(std::move(name)) {}

el::view_limits ParamKnob::limits(const el::basic_context&) const
{
    const float h = kKnobNameHeight + kKnobDiameter + kKnobValueHeight + 4.0f;
    return {{kKnobCellWidth, h}, {kKnobCellWidth, h}};
}

void ParamKnob::draw(const el::context& ctx)
{
    auto& cnv = ctx.canvas;
    const auto b = ctx.bounds;
    const bool light = mTheme.IsLight();
    // The web UI's .node-param-label and .node-param-value.
    const auto labelFont = Font(Weight::SemiBold).size(9.0f);
    const auto valueFont = Font(Weight::SemiBold).size(10.0f);
    const el::color labelInk = light ? mTheme.TextSecondary() : el::rgba(201, 209, 225, 184);
    const el::color valueInk = light ? mTheme.Text() : el::rgba(231, 236, 246, 194);

    const auto nameBounds = el::rect{b.left, b.top, b.right, b.top + kKnobNameHeight};
    FillText(cnv, Ellipsize(cnv, Upper(mName), labelFont, nameBounds.width() + 6.0f), labelFont, labelInk, nameBounds,
             Text::Align::Centre);

    const float cx = (b.left + b.right) * 0.5f;
    const float cy = nameBounds.bottom + 2.0f + kKnobDiameter * 0.5f;
    const float r = kKnobDiameter * 0.5f;
    const float v = static_cast<float>(std::clamp(mValue, 0.0, 1.0));

    // The ring: a faint track all the way round, and the travel over it, shading from the ring's
    // start colour into its end colour as the web UI's conic gradient does (from the left end, or
    // from the centre of a bipolar control: the default sitting at the middle).
    constexpr float kArcWidth = 3.0f;
    const float arcR = r - kArcWidth * 0.5f;
    {
        auto state = cnv.new_state();
        cnv.line_width(kArcWidth);
        cnv.stroke_style(mTheme.KnobRingTrack());
        cnv.begin_path();
        cnv.arc(cx, cy, arcR, 0.0f, 2.0f * 3.14159265f);
        cnv.stroke();

        const float from = (mDefault > 0.4 && mDefault < 0.6) ? static_cast<float>(mDefault) : 0.0f;
        const float lo = std::min(from, v);
        const float hi = std::max(from, v);
        if (hi - lo > 0.002f)
        {
            const auto start = mTheme.KnobRingStart(), end = mTheme.KnobRingEnd();
            constexpr int kSteps = 24;
            for (int i = 0; i < kSteps; ++i)
            {
                const float t0 = lo + (hi - lo) * static_cast<float>(i) / kSteps;
                const float t1 = lo + (hi - lo) * static_cast<float>(i + 1) / kSteps;
                // The shade follows the position on the dial, not the slice of travel drawn.
                const float mix = std::clamp(t1 / std::max(0.001f, hi), 0.0f, 1.0f);
                cnv.stroke_style(el::color{start.red + (end.red - start.red) * mix, start.green + (end.green - start.green) * mix,
                                           start.blue + (end.blue - start.blue) * mix, 1.0f});
                cnv.begin_path();
                cnv.arc(cx, cy, arcR, kKnobStart + kKnobSweep * t0, kKnobStart + kKnobSweep * t1 + 0.01f);
                cnv.stroke();
            }
        }
    }

    // The cap: a dark dome inside the ring, with a white pointer.
    const float capR = r - kArcWidth - 1.5f;
    {
        auto state = cnv.new_state();
        cycfi::artist::canvas::radial_gradient dome{el::point{cx, cy - capR * 0.4f}, 0.0f, el::point{cx, cy}, capR};
        const auto top = light ? el::rgba(250, 250, 252, 255) : el::rgba(58, 58, 68, 255);
        const auto bottom = light ? el::rgba(214, 216, 224, 255) : el::rgba(22, 22, 28, 255);
        dome.add_color_stop(0.0f, (mDragging || mHover) ? top.level(1.15f) : top);
        dome.add_color_stop(1.0f, bottom);
        cnv.fill_style(dome);
        cnv.add_circle(cycfi::artist::circle(el::point{cx, cy}, capR));
        cnv.fill();
    }
    cnv.stroke_style(light ? el::rgba(0, 0, 0, 40) : el::rgba(0, 0, 0, 140));
    cnv.line_width(1.0f);
    cnv.add_circle(cycfi::artist::circle(el::point{cx, cy}, capR));
    cnv.stroke();

    const float angle = kKnobStart + kKnobSweep * v;
    const float c = std::cos(angle), s = std::sin(angle);
    {
        auto state = cnv.new_state();
        cnv.stroke_style(light ? mTheme.Text() : el::rgba(240, 240, 248, 255));
        cnv.line_width(2.0f);
        cnv.line_cap(canvas::round);
        cnv.begin_path();
        cnv.move_to(cx + c * capR * 0.40f, cy + s * capR * 0.40f);
        cnv.line_to(cx + c * capR * 0.85f, cy + s * capR * 0.85f);
        cnv.stroke();
    }

    const auto valueBounds = el::rect{b.left, b.bottom - kKnobValueHeight, b.right, b.bottom};
    FillText(cnv, Ellipsize(cnv, mValueText, valueFont, valueBounds.width() + 6.0f), valueFont, valueInk, valueBounds,
             Text::Align::Centre);
}

bool ParamKnob::click(const el::context& ctx, el::mouse_button btn)
{
    if (btn.state != el::mouse_button::left)
        return false;

    if (btn.down)
    {
        if (btn.num_clicks == 2 && onReset)
        {
            onReset();
            return true;
        }
        mDragging = true;
        mDragStartY = btn.pos.y;
        mDragStartValue = mValue;
    }
    else
    {
        mDragging = false;
        ctx.view.refresh(ctx);
    }
    return true;
}

void ParamKnob::drag(const el::context& ctx, el::mouse_button btn)
{
    if (!mDragging)
        return;

    // Up turns it up. Holding Shift slows the turn to a fifth.
    const float scale = btn.modifiers & el::mod_shift ? 5.0f : 1.0f;
    const double delta = static_cast<double>(mDragStartY - btn.pos.y) / (kKnobDragPixels * scale);
    mValue = std::clamp(mDragStartValue + delta, 0.0, 1.0);
    if (onChange)
        onChange(mValue);
    ctx.view.refresh(ctx);
}

bool ParamKnob::cursor(const el::context& ctx, el::point, el::cursor_tracking status)
{
    const bool hover = status != el::cursor_tracking::leaving;
    if (hover != mHover)
    {
        mHover = hover;
        ctx.view.refresh(ctx);
    }
    return false;
}

bool ParamKnob::scroll(const el::context& ctx, el::point dir, el::point)
{
    if (dir.y == 0.0f || !mHover)
        return false;

    mValue = std::clamp(mValue + (dir.y > 0.0f ? 0.02 : -0.02), 0.0, 1.0);
    if (onChange)
        onChange(mValue);
    ctx.view.refresh(ctx);
    return true;
}

void ParamKnob::Update(double normalised, std::string valueText)
{
    if (mDragging)
        return;
    mValue = normalised;
    mValueText = std::move(valueText);
}

// ── GroupHeading ────────────────────────────────────────────────────────────────

GroupHeading::GroupHeading(const Theme& theme, std::string text) : mTheme(theme), mText(std::move(text))
{
    for (auto& ch : mText)
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
}

el::view_limits GroupHeading::limits(const el::basic_context&) const
{
    return {{0.0f, 24.0f}, {el::full_extent, 24.0f}};
}

void GroupHeading::draw(const el::context& ctx)
{
    auto& cnv = ctx.canvas;
    const auto b = ctx.bounds;
    const auto font = Font(Weight::SemiBold).size(11.0f);
    const float w = el::measure_text(cnv, mText, font).x;

    FillText(cnv, mText, font, mTheme.TextMuted(), el::rect{b.left, b.top + 4.0f, b.left + w + 2.0f, b.bottom}, Text::Align::Left);

    const float y = b.top + 4.0f + (b.height() - 4.0f) * 0.5f;
    if (b.left + w + 14.0f < b.right)
    {
        cnv.stroke_style(mTheme.Border());
        cnv.line_width(1.0f);
        cnv.begin_path();
        cnv.move_to(b.left + w + 12.0f, y);
        cnv.line_to(b.right, y);
        cnv.stroke();
    }
}

// ── MeterBar ────────────────────────────────────────────────────────────────────

MeterBar::MeterBar(const Theme& theme) : mTheme(theme) {}

el::view_limits MeterBar::limits(const el::basic_context&) const
{
    if (mVertical)
        return {{30.0f, 84.0f}, {30.0f, 84.0f}};
    return {{60.0f, 8.0f}, {el::full_extent, 8.0f}};
}

void MeterBar::draw(const el::context& ctx)
{
    auto& cnv = ctx.canvas;
    const auto b = ctx.bounds;
    if (mVertical)
    {
        // The peak over a column of eight segments, lit from the bottom: green, then amber in
        // the top two, red when it clips.
        std::string text = "-\xE2\x88\x9E dB"; // -inf
        if (mPeakDb > -60.0)
        {
            char number[16];
            std::snprintf(number, sizeof(number), "%.0f dB", mPeakDb);
            text = number;
        }
        FillText(cnv, text, Font(Weight::SemiBold).size(9.0f), mTheme.TextSecondary(), el::rect{b.left - 6.0f, b.top, b.right + 6.0f, b.top + 12.0f},
                 Text::Align::Centre);
        constexpr int kSegments = 8;
        const float level = static_cast<float>(std::clamp((mPeakDb + 60.0) / 60.0, 0.0, 1.0));
        const int lit = static_cast<int>(std::ceil(level * kSegments - 0.01f));
        const float top = b.top + 16.0f, h = (b.bottom - top) / kSegments;
        const float cx = (b.left + b.right) * 0.5f;
        for (int i = 0; i < kSegments; ++i)
        {
            const el::rect seg{cx - 6.0f, b.bottom - (i + 1) * h + 1.0f, cx + 6.0f, b.bottom - i * h - 1.0f};
            el::color colour = mTheme.Pick(el::rgba(46, 46, 56, 255), mTheme.KnobRingTrack());
            if (i < lit)
                colour = mClipped ? mTheme.Error() : (i >= kSegments - 2 ? mTheme.Warning() : mTheme.Success());
            cnv.fill_style(colour);
            cnv.fill_round_rect(seg, 1.5f);
        }
        return;
    }
    cnv.fill_style(mTheme.Input());
    cnv.fill_round_rect(b, b.height() * 0.5f);

    const float level = static_cast<float>(std::clamp((mPeakDb + 60.0) / 60.0, 0.0, 1.0));
    if (level <= 0.0f)
        return;

    auto lit = b;
    lit.right = b.left + std::max(b.height(), b.width() * level);
    cnv.fill_style(mClipped ? mTheme.Error() : (mPeakDb > -6.0 ? mTheme.Warning() : mTheme.Success()));
    cnv.fill_round_rect(lit, b.height() * 0.5f);
}

bool MeterBar::Set(double peakDb, bool clipped)
{
    // Quarter-decibel steps are invisible (a whole decibel, read as a number); skipping them
    // saves a redraw per frame.
    const bool changed = std::abs(peakDb - mPeakDb) >= (mVertical ? 1.0 : 0.25) || clipped != mClipped;
    if (changed)
    {
        mPeakDb = peakDb;
        mClipped = clipped;
    }
    return changed;
}

// ── ListRow, Surface, Backdrop ──────────────────────────────────────────────────

ListRow::ListRow(const Theme& theme, std::string text, std::string secondary, bool selected, bool heading,
                 std::function<void()> onClick)
    : mTheme(theme), mText(std::move(text)), mSecondary(std::move(secondary)), mSelected(selected), mHeading(heading),
      mOnClick(std::move(onClick))
{
}

el::view_limits ListRow::limits(const el::basic_context&) const
{
    const float h = mHeading ? 30.0f : metrics::kRowHeight;
    return {{160.0f, h}, {el::full_extent, h}};
}

void ListRow::draw(const el::context& ctx)
{
    auto& cnv = ctx.canvas;
    const auto b = ctx.bounds.inset(0.0f, 1.0f);

    if (mHeading)
    {
        FillText(cnv, mText, Font(Weight::SemiBold).size(11.0f), mTheme.TextMuted(), b.inset(12.0f, 0.0f), Text::Align::Left);
        return;
    }

    if (mSelected)
        cnv.fill_style(mTheme.AccentTint());
    else if (mHover)
        cnv.fill_style(mTheme.Elevated().opacity(0.7f));
    else
        cnv.fill_style(el::rgba(0, 0, 0, 0));
    cnv.fill_round_rect(b, metrics::kRadius);

    float secondaryWidth = 0.0f;
    const auto secondaryFont = Font(Weight::Regular).size(metrics::kSmallText);
    if (!mSecondary.empty())
    {
        secondaryWidth = el::measure_text(cnv, mSecondary, secondaryFont).x;
        FillText(cnv, mSecondary, secondaryFont, mTheme.TextMuted(), b.inset(12.0f, 0.0f), Text::Align::Right);
    }

    const auto textFont = Font(mSelected ? Weight::SemiBold : Weight::Medium).size(metrics::kTextSize);
    auto textBounds = b.inset(12.0f, 0.0f);
    textBounds.right -= secondaryWidth + (secondaryWidth > 0.0f ? 12.0f : 0.0f);
    if (mIcons != nullptr && !mIcon.empty())
    {
        const float mid = b.top + b.height() * 0.5f;
        mIcons->Draw(cnv, mIcon, el::rect{textBounds.left, mid - 9.0f, textBounds.left + 18.0f, mid + 9.0f}, mTint);
        textBounds.left += 28.0f;
    }
    FillText(cnv, Ellipsize(cnv, mText, textFont, textBounds.width()), textFont, mTheme.Text(), textBounds,
             Text::Align::Left);
}

ListRow& ListRow::Icon(Icons* icons, std::string name, el::color tint)
{
    mIcons = icons;
    mIcon = std::move(name);
    mTint = tint;
    return *this;
}

bool ListRow::click(const el::context& ctx, el::mouse_button btn)
{
    if (mHeading)
        return false;
    if (btn.state == el::mouse_button::right)
    {
        if (btn.down && onMenu)
            onMenu();
        return true;
    }
    if (btn.state != el::mouse_button::left)
        return false;
    if (btn.down)
    {
        mDown = true;
        return true;
    }
    const bool wasDown = mDown;
    mDown = false;
    if (wasDown && ctx.bounds.includes(btn.pos) && mOnClick)
        mOnClick();
    return true;
}

bool ListRow::cursor(const el::context& ctx, el::point, el::cursor_tracking status)
{
    const bool hover = status != el::cursor_tracking::leaving && !mHeading;
    if (hover != mHover)
    {
        mHover = hover;
        ctx.view.refresh(ctx);
    }
    return false;
}

void Surface::draw(const el::context& ctx)
{
    auto& cnv = ctx.canvas;
    cnv.fill_style(mFill);
    cnv.fill_round_rect(ctx.bounds, mRadius);
    cnv.stroke_style(mLine);
    cnv.line_width(1.0f);
    cnv.stroke_round_rect(ctx.bounds.inset(0.5f, 0.5f), mRadius);
}

void Backdrop::draw(const el::context& ctx)
{
    ctx.canvas.fill_style(el::rgba(0, 0, 0, 140));
    ctx.canvas.fill_rect(ctx.bounds);
}

bool Backdrop::click(const el::context& ctx, el::mouse_button btn)
{
    if (btn.state != el::mouse_button::left)
        return false;
    if (btn.down)
    {
        mDown = true;
        return true;
    }
    const bool wasDown = mDown;
    mDown = false;
    if (wasDown && mOnClick)
        mOnClick();
    (void)ctx;
    return true;
}

// ── Toast ───────────────────────────────────────────────────────────────────────

Toast::Toast(const Theme& theme, std::string title, std::string detail, bool isError)
    : mTheme(theme), mTitle(std::move(title)), mDetail(std::move(detail)), mError(isError)
{
}

el::view_limits Toast::limits(const el::basic_context&) const
{
    const float h = mDetail.empty() ? 48.0f : 68.0f;
    return {{480.0f, h}, {480.0f, h}};
}

void Toast::draw(const el::context& ctx)
{
    auto& cnv = ctx.canvas;
    const auto b = ctx.bounds;

    cnv.fill_style(mTheme.Elevated());
    cnv.fill_round_rect(b, 10.0f);
    cnv.stroke_style(mError ? mTheme.Error() : mTheme.BorderStrong());
    cnv.line_width(mError ? 2.0f : 1.0f);
    cnv.stroke_round_rect(b.inset(0.5f, 0.5f), 10.0f);

    const auto titleFont = Font(Weight::SemiBold).size(metrics::kTextSize);
    const auto detailFont = Font(Weight::Regular).size(metrics::kSmallText);
    auto inner = b.inset(16.0f, 0.0f);

    if (mDetail.empty())
    {
        FillText(cnv, Ellipsize(cnv, mTitle, titleFont, inner.width()), titleFont, mTheme.Text(), inner, Text::Align::Left);
        return;
    }

    auto top = inner;
    top.bottom = b.top + b.height() * 0.5f - 1.0f;
    auto bottom = inner;
    bottom.top = b.top + b.height() * 0.5f - 1.0f;
    bottom.bottom = b.bottom - 4.0f;
    FillText(cnv, Ellipsize(cnv, mTitle, titleFont, inner.width()), titleFont, mTheme.Text(), top, Text::Align::Left);
    FillText(cnv, Ellipsize(cnv, mDetail, detailFont, inner.width()), detailFont, mTheme.TextSecondary(), bottom, Text::Align::Left);
}

// ── Helpers ─────────────────────────────────────────────────────────────────────

el::element_ptr Panel(el::color fill, float radius, el::element_ptr content)
{
    return Rounded(fill, radius, std::move(content));
}

el::element_ptr Gap(float width, float height)
{
    return Space(width, height);
}
} // namespace nanoq::ui
