// The tuner: a dialog that stays open while the player plays, so what it shows is patched in
// place from the engine's feed rather than rebuilt. The metronome is in Metronome.cpp.

#include "ui/Drawn.h"
#include "ui/Shell.h"

#include "uiclient/Instruments.h"

#include <elements/support/text_utils.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace nanoq::ui
{
using guitarfx::uiclient::Topic;

namespace
{
using cycfi::artist::canvas;

std::string Fixed(double value, int decimals)
{
    char text[64];
    std::snprintf(text, sizeof(text), "%.*f", decimals, value);
    return text;
}

void DrawText(canvas& cnv, const std::string& text, const el::font_descr& font, el::color colour, el::point at,
              int align)
{
    auto state = cnv.new_state();
    cnv.font(font);
    cnv.fill_style(colour);
    cnv.text_align(align);
    cnv.fill_text(text, at);
}
} // namespace

// ── The tuner ───────────────────────────────────────────────────────────────────

/// The note, its neighbours, a needle over a ±50 cent scale, and the reading in numbers.
class TunerFace final : public el::element, public Drawn
{
  public:
    explicit TunerFace(const Theme& theme) : mTheme(theme)
    {
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{300.0f, 220.0f}, {el::full_extent, el::full_extent}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        auto area = ctx.bounds;
        const bool inTune = mReading.detected && mReading.inTune;
        const el::color noteColour =
            !mReading.detected ? mTheme.TextMuted() : (inTune ? el::rgba(0x40, 0xc0, 0x80, 255) : mTheme.Accent());

        // The note, with its neighbours either side.
        const float noteHeight = area.height() * 0.5f;
        const float noteSize = std::clamp(noteHeight * 0.7f, 34.0f, 110.0f);
        const el::point middle{area.left + area.width() * 0.5f, area.top + noteHeight * 0.5f};
        DrawText(cnv, mReading.detected ? mReading.note : std::string("-"), Font(Weight::SemiBold).size(noteSize),
                 noteColour, middle, canvas::center | canvas::middle);

        if (mReading.detected)
        {
            const auto side = Font(Weight::Regular).size(noteSize * 0.35f);
            DrawText(cnv, mReading.noteLeft, side, mTheme.TextMuted(), {area.left + area.width() * 0.15f, middle.y},
                     canvas::center | canvas::middle);
            DrawText(cnv, mReading.noteRight, side, mTheme.TextMuted(), {area.left + area.width() * 0.85f, middle.y},
                     canvas::center | canvas::middle);
        }

        // The scale: ±50 cents across, the centre marked.
        const float scaleTop = area.top + noteHeight;
        const float scaleHeight = std::min(60.0f, area.height() * 0.28f);
        const auto scale = el::rect{area.left + area.width() * 0.06f, scaleTop + 8.0f,
                                    area.right - area.width() * 0.06f, scaleTop + 8.0f + scaleHeight};
        const float cy = scale.top + scale.height() * 0.5f;

        cnv.fill_style(mTheme.Border());
        cnv.fill_round_rect(el::rect{scale.left, cy - 3.0f, scale.right, cy + 3.0f}, 3.0f);

        for (int cents = -50; cents <= 50; cents += 10)
        {
            const float x = scale.left + scale.width() * (0.05f + 0.9f * static_cast<float>(cents + 50) / 100.0f);
            const float tick = cents == 0 ? scale.height() : scale.height() * 0.4f;
            cnv.fill_style(cents == 0 ? mTheme.Text() : mTheme.TextMuted());
            cnv.fill_rect(el::rect{x - 1.0f, cy - tick * 0.5f, x + 1.0f, cy + tick * 0.5f});
        }

        if (mReading.detected)
        {
            const float x = scale.left + scale.width() * static_cast<float>(mReading.needle);
            cnv.fill_style(noteColour);
            cnv.fill_round_rect(el::rect{x - 4.0f, scale.top, x + 4.0f, scale.bottom}, 4.0f);
        }

        std::string detail = "Play a string";

        if (mReading.detected)
        {
            detail = std::string(mReading.cents >= 0 ? "+" : "") + Fixed(mReading.cents, 1) + " cents    " +
                     Fixed(mReading.frequency, 1) + " Hz";
        }

        DrawText(cnv, detail, Font(Weight::Medium).size(metrics::kTextSize), mTheme.TextSecondary(),
                 {area.left + area.width() * 0.5f, scale.bottom + 22.0f}, canvas::center | canvas::middle);
    }

    void Set(const guitarfx::uiclient::TunerModel::Reading& reading)
    {
        mReading = reading;
    }

  private:
    const Theme& mTheme;
    guitarfx::uiclient::TunerModel::Reading mReading;
};

void Shell::OpenTuner()
{
    auto& commands = mSession.Commands();
    auto face = std::make_shared<TunerFace>(mTheme);
    auto model = std::make_shared<guitarfx::uiclient::TunerModel>();

    auto mute = std::make_shared<Button>(
        mTheme, "Mute while tuning",
        [this, &commands] {
            commands.SetTunerLiveMode(!mState.tuner.liveMode); // live mode on means the sound plays
        },
        Button::Style::Plain);
    mute->Dot(true).MinWidth(190);

    auto reference = std::make_shared<Text>(mTheme, "A = 440 Hz", metrics::kTextSize, Weight::Medium);
    reference->Alignment(Text::Align::Centre).Colour(mTheme.TextSecondary());

    auto down = std::make_shared<Button>(
        mTheme, "\xE2\x88\x92",
        [this, &commands] {
            commands.SetTunerReference(std::clamp(std::round(mState.tuner.referenceFrequency) - 1.0, 400.0, 480.0));
        },
        Button::Style::Plain);
    auto up = std::make_shared<Button>(
        mTheme, "+",
        [this, &commands] {
            commands.SetTunerReference(std::clamp(std::round(mState.tuner.referenceFrequency) + 1.0, 400.0, 480.0));
        },
        Button::Style::Plain);
    down->MinWidth(44);
    up->MinWidth(44);

    auto sync = [this, face, model, mute, reference] {
        const auto& t = mState.tuner;
        face->Set(model->Push(t.detected, t.noteName, t.octave, t.centOffset, t.frequency));
        mute->On(!t.liveMode);
        reference->Set("A = " + Fixed(t.referenceFrequency, 0) + " Hz");
        mSession.Repaint(*face);
        mSession.Repaint(*mute);
        mSession.Repaint(*reference);
    };

    auto heading = std::make_shared<Text>(mTheme, "Tuner", 15.0f, Weight::SemiBold);
    auto close = std::make_shared<Button>(mTheme, "Done", [this] { CloseOverlay(); }, Button::Style::Primary);
    close->MinWidth(88);

    const auto view = ViewSize();
    ShowOverlay(Pad(16, 14, 16, 14,
                    Col({Row({FixedHeight(30, heading), Stretch(nullptr, 0.001), close}), face, VGap(8),
                         FixedHeight(44, Row({mute, Stretch(), down, FixedWidth(120, reference), up}))})),
                std::min(560.0f, std::max(300.0f, view.x - 48.0f)), std::min(420.0f, std::max(300.0f, view.y - 80.0f)));

    // The tuner listens only while it is open.
    mInstrumentSubs.push_back(mSession.Client().Subscribe(Topic::Tuner, sync));
    mOnOverlayClosed = [&commands] { commands.SetTunerActive(false); };
    commands.SetTunerActive(true);
    sync();
}
} // namespace nanoq::ui
