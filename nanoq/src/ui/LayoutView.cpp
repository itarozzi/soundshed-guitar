// A custom effect layout drawn and played (see LayoutView.h). The web UI's layoutRenderer.ts
// builds it from positioned elements scaled by a CSS transform; here one canvas holds them, laid
// out at their design positions (PlacedLayer) and scaled as one piece (ScaledElement), so a
// control's drag, a click and a redraw all pass through the same transform.

#include "ui/LayoutView.h"

#include "ui/TextDraw.h"
#include "ui/Widgets.h"
#include "uiclient/ParamFormat.h"

#include <algorithm>
#include <cmath>

namespace nanoq::ui
{
using namespace guitarfx::uiclient;

namespace
{
constexpr float kPi = 3.14159265f;
constexpr float kDragPixels = 200.0f; // a knob's full travel, as the web UI's and ParamKnob's
constexpr float kCornerRadius = 8.0f;

/// Draws its subject at a scale (x and y apart, as a CSS scale() may be), from its top-left
/// corner, optionally clipped to a rounded rectangle of the subject's own size. Points and
/// bounds are mapped on the way in, so the subject handles events in its own coordinates and a
/// refresh it asks for lands where it is on screen (the view maps it through the canvas).
template <el::concepts::Element Subject>
class ScaledElement : public el::proxy<Subject>
{
public:
    using base_type = el::proxy<Subject>;

    ScaledElement(Subject subject, float sx, float sy, float radius)
        : base_type(std::move(subject)), mSx(sx), mSy(sy), mRadius(radius)
    {
    }

    el::view_limits limits(const el::basic_context& ctx) const override
    {
        auto l = this->subject().limits(ctx);
        l.min.x *= mSx;
        l.min.y *= mSy;
        l.max.x = std::min(l.max.x * mSx, el::full_extent);
        l.max.y = std::min(l.max.y * mSy, el::full_extent);
        return l;
    }

    void draw(const el::context& ctx) override
    {
        mDrawing = true;
        base_type::draw(ctx);
        mDrawing = false;
    }

    void prepare_subject(el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        cnv.save();
        cnv.translate(b.left, b.top);
        cnv.scale(mSx, mSy);
        ctx.bounds = el::rect{0.0f, 0.0f, b.width() / mSx, b.height() / mSy};
        // Clipped only while drawing: the canvas an event is handled with is not drawing, and
        // Direct2D clips only between the begin and end of a draw.
        if (mRadius > 0.0f && mDrawing)
        {
            cnv.begin_path();
            cnv.add_round_rect(ctx.bounds, mRadius);
            cnv.clip();
        }
    }

    void prepare_subject(el::context& ctx, el::point& p) override
    {
        const auto b = ctx.bounds;
        p = el::point{(p.x - b.left) / mSx, (p.y - b.top) / mSy};
        prepare_subject(ctx);
    }

    void restore_subject(el::context& ctx) override
    {
        ctx.canvas.restore();
    }

private:
    float mSx, mSy, mRadius;
    bool mDrawing = false;
};

Ptr Scaled(Ptr content, float sx, float sy, float radius)
{
    return el::share(ScaledElement(el::hold_any(std::move(content)), sx, sy, radius));
}

/// Children at fixed rectangles from its top-left corner, drawn in the order added: the layout's
/// absolute positioning.
class PlacedLayer final : public el::vector_composite<el::layer_element>
{
public:
    PlacedLayer(float w, float h) : mW(w), mH(h) {}

    void Add(Ptr child, el::rect at)
    {
        push_back(std::move(child));
        mRects.push_back(at);
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{mW, mH}, {mW, mH}};
    }

    el::rect bounds_of(const el::context& ctx, std::size_t index) const override
    {
        const auto r = mRects[index];
        return {ctx.bounds.left + r.left, ctx.bounds.top + r.top, ctx.bounds.left + r.right, ctx.bounds.top + r.bottom};
    }

private:
    float mW, mH;
    std::vector<el::rect> mRects;
};

el::color ToColour(const LayoutColour& c, double opacity = 1.0)
{
    const double a = std::clamp(c.a * opacity, 0.0, 1.0);
    return el::rgba(c.r, c.g, c.b, static_cast<std::uint8_t>(std::lround(a * 255.0)));
}

/// A point in view coordinates (where menus are placed), from one in a scaled layout's.
el::point ToView(const el::context& ctx, el::point p)
{
    const auto device = ctx.canvas.user_to_device(p);
    const float scale = ctx.view.scale() > 0.0f ? ctx.view.scale() : 1.0f;
    return {device.x / scale, device.y / scale};
}

/// The text inks a layout's container theme gives, or the app theme's when it sets none.
struct Inks
{
    el::color label, value, text;
};

Inks InksFor(const Theme& theme, const std::string& containerTheme)
{
    if (containerTheme == "light")
        return {el::rgba(70, 74, 86, 255), el::rgba(96, 100, 112, 255), el::rgba(24, 26, 32, 255)};
    if (containerTheme == "dark" || containerTheme == "classic")
        return {el::rgba(201, 209, 225, 210), el::rgba(160, 168, 186, 230), el::rgba(238, 242, 255, 255)};
    return {theme.Pick(el::rgba(201, 209, 225, 184), theme.TextSecondary()), theme.TextMuted(), theme.Text()};
}

void DrawImageInto(artist::canvas& cnv, const artist::image& image, el::rect box, const std::string& fit)
{
    if (fit == "cover")
        DrawImageCovering(cnv, image, box);
    else if (fit == "stretch")
        cnv.draw(image, box);
    else
        DrawImageFitted(cnv, image, box);
}

void DrawBackgroundImage(artist::canvas& cnv, const artist::image& image, const LayoutBackground& bg, el::rect canvasBox)
{
    const auto size = image.size();
    if (size.x <= 0.0f || size.y <= 0.0f)
        return;
    const float w = canvasBox.width(), h = canvasBox.height();
    // CSS background-position: the top-left corner moved by the offsets, for every size mode.
    const float x = canvasBox.left + static_cast<float>(bg.offsetX);
    const float y = canvasBox.top + static_cast<float>(bg.offsetY);

    if (bg.size == "tile")
    {
        // Repeated both ways from the offset at the picture's own size, covering the canvas.
        const float startX = x - std::ceil((x - canvasBox.left) / size.x) * size.x;
        const float startY = y - std::ceil((y - canvasBox.top) / size.y) * size.y;
        for (float ty = startY; ty < canvasBox.bottom; ty += size.y)
            for (float tx = startX; tx < canvasBox.right; tx += size.x)
                cnv.draw(image, el::rect{tx, ty, tx + size.x, ty + size.y});
        return;
    }

    float dw = size.x, dh = size.y; // "auto": its own size, as CSS draws a size it cannot read
    if (bg.size == "cover" || bg.size == "contain")
    {
        const float s = bg.size == "cover" ? std::max(w / size.x, h / size.y) : std::min(w / size.x, h / size.y);
        dw = size.x * s;
        dh = size.y * s;
    }
    else if (bg.size == "stretch")
    {
        dw = w;
        dh = h;
    }
    else if (bg.size == "custom" && bg.scale)
    {
        // A percentage of the canvas's width, the height keeping the picture's aspect.
        dw = static_cast<float>(*bg.scale) * w;
        dh = dw * size.y / size.x;
    }
    cnv.draw(image, el::rect{x, y, x + dw, y + dh});
}

/// A rectangle overlay's paint: its fill (a hex colour takes the opacity, as colorWithAlpha
/// applies it), its picture, and its border inside the edge.
void DrawOverlay(artist::canvas& cnv, LayoutImages& images, const LayoutOverlay& overlay, el::rect r)
{
    const float radius = std::min(static_cast<float>(overlay.borderRadius), std::min(r.width(), r.height()) * 0.5f);
    auto state = cnv.new_state();
    // An unreadable colour falls back to black, which is hex, so it takes the opacity too.
    const auto parsed = ParseLayoutColour(overlay.backgroundColor);
    const auto first = overlay.backgroundColor.find_first_not_of(" \t");
    const bool hex = !parsed || (first != std::string::npos && overlay.backgroundColor[first] == '#');
    cnv.fill_style(ToColour(parsed.value_or(LayoutColour{}), hex ? overlay.backgroundOpacity : 1.0));
    cnv.begin_path();
    cnv.add_round_rect(r, radius);
    cnv.fill();

    if (!overlay.imageId.empty())
        if (const auto image = images.Find(overlay.imageId))
        {
            auto clip = cnv.new_state();
            cnv.begin_path();
            cnv.add_round_rect(r, radius);
            cnv.clip();
            DrawImageInto(cnv, *image, r, overlay.imageFit);
        }

    if (overlay.borderWidth > 0.0)
    {
        const float bw = static_cast<float>(overlay.borderWidth);
        cnv.stroke_style(ToColour(ParseLayoutColour(overlay.borderColor).value_or(LayoutColour{255, 255, 255, 1.0})));
        cnv.line_width(bw);
        cnv.begin_path();
        const el::rect inner{r.left + bw * 0.5f, r.top + bw * 0.5f, r.right - bw * 0.5f, r.bottom - bw * 0.5f};
        cnv.add_round_rect(inner, std::max(0.0f, radius - bw * 0.5f));
        cnv.stroke();
    }
}

/// The canvas under the controls: background layers, bottom first, and the rectangles that are
/// not bypass switches. It takes no events.
class LayoutBackdropView final : public el::element
{
public:
    LayoutBackdropView(LayoutImages& images, std::shared_ptr<const EffectLayout> layout, bool bypassed)
        : mImages(images), mLayout(std::move(layout)), mBypassed(bypassed)
    {
    }

    el::element* hit_test(const el::context&, el::point, bool, bool) override
    {
        return nullptr;
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        for (const auto& bg : mLayout->backgrounds)
            DrawLayer(cnv, bg, b);
        for (const auto& overlay : mLayout->overlays)
            if (!overlay.toggleBypassOnClick && LayoutOverlayVisible(overlay, mBypassed))
                DrawOverlay(cnv, mImages, overlay, Place(b, overlay));
    }

    static el::rect Place(el::rect b, const LayoutOverlay& overlay)
    {
        const float x = b.left + std::round(static_cast<float>(overlay.position.x));
        const float y = b.top + std::round(static_cast<float>(overlay.position.y));
        return {x, y, x + std::round(static_cast<float>(overlay.size.width)), y + std::round(static_cast<float>(overlay.size.height))};
    }

private:
    void DrawLayer(artist::canvas& cnv, const LayoutBackground& bg, el::rect b)
    {
        const double opacity = std::clamp(bg.opacity, 0.0, 1.0);
        auto state = cnv.new_state();
        if (bg.type == "image")
        {
            // Artist draws pictures without an opacity, so a faded picture is drawn whole and
            // one faded out entirely is left out.
            if (opacity <= 0.01)
                return;
            if (const auto image = mImages.Find(bg.value))
                DrawBackgroundImage(cnv, *image, bg, b);
            return;
        }
        const auto paint = ParseLayoutPaint(bg.value);
        if (paint.kind == LayoutPaint::Kind::Solid)
        {
            cnv.fill_style(ToColour(paint.colour, opacity));
            cnv.fill_rect(b);
        }
        else if (paint.kind == LayoutPaint::Kind::Linear && !paint.stops.empty())
        {
            double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            LinearGradientLine(paint.angleDegrees, b.width(), b.height(), x0, y0, x1, y1);
            artist::canvas::linear_gradient gradient{el::point{b.left + static_cast<float>(x0), b.top + static_cast<float>(y0)},
                                                     el::point{b.left + static_cast<float>(x1), b.top + static_cast<float>(y1)}};
            for (const auto& stop : paint.stops)
                gradient.add_color_stop(static_cast<float>(stop.offset), ToColour(stop.colour, opacity));
            cnv.fill_style(gradient);
            cnv.fill_rect(b);
        }
    }

    LayoutImages& mImages;
    std::shared_ptr<const EffectLayout> mLayout;
    bool mBypassed;
};

/// The layout's own text, over the controls. It takes no events.
class LayoutTextView final : public el::element
{
public:
    LayoutTextView(std::shared_ptr<const EffectLayout> layout, el::color ink) : mLayout(std::move(layout)), mInk(ink) {}

    el::element* hit_test(const el::context&, el::point, bool, bool) override
    {
        return nullptr;
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        for (const auto& label : mLayout->textLabels)
        {
            if (label.text.empty())
                continue;
            // A span shrinks to its text, so its alignment moves nothing: each starts at its position.
            std::string text = label.text;
            std::replace(text.begin(), text.end(), '\n', ' ');
            const float size = std::clamp(static_cast<float>(label.fontSize), 1.0f, 512.0f);
            const auto font = Font(label.bold ? Weight::SemiBold : Weight::Regular).size(size);
            const float x = b.left + std::round(static_cast<float>(label.position.x));
            const float y = b.top + std::round(static_cast<float>(label.position.y));
            const auto colour = ParseLayoutColour(label.color);
            FillText(cnv, text, font, colour ? ToColour(*colour) : mInk, el::rect{x, y, std::max(b.right, x + 4096.0f), y + size * 1.25f},
                     HAlign::Left);
        }
    }

private:
    std::shared_ptr<const EffectLayout> mLayout;
    el::color mInk;
};

/// A rectangle that switches the effect's bypass: always there to click, drawn only while its
/// visibility mode says so, over everything else.
class BypassHotspot final : public el::element
{
public:
    BypassHotspot(LayoutImages& images, LayoutOverlay overlay, bool visible, std::function<void()> onClick)
        : mImages(images), mOverlay(std::move(overlay)), mVisible(visible), mOnClick(std::move(onClick))
    {
    }

    bool wants_control() const override
    {
        return true;
    }

    void draw(const el::context& ctx) override
    {
        if (mVisible)
            DrawOverlay(ctx.canvas, mImages, mOverlay, ctx.bounds);
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (btn.state != el::mouse_button::left)
            return false;
        if (btn.down)
            mDown = true;
        else if (mDown && ctx.bounds.includes(btn.pos))
        {
            mDown = false;
            if (mOnClick)
                mOnClick();
        }
        return true;
    }

private:
    LayoutImages& mImages;
    LayoutOverlay mOverlay;
    bool mVisible;
    std::function<void()> mOnClick;
    bool mDown = false;
};

float LabelLine(float size)
{
    return std::ceil(size * 1.25f);
}

float ValueLine(float size)
{
    return std::ceil(size * 1.3f);
}

/// The control itself, by kind: a knob's diameter, a switch's size, a slider's length and the
/// height its margins give it (layoutControlImages.ts).
el::extent ControlExtent(LayoutControlKind kind, const LayoutControlStyle& style, bool imageToggle, bool imageSlider)
{
    switch (kind)
    {
    case LayoutControlKind::Toggle:
        if (imageToggle)
        {
            const auto size = LayoutToggleSize(style);
            return {static_cast<float>(size.width), static_cast<float>(size.height)};
        }
        return {36.0f, 20.0f};
    case LayoutControlKind::Slider:
        return {static_cast<float>(LayoutSliderLength(style)), imageSlider ? 32.0f : 20.0f};
    default:
    {
        const float d = static_cast<float>(LayoutKnobDiameter(style));
        return {d, d};
    }
    }
}
} // namespace

// ── LayoutImages ───────────────────────────────────────────────────────────────────────

std::shared_ptr<artist::image> LayoutImages::Find(const std::string& imageId)
{
    if (imageId.empty())
        return nullptr;
    if (const auto it = mImages.find(imageId); it != mImages.end())
        return it->second;

    std::shared_ptr<artist::image> image;
    if (const auto file = mResolve ? mResolve(imageId) : std::nullopt)
    {
        try
        {
            image = std::make_shared<artist::image>(*file);
            if (image->size().x <= 0.0f || image->size().y <= 0.0f)
                image.reset();
        }
        catch (...)
        {
            image.reset(); // a format the backend cannot decode (an SVG) draws nothing
        }
    }
    mImages[imageId] = image;
    return image;
}

// ── LayoutControlView ──────────────────────────────────────────────────────────────────

LayoutControlView::LayoutControlView(const Theme& theme, LayoutImages& images, EffectParamInfo param, Look look,
                                     const LayoutActions& actions)
    : mTheme(theme), mImages(images), mParam(std::move(param)), mLook(std::move(look)), mActions(actions)
{
    mValue = mParam.defaultValue;
    mPosition = ParamValueToPosition(mParam, mValue);
}

el::extent LayoutControlView::Size() const
{
    float h = mLook.control.y;
    if (mLook.labelTop)
        h += LabelLine(mLook.labelSize) + 4.0f;
    if (mLook.labelBottom)
        h += 4.0f + LabelLine(mLook.labelSize);
    if (mLook.style.showValue)
        h += 2.0f + ValueLine(mLook.valueSize);
    return {mLook.width, h};
}

el::view_limits LayoutControlView::limits(const el::basic_context&) const
{
    const auto s = Size();
    return {{s.x, s.y}, {s.x, s.y}};
}

el::rect LayoutControlView::ControlBox(el::rect b) const
{
    const float top = b.top + (mLook.labelTop ? LabelLine(mLook.labelSize) + 4.0f : 0.0f);
    const float cx = (b.left + b.right) * 0.5f;
    return {cx - mLook.control.x * 0.5f, top, cx + mLook.control.x * 0.5f, top + mLook.control.y};
}

el::rect LayoutControlView::ValueBox(el::rect b) const
{
    if (!mLook.style.showValue)
        return {};
    return {b.left, b.bottom - ValueLine(mLook.valueSize), b.right, b.bottom};
}

el::element* LayoutControlView::hit_test(const el::context& ctx, el::point p, bool, bool)
{
    // The control and its value answer; the label, as in the web UI, does not.
    return ControlBox(ctx.bounds).includes(p) || ValueBox(ctx.bounds).includes(p) ? this : nullptr;
}

void LayoutControlView::draw(const el::context& ctx)
{
    auto& cnv = ctx.canvas;
    const auto b = ctx.bounds;
    const auto labelFont = Font(Weight::SemiBold).size(mLook.labelSize);
    const float labelWidth = std::min(b.width(), mLook.labelMaxWidth);
    const float cx = (b.left + b.right) * 0.5f;
    const auto box = ControlBox(b);

    if (mLook.labelTop)
        FillText(cnv, Ellipsize(cnv, mLook.label, labelFont, labelWidth), labelFont, mLook.labelInk,
                 el::rect{cx - labelWidth * 0.5f, b.top, cx + labelWidth * 0.5f, b.top + LabelLine(mLook.labelSize)}, HAlign::Centre);

    switch (mLook.kind)
    {
    case LayoutControlKind::Toggle:
        DrawToggle(cnv, box);
        break;
    case LayoutControlKind::Slider:
        DrawSlider(cnv, box);
        break;
    default:
        DrawKnob(cnv, box);
        break;
    }

    if (mLook.labelBottom)
        FillText(cnv, Ellipsize(cnv, mLook.label, labelFont, labelWidth), labelFont, mLook.labelInk,
                 el::rect{cx - labelWidth * 0.5f, box.bottom + 4.0f, cx + labelWidth * 0.5f, box.bottom + 4.0f + LabelLine(mLook.labelSize)},
                 HAlign::Centre);

    if (mLook.style.showValue)
    {
        const auto valueFont = Font(Weight::SemiBold).size(mLook.valueSize);
        FillText(cnv, Ellipsize(cnv, FormatParamValue(mParam, mValue), valueFont, b.width()), valueFont, mLook.valueInk, ValueBox(b),
                 HAlign::Centre);
    }
}

void LayoutControlView::DrawKnob(artist::canvas& cnv, el::rect box)
{
    const float d = box.width();
    const float r = d * 0.5f;
    const el::point c{(box.left + box.right) * 0.5f, (box.top + box.bottom) * 0.5f};
    const auto& style = mLook.style;
    const float position = static_cast<float>(std::clamp(mPosition, 0.0, 1.0));

    // An image knob (a turning face or a filmstrip, with its base and glare) wins, then a custom
    // knob's turning face; either one whose face cannot be drawn is drawn as a styled knob.
    if (const auto* knobImage = UsableKnobImage(style))
        if (const auto face = mImages.Find(knobImage->imageId))
        {
            if (const auto base = mImages.Find(knobImage->baseImageId))
                DrawImageFitted(cnv, *base, box);
            if (knobImage->mode == "filmstrip")
            {
                const int frame = FilmstripFrame(position, knobImage->frames);
                const auto cell = FilmstripCellOf(frame, knobImage->frames, knobImage->columns);
                const auto size = face->size();
                const float cw = size.x / static_cast<float>(cell.columns), ch = size.y / static_cast<float>(cell.rows);
                cnv.draw(*face, el::rect{cell.column * cw, cell.row * ch, (cell.column + 1) * cw, (cell.row + 1) * ch}, box);
            }
            else
            {
                auto state = cnv.new_state();
                const el::rect faceBox = knobImage->clip ? el::rect{box.left + 4.0f, box.top + 4.0f, box.right - 4.0f, box.bottom - 4.0f} : box;
                if (knobImage->clip)
                {
                    cnv.begin_path();
                    cnv.add_circle(artist::circle(c, faceBox.width() * 0.5f));
                    cnv.clip();
                }
                cnv.translate(c.x, c.y);
                cnv.rotate(static_cast<float>(KnobFaceAngle(position, knobImage->minAngle, knobImage->maxAngle)) * kPi / 180.0f);
                const float half = faceBox.width() * 0.5f;
                DrawImageFitted(cnv, *face, el::rect{-half, -half, half, half});
            }
            if (const auto glare = mImages.Find(knobImage->glareImageId))
                DrawImageFitted(cnv, *glare, box);
            return;
        }

    if (style.knobStyle == "custom" && !style.knobImageId.empty() && !UsableKnobImage(style))
        if (const auto face = mImages.Find(style.knobImageId))
        {
            auto state = cnv.new_state();
            const float half = r - 4.0f;
            cnv.begin_path();
            cnv.add_circle(artist::circle(c, half));
            cnv.clip();
            cnv.translate(c.x, c.y);
            cnv.rotate((position * 270.0f - 135.0f) * kPi / 180.0f);
            DrawImageFitted(cnv, *face, el::rect{-half, -half, half, half});
            return;
        }

    // A styled knob (custom-layouts.css), its pointer at the position on a 270 degree sweep.
    const float angle = (-135.0f + position * 270.0f - 90.0f) * kPi / 180.0f;
    const float ca = std::cos(angle), sa = std::sin(angle);
    const float k = d / 38.0f; // the styles are drawn for a 38 px knob
    auto pointer = [&](el::color ink, float width, float from, float to) {
        auto state = cnv.new_state();
        cnv.stroke_style(ink);
        cnv.line_width(width * k);
        cnv.line_cap(artist::canvas::round);
        cnv.begin_path();
        cnv.move_to(c.x + ca * from * k, c.y + sa * from * k);
        cnv.line_to(c.x + ca * to * k, c.y + sa * to * k);
        cnv.stroke();
    };
    auto body = [&](el::color fill, el::color edge, float edgeWidth) {
        cnv.fill_style(fill);
        cnv.begin_path();
        cnv.add_circle(artist::circle(c, r - edgeWidth * 0.5f));
        cnv.fill();
        cnv.stroke_style(edge);
        cnv.line_width(edgeWidth);
        cnv.begin_path();
        cnv.add_circle(artist::circle(c, r - edgeWidth * 0.5f));
        cnv.stroke();
    };

    if (style.knobStyle == "pedal")
    {
        body(el::rgba(200, 202, 210, 255), el::rgba(136, 136, 136, 255), 1.0f);
        pointer(el::rgba(51, 51, 51, 255), 2.0f, 6.0f, 13.0f);
        return;
    }
    if (style.knobStyle == "amp")
    {
        body(el::rgba(47, 48, 56, 255), el::rgba(102, 102, 102, 255), 1.0f);
        pointer(el::rgba(240, 192, 64, 255), 2.0f, 6.0f, 13.0f);
        return;
    }
    if (style.knobStyle == "minimal")
    {
        cnv.stroke_style(el::rgba(255, 255, 255, 77));
        cnv.line_width(2.0f * k);
        cnv.begin_path();
        cnv.add_circle(artist::circle(c, r - k));
        cnv.stroke();
        pointer(mLook.accent, 2.0f, 2.0f, 15.0f);
        return;
    }

    // The default: Nano Q's own knob face, a travel ring round a dark cap, at this size.
    const float ring = 3.0f * k;
    const float arcR = r - ring * 0.5f;
    {
        auto state = cnv.new_state();
        cnv.line_width(ring);
        cnv.stroke_style(mTheme.KnobRingTrack());
        cnv.begin_path();
        cnv.arc(c.x, c.y, arcR, 0.0f, 2.0f * kPi);
        cnv.stroke();
        if (position > 0.002f)
        {
            cnv.stroke_style(mTheme.KnobRingEnd());
            cnv.begin_path();
            cnv.arc(c.x, c.y, arcR, kPi * 0.75f, kPi * 0.75f + kPi * 1.5f * position);
            cnv.stroke();
        }
    }
    const float capR = r - ring - 1.5f * k;
    {
        auto state = cnv.new_state();
        const bool light = mTheme.IsLight();
        artist::canvas::radial_gradient dome{el::point{c.x, c.y - capR * 0.4f}, 0.0f, c, capR};
        const auto top = light ? el::rgba(250, 250, 252, 255) : el::rgba(58, 58, 68, 255);
        dome.add_color_stop(0.0f, (mDragging || mHover) ? top.level(1.15f) : top);
        dome.add_color_stop(1.0f, light ? el::rgba(214, 216, 224, 255) : el::rgba(22, 22, 28, 255));
        cnv.fill_style(dome);
        cnv.begin_path();
        cnv.add_circle(artist::circle(c, capR));
        cnv.fill();
    }
    pointer(mTheme.IsLight() ? mTheme.Text() : el::rgba(240, 240, 248, 255), 2.0f, capR * 0.40f / k, capR * 0.85f / k);
}

void LayoutControlView::DrawToggle(artist::canvas& cnv, el::rect box)
{
    const bool on = mValue >= 0.5;
    const auto& style = mLook.style;
    // An image switch needs both of its pictures; with one missing it is the standard switch.
    if (!style.toggleOnImageId.empty() && !style.toggleOffImageId.empty())
    {
        const auto onImage = mImages.Find(style.toggleOnImageId);
        const auto offImage = mImages.Find(style.toggleOffImageId);
        if (onImage && offImage)
        {
            DrawImageFitted(cnv, on ? *onImage : *offImage, box);
            return;
        }
    }
    const el::rect track{box.left, box.top + (box.height() - 20.0f) * 0.5f, box.left + 36.0f, box.top + (box.height() + 20.0f) * 0.5f};
    cnv.fill_style(on ? mTheme.Accent().level(mHover ? 1.05f : 0.92f) : mTheme.ToggleBg().level(mHover ? 1.15f : 1.0f));
    cnv.fill_round_rect(track, 10.0f);
    cnv.fill_style(el::rgba(255, 255, 255, 240));
    cnv.begin_path();
    cnv.add_circle(artist::circle(el::point{on ? track.right - 10.0f : track.left + 10.0f, (track.top + track.bottom) * 0.5f}, 7.0f));
    cnv.fill();
}

void LayoutControlView::DrawSlider(artist::canvas& cnv, el::rect box)
{
    const auto& style = mLook.style;
    const float position = static_cast<float>(std::clamp(mPosition, 0.0, 1.0));
    const float cy = (box.top + box.bottom) * 0.5f;
    const auto track = mImages.Find(style.sliderTrackImageId);
    const auto thumb = mImages.Find(style.sliderThumbImageId);
    if (!style.sliderTrackImageId.empty() || !style.sliderThumbImageId.empty())
    {
        // An image slider: a 16 px track, the picture stretched along it, and a 24 px thumb.
        const el::rect bar{box.left, cy - 8.0f, box.right, cy + 8.0f};
        if (track)
            cnv.draw(*track, bar);
        else
        {
            cnv.fill_style(el::rgba(255, 255, 255, 38));
            cnv.fill_rect(bar);
        }
        const float x = box.left + 12.0f + position * (box.width() - 24.0f);
        const el::rect knob{x - 12.0f, cy - 12.0f, x + 12.0f, cy + 12.0f};
        if (thumb)
            DrawImageFitted(cnv, *thumb, knob);
        else
        {
            cnv.fill_style(mLook.accent);
            cnv.fill_rect(knob);
        }
        return;
    }
    cnv.fill_style(mTheme.IsLight() ? el::rgba(0, 0, 0, 38) : el::rgba(255, 255, 255, 38));
    cnv.fill_round_rect(el::rect{box.left, cy - 2.0f, box.right, cy + 2.0f}, 2.0f);
    const float x = box.left + 7.0f + position * (box.width() - 14.0f);
    cnv.fill_style(mLook.accent.level(mHover || mDragging ? 1.1f : 1.0f));
    cnv.begin_path();
    cnv.add_circle(artist::circle(el::point{x, cy}, 7.0f));
    cnv.fill();
    cnv.stroke_style(el::rgba(255, 255, 255, 77));
    cnv.line_width(2.0f);
    cnv.begin_path();
    cnv.add_circle(artist::circle(el::point{x, cy}, 6.0f));
    cnv.stroke();
}

void LayoutControlView::SetPosition(double position, const el::context& ctx)
{
    const double value = SnapParamValue(mParam, ParamPositionToValue(mParam, std::clamp(position, 0.0, 1.0)));
    if (value != mValue)
    {
        mValue = value;
        if (mActions.setParam)
            mActions.setParam(mParam, value);
    }
    // A stepped control sits on its steps; a continuous one follows the pointer.
    mPosition = mParam.step > 0.0 || IsEnumParam(mParam) ? ParamValueToPosition(mParam, mValue) : std::clamp(position, 0.0, 1.0);
    ctx.view.refresh(ctx);
}

bool LayoutControlView::click(const el::context& ctx, el::mouse_button btn)
{
    if (btn.state == el::mouse_button::right)
    {
        if (btn.down && mActions.menu)
            mActions.menu(mParam, ToView(ctx, btn.pos));
        return true;
    }
    if (btn.state != el::mouse_button::left)
        return false;

    const auto box = ControlBox(ctx.bounds);
    if (!btn.down)
    {
        const bool wasDragging = mDragging;
        mDragging = false;
        // A switch flips on the release over it, as a click on its checkbox does.
        if (mLook.kind == LayoutControlKind::Toggle && box.includes(btn.pos) && mActions.setParam)
        {
            // The checkbox sends 1 or 0, as the web UI's does.
            mValue = std::clamp(mValue >= 0.5 ? 0.0 : 1.0, mParam.minValue, mParam.maxValue);
            mActions.setParam(mParam, mValue);
        }
        if (wasDragging || mLook.kind == LayoutControlKind::Toggle)
            ctx.view.refresh(ctx);
        return true;
    }

    if (mLook.kind == LayoutControlKind::Toggle)
        return true;

    if (btn.num_clicks == 2)
    {
        mDragging = false;
        // A double click on the value types one in; anywhere else on the control resets it.
        if (ValueBox(ctx.bounds).includes(btn.pos) && !IsEnumParam(mParam) && mActions.editValue)
            mActions.editValue(mParam);
        else
            SetPosition(ParamValueToPosition(mParam, mParam.defaultValue), ctx);
        return true;
    }

    if (!box.includes(btn.pos))
        return true;
    mDragging = true;
    // In view pixels, so a knob drawn small on a scaled layout turns no faster than a big one,
    // as the web UI's knob measures the pointer on the screen.
    mDragStartY = ToView(ctx, btn.pos).y;
    mDragStartPosition = mPosition;
    // A slider's thumb jumps to where its track is pressed, as a range input's does.
    if (mLook.kind == LayoutControlKind::Slider)
    {
        const float inset = mLook.style.sliderThumbImageId.empty() && mLook.style.sliderTrackImageId.empty() ? 7.0f : 12.0f;
        SetPosition((btn.pos.x - box.left - inset) / std::max(1.0f, box.width() - 2.0f * inset), ctx);
    }
    return true;
}

void LayoutControlView::drag(const el::context& ctx, el::mouse_button btn)
{
    if (!mDragging)
        return;
    if (mLook.kind == LayoutControlKind::Slider)
    {
        const auto box = ControlBox(ctx.bounds);
        const float inset = mLook.style.sliderThumbImageId.empty() && mLook.style.sliderTrackImageId.empty() ? 7.0f : 12.0f;
        SetPosition((btn.pos.x - box.left - inset) / std::max(1.0f, box.width() - 2.0f * inset), ctx);
        return;
    }
    // Up turns it up; Shift slows the turn to a fifth, as ParamKnob does.
    const float slow = btn.modifiers & el::mod_shift ? 5.0f : 1.0f;
    SetPosition(mDragStartPosition + static_cast<double>(mDragStartY - ToView(ctx, btn.pos).y) / (kDragPixels * slow), ctx);
}

bool LayoutControlView::cursor(const el::context& ctx, el::point, el::cursor_tracking status)
{
    const bool hover = status != el::cursor_tracking::leaving;
    if (hover != mHover)
    {
        mHover = hover;
        ctx.view.refresh(ctx);
    }
    return false;
}

bool LayoutControlView::scroll(const el::context& ctx, el::point dir, el::point)
{
    if (dir.y == 0.0f || !mHover || !ParamKnob::sWheelAdjusts || mLook.kind == LayoutControlKind::Toggle)
        return false;
    // One label at a time for a list, else a fiftieth of the travel.
    if (IsEnumParam(mParam))
    {
        const double step = mParam.step > 0.0 ? mParam.step : 1.0;
        SetPosition(ParamValueToPosition(mParam, std::clamp(mValue + (dir.y > 0.0f ? step : -step), mParam.minValue, mParam.maxValue)), ctx);
    }
    else
        SetPosition(mPosition + (dir.y > 0.0f ? 0.02 : -0.02), ctx);
    return true;
}

bool LayoutControlView::Update(const guitarfx::GraphNode& node)
{
    if (mDragging)
        return false;
    const double value = NodeParamValue(node, mParam);
    if (value == mValue)
        return false;
    mValue = value;
    mPosition = ParamValueToPosition(mParam, value);
    return true;
}

// ── Building ───────────────────────────────────────────────────────────────────────────

namespace
{
LayoutControlView::Look MakeLook(const Theme& theme, const EffectLayout& layout, const LayoutControl& control,
                                 const EffectParamInfo& param, const Inks& inks)
{
    LayoutControlView::Look look;
    look.kind = ClassifyLayoutControl(control, &param);
    look.style = control.style;
    look.label = LayoutControlLabel(control, &param);
    look.labelTop = !control.style.hideLabel && control.style.labelPosition == "top";
    look.labelBottom = !control.style.hideLabel && control.style.labelPosition == "bottom";
    look.labelSize = static_cast<float>(LayoutLabelFontSize(layout, control));
    look.valueSize = static_cast<float>(LayoutValueFontSize(layout, control));
    look.labelMaxWidth = static_cast<float>(LayoutLabelMaxWidth(layout));
    const auto labelColour = LayoutLabelColour(layout, control);
    const auto valueColour = LayoutValueColour(layout, control);
    look.labelInk = labelColour ? ToColour(*labelColour) : inks.label;
    look.valueInk = valueColour ? ToColour(*valueColour) : inks.value;
    look.accent = theme.Accent();

    const bool imageToggle = !control.style.toggleOnImageId.empty() && !control.style.toggleOffImageId.empty();
    const bool imageSlider = !control.style.sliderTrackImageId.empty() || !control.style.sliderThumbImageId.empty();
    look.control = ControlExtent(look.kind, control.style, imageToggle, imageSlider);

    // The wrapper: its own width when the layout gives one, else at least 80 and as wide as
    // what it holds (the web UI's column of centred items).
    if (control.size && control.size->width > 0.0)
        look.width = std::round(static_cast<float>(control.size->width));
    else
    {
        float content = look.control.x;
        if (look.labelTop || look.labelBottom)
            content = std::max(content, std::min(look.labelMaxWidth, MeasureTextWidth(look.label, Font(Weight::SemiBold).size(look.labelSize))));
        if (control.style.showValue)
            content = std::max(content, MeasureTextWidth(FormatParamValue(param, param.maxValue), Font(Weight::SemiBold).size(look.valueSize)));
        look.width = std::max(80.0f, std::ceil(content));
    }
    return look;
}
} // namespace

LayoutViewParts BuildLayoutView(const Theme& theme, LayoutImages& images, const LayoutViewInput& input, const LayoutActions& actions)
{
    LayoutViewParts parts;
    if (input.layout == nullptr || input.node == nullptr)
        return parts;

    const auto layout = std::make_shared<const EffectLayout>(*input.layout);
    const auto& node = *input.node;
    const float w = std::round(static_cast<float>(layout->dimensions.width));
    const float h = std::round(static_cast<float>(layout->dimensions.height));
    const auto inks = InksFor(theme, layout->containerTheme);
    const bool bypassed = !node.enabled;

    auto placed = std::make_shared<PlacedLayer>(w, h);
    placed->Add(std::make_shared<LayoutBackdropView>(images, layout, bypassed), el::rect{0.0f, 0.0f, w, h});

    if (layout->useDefaultControls)
    {
        // The standard controls on the backdrop, moved and scaled as one block.
        if (input.defaultControls)
        {
            const auto finite = [](double v) { return std::isfinite(v) && v > 0.0 ? static_cast<float>(v) : 1.0f; };
            const float sx = finite(layout->defaultControlsScale.x), sy = finite(layout->defaultControlsScale.y);
            const float x = std::round(static_cast<float>(layout->defaultControlsOffset.x));
            const float y = std::round(static_cast<float>(layout->defaultControlsOffset.y));
            placed->Add(Scaled(input.defaultControls, sx, sy, 0.0f),
                        el::rect{x, y, x + input.defaultControlsSize.x * sx, y + input.defaultControlsSize.y * sy});
        }
    }
    else
    {
        for (const auto& control : layout->controls)
        {
            const float x = std::round(static_cast<float>(control.position.x));
            const float y = std::round(static_cast<float>(control.position.y));
            if (IsLayoutResourceControl(control))
            {
                const auto slot = LayoutControlResourceSlot(control);
                if (!slot || !input.resourceSlot)
                    continue;
                // A slot as wide as the layout gives it, else to the layout's right edge.
                const float width = control.size && control.size->width > 0.0 ? static_cast<float>(control.size->width)
                                                                               : std::min(360.0f, w - x);
                const std::string label = control.labelOverride.empty() ? std::string() : control.labelOverride;
                if (auto row = input.resourceSlot(*slot, label, width))
                    placed->Add(row, el::rect{x, y, x + width, y + 26.0f});
                continue;
            }
            const auto* param = input.type != nullptr ? input.type->FindParam(control.paramKey) : nullptr;
            if (param == nullptr)
                continue; // a parameter this effect no longer has
            auto view = std::make_shared<LayoutControlView>(theme, images, *param, MakeLook(theme, *layout, control, *param, inks), actions);
            view->Update(node);
            const auto size = view->Size();
            placed->Add(view, el::rect{x, y, x + size.x, y + size.y});
            parts.controls.push_back(view);
        }
    }

    placed->Add(std::make_shared<LayoutTextView>(layout, inks.text), el::rect{0.0f, 0.0f, w, h});

    for (const auto& overlay : layout->overlays)
        if (overlay.toggleBypassOnClick)
            placed->Add(std::make_shared<BypassHotspot>(images, overlay, LayoutOverlayVisible(overlay, bypassed), actions.toggleBypass),
                        LayoutBackdropView::Place(el::rect{0.0f, 0.0f, w, h}, overlay));

    const float scale = static_cast<float>(input.scale > 0.0 ? input.scale : 1.0);
    parts.element = Scaled(placed, scale, scale, kCornerRadius);
    parts.size = {w * scale, h * scale};
    return parts;
}
} // namespace nanoq::ui
