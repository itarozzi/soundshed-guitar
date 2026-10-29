#pragma once

// The frame every dialog shares, as the web UI draws its modals (css/modals/shell.css): a
// rounded card with a raised header band holding the title and a close cross, the body under
// it, and, when the dialog has actions, a raised footer band with its buttons at the right.
// Also the two small controls dialogs use for a yes or no: the checkbox and the switch.
// Shell::ShowDialog puts the pieces together.

#include "ui/ShellWidgets.h"

#include <string>
#include <vector>
#include <utility>

namespace nanoq::ui
{
inline constexpr float kDialogHeaderHeight = 56.0f;
inline constexpr float kDialogFooterHeight = 60.0f;
inline constexpr float kDialogRadius = 12.0f;

/// The card (.modal-content): the dialog's colour, the header band (.modal-header) with a rule
/// under it, and the footer band (.modal-footer) with a rule over it when `footer` is not 0.
class DialogFace final : public el::element
{
  public:
    DialogFace(const Theme& theme, float footer, float header = kDialogHeaderHeight)
        : mTheme(theme), mHeader(header), mFooter(footer)
    {
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        cnv.fill_style(mTheme.ModalBg());
        cnv.fill_round_rect(b, kDialogRadius);
        {
            auto state = cnv.new_state();
            cnv.add_round_rect(b, kDialogRadius);
            cnv.clip();
            cnv.fill_style(mTheme.ModalRaised());

            if (mHeader > 0.0f)
            {
                cnv.fill_rect(el::rect{b.left, b.top, b.right, b.top + mHeader});
            }

            if (mFooter > 0.0f)
            {
                cnv.fill_rect(el::rect{b.left, b.bottom - mFooter, b.right, b.bottom});
            }
        }
        cnv.fill_style(mTheme.ModalRule());

        if (mHeader > 0.0f)
        {
            cnv.fill_rect(el::rect{b.left, b.top + mHeader - 1.0f, b.right, b.top + mHeader});
        }

        if (mFooter > 0.0f)
        {
            cnv.fill_rect(el::rect{b.left, b.bottom - mFooter, b.right, b.bottom - mFooter + 1.0f});
        }

        cnv.stroke_style(mTheme.ModalBorder());
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(b.inset(0.5f, 0.5f), kDialogRadius);
    }

  private:
    const Theme& mTheme;
    float mHeader, mFooter;
};

/// The header's close cross (.modal-close): muted, on a faint square under the pointer.
class CloseCross final : public Pressable
{
  public:
    explicit CloseCross(const Theme& theme) : mTheme(theme)
    {
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{28.0f, 28.0f}, {28.0f, 28.0f}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;

        if (mHover)
        {
            cnv.fill_style(mTheme.ModalText().opacity(0.08f));
            cnv.fill_round_rect(b, 6.0f);
        }

        const auto c = cycfi::artist::center_point(b);
        constexpr float kArm = 4.5f;
        cnv.stroke_style(mHover ? mTheme.ModalText() : mTheme.ModalTextMuted());
        cnv.line_width(2.0f);
        cnv.line_cap(cycfi::artist::canvas::round);
        cnv.begin_path();
        cnv.move_to(c.x - kArm, c.y - kArm);
        cnv.line_to(c.x + kArm, c.y + kArm);
        cnv.move_to(c.x + kArm, c.y - kArm);
        cnv.line_to(c.x - kArm, c.y + kArm);
        cnv.stroke();
    }

  private:
    const Theme& mTheme;
};

/// A checkbox with its label (an input type=checkbox in the web UI): a rounded square, filled
/// with the accent and ticked when checked, and the label after it. A tap anywhere on it
/// flips it; `onClick` decides what that means, then the owner calls Set().
class CheckBox final : public Pressable
{
  public:
    CheckBox(const Theme& theme, std::string label, float size = metrics::kSmallText)
        : mTheme(theme), mLabel(std::move(label)), mSize(size)
    {
    }

    void Set(bool checked)
    {
        mChecked = checked;
    }

    [[nodiscard]] bool Checked() const
    {
        return mChecked;
    }

    el::view_limits limits(const el::basic_context& ctx) const override
    {
        const float w = 24.0f + el::measure_text(ctx.canvas, mLabel, Font(Weight::Regular).size(mSize)).x;
        return {{w, 22.0f}, {w, 22.0f}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const float fade = mEnabled ? 1.0f : 0.5f;
        const float cy = b.top + b.height() * 0.5f;
        const el::rect box{b.left + 0.5f, cy - 8.0f, b.left + 16.5f, cy + 8.0f};

        if (mChecked)
        {
            cnv.fill_style(mTheme.Accent().level(mHover ? 1.08f : 1.0f).opacity(fade));
            cnv.fill_round_rect(box, 4.0f);
            cnv.stroke_style(el::rgba(255, 255, 255, 245).opacity(fade));
            cnv.line_width(2.0f);
            cnv.line_cap(cycfi::artist::canvas::round);
            cnv.line_join(cycfi::artist::canvas::round_join);
            cnv.begin_path();
            cnv.move_to(box.left + 4.0f, cy + 0.5f);
            cnv.line_to(box.left + 7.0f, cy + 3.5f);
            cnv.line_to(box.left + 12.0f, cy - 3.0f);
            cnv.stroke();
        }
        else
        {
            cnv.fill_style(mTheme.Bar().opacity(fade));
            cnv.fill_round_rect(box, 4.0f);
            cnv.stroke_style((mHover && mEnabled ? mTheme.Accent() : mTheme.BorderStrong()).opacity(fade));
            cnv.line_width(1.0f);
            cnv.stroke_round_rect(box, 4.0f);
        }

        const auto font = Font(Weight::Regular).size(mSize);
        const el::rect text{b.left + 24.0f, b.top, b.right, b.bottom};
        FillText(cnv, Ellipsize(cnv, mLabel, font, text.width()), font, mTheme.ModalText().opacity(fade), text,
                 HAlign::Left);
    }

  private:
    const Theme& mTheme;
    std::string mLabel;
    float mSize;
    bool mChecked = false;
};

/// A picker's trigger (the web UI's .metro-select-trigger and themed selects): the choice in a filled box with a
/// chevron at the right, its border lit under the pointer. A tap opens the list under it.
class PickerField final : public Pressable
{
  public:
    PickerField(const Theme& theme, Icons& icons) : mTheme(theme), mIcons(icons)
    {
    }

    void Set(std::string text)
    {
        mText = std::move(text);
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{60.0f, 32.0f}, {el::full_extent, 32.0f}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const float fade = mEnabled ? 1.0f : 0.6f;
        cnv.fill_style(mTheme.Tertiary().opacity(fade));
        cnv.fill_round_rect(b, 8.0f);
        cnv.stroke_style((mHover && mEnabled ? mTheme.Accent() : mTheme.BorderStrong()).opacity(fade));
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(b.inset(0.5f, 0.5f), 8.0f);
        const auto font = Font(Weight::Regular).size(metrics::kSmallText);
        const el::rect text{b.left + 10.0f, b.top, b.right - 30.0f, b.bottom};
        FillText(cnv, Ellipsize(cnv, mText, font, text.width()), font, mTheme.Text().opacity(fade), text, HAlign::Left);
        const float cy = b.top + b.height() * 0.5f;
        mIcons.Draw(cnv, "ui-caret", el::rect{b.right - 24.0f, cy - 7.0f, b.right - 10.0f, cy + 7.0f},
                    mTheme.TextMuted().opacity(fade));
    }

  private:
    const Theme& mTheme;
    Icons& mIcons;
    std::string mText;
};

/// A plain on/off switch (.toggle-switch): a 36 by 20 pill, the accent when on, with its white
/// thumb at that end.
class ToggleSwitch final : public Pressable
{
  public:
    explicit ToggleSwitch(const Theme& theme, bool on = false) : mTheme(theme), mOn(on)
    {
    }

    void Set(bool on)
    {
        mOn = on;
    }

    [[nodiscard]] bool IsOn() const
    {
        return mOn;
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{36.0f, 20.0f}, {36.0f, 20.0f}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const float fade = mEnabled ? 1.0f : 0.5f;
        const float radius = b.height() * 0.5f;
        cnv.fill_style(
            (mOn ? mTheme.Accent().level(mHover ? 1.05f : 0.92f) : mTheme.ToggleBg().level(mHover ? 1.2f : 1.0f))
                .opacity(fade));
        cnv.fill_round_rect(b, radius);
        cnv.fill_style(el::rgba(255, 255, 255, 240).opacity(fade));
        cnv.add_circle(
            cycfi::artist::circle(el::point{mOn ? b.right - radius : b.left + radius, b.top + radius}, radius - 3.0f));
        cnv.fill();
    }

  private:
    const Theme& mTheme;
    bool mOn;
};

/// Lines of code in a fixed-pitch face (the web UI's pre blocks): a preset's JSON, a report.
/// As tall as its lines; put it in a scroller.
class CodeBlock final : public el::element
{
  public:
    explicit CodeBlock(const Theme& theme) : mTheme(theme)
    {
    }

    void Set(const std::string& text)
    {
        mLines.clear();
        std::size_t start = 0;

        while (start <= text.size())
        {
            const auto end = text.find('\n', start);
            mLines.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));

            if (end == std::string::npos)
            {
                break;
            }

            start = end + 1;
        }
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        const float h = kLine * static_cast<float>(mLines.size()) + 16.0f;
        return {{0.0f, h}, {el::full_extent, h}};
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        cnv.fill_style(mTheme.Bar());
        cnv.fill_round_rect(b, 8.0f);
        const auto font = MonoFont().size(11.0f);
        const auto visible = ctx.view_bounds(); // only the lines on screen are drawn
        float y = b.top + 8.0f;

        for (const auto& line : mLines)
        {
            if (y + kLine >= visible.top && y <= visible.bottom)
            {
                FillText(cnv, line, font, mTheme.ModalTextSecondary(),
                         el::rect{b.left + 10.0f, y, b.right - 6.0f, y + kLine}, HAlign::Left);
            }

            y += kLine;
        }
    }

  private:
    static constexpr float kLine = 15.0f;
    const Theme& mTheme;
    std::vector<std::string> mLines;
};
} // namespace nanoq::ui
