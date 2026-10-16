#pragma once

#include "dsp/effects/BuiltinAmpFilters.h"
#include "dsp/effects/BuiltinAmpVoicing.h"
#include <algorithm>
#include <array>
#include <type_traits>

/**
 * A chunk of the Heavy American's signal, as BuiltinAmpRender.h runs it: the controls over the
 * chunk, and the loops that run a linear section or a clipper over it.
 */
namespace guitarfx::builtin_amp
{
/// Host samples per pass: 256 oversampled ones at 4x.
inline constexpr int kChunk = 64;
inline constexpr int kOversampledChunk = 4 * kChunk;

// The fixed-shape filters, in the order the signal meets them.
enum Filter
{
    kPreHighPass,
    kPreEmphasisShelf,
    kBassShelf,
    kMiddlePeak,
    kContourPeak,
    kTrebleShelf,
    kVoicingPeak,
    kPresencePeak,
    kDepthShelf,
    kResonancePeak,
    kDampingShelf,
    kPostHighPass,
    kFilterCount
};

using HostValues = std::array<float, kChunk>;

/// Whether a control holds one value for the whole chunk, so a loop can take it as a constant.
[[nodiscard]] inline bool IsHeld(const HostValues& values, int count)
{
    int differ = 0;

    for (int h = 1; h < count; ++h)
    {
        differ |= values[h] != values[0] ? 1 : 0;
    }

    return differ == 0;
}

/// What a step of the voicing changed in the clippers: the knee, which every stage uses, or only
/// the power stage.
struct ClipperChange
{
    bool knee = false;
    bool power = false;
};

/**
 * The controls over one chunk, as the effect stepped them: each host sample's value, read by every
 * oversampled sample it covers, and the clippers as Character, Power Drive and Bias set them up.
 */
struct ChunkControls
{
    /// Keeps the clippers as they are at host sample `h`. Only the first is kept until they move,
    /// and the loops then use it throughout.
    void RecordClippers(int h, const Clippers& now, ClipperChange change)
    {
        if (h == 0)
        {
            clippers[0] = now;
            kneeMoves = false;
            powerMoves = false;
            return;
        }

        if ((change.knee || change.power) && !kneeMoves && !powerMoves)
        {
            std::fill(clippers.begin() + 1, clippers.begin() + h, clippers[0]);
        }

        kneeMoves = kneeMoves || change.knee;
        powerMoves = powerMoves || change.power;

        if (kneeMoves || powerMoves)
        {
            clippers[h] = now;
        }
    }

    /// Host sample h's values for the rest of the chunk, once the controls have come to rest.
    void HoldFrom(int h)
    {
        const auto hold = [this, h](HostValues& values) {
            std::fill(values.begin() + h + 1, values.begin() + count, values[h]);
        };

        for (HostValues* values : {&stageGain, &voice, &powerDrive, &sag, &outputGain, &levelMakeup, &powerMakeup})
        {
            hold(*values);
        }

        for (HostValues& values : drive)
        {
            hold(values);
        }

        for (HostValues& values : mix)
        {
            hold(values);
        }

        if (kneeMoves || powerMoves)
        {
            std::fill(clippers.begin() + h + 1, clippers.begin() + count, clippers[h]);
        }
    }

    int count = 0;             ///< Host samples in the chunk.
    int shift = 2;             ///< log2 of the oversampling factor.
    double filterStep = 1.0;   ///< How far the coefficient glides step per host sample.
    HostValues stageGain = {}; ///< Input Trim, with the nominal level's input scaling.
    HostValues voice = {};
    std::array<HostValues, kClipCount> drive = {}; ///< What each clipper's input is scaled by.
    std::array<HostValues, kMaxStages> mix = {};   ///< How far each preamp stage is in (StageFades).
    HostValues powerDrive = {};
    HostValues sag = {};
    HostValues outputGain = {};
    HostValues levelMakeup = {};
    HostValues powerMakeup = {};
    std::array<Clippers, kChunk> clippers = {};
    bool kneeMoves = false;  ///< Character glides within the chunk: every clipper's set-up moves.
    bool powerMoves = false; ///< The power stage's set-up moves (Character, Power Drive or Bias).
};

/// A section with nothing between its filters.
struct NoTap
{
    double operator()(int, int, double v) const
    {
        return v;
    }
};

/**
 * Runs N consecutive GlidingBiquads over one channel's oversampled samples of host samples
 * [0, hostCount), all N per sample in one loop, with their state in locals that nothing else can
 * reach, so it stays in registers. The coefficients are read where they are each time: a load is
 * off the filters' recursions, and with them in registers too the state was spilled to the stack.
 *
 * Sample i of `x` goes through the preamp stage `coupling` first where there is one (Coupled),
 * then filters [0, Tap), then tap(i, h, y) (h being its host sample), then the rest, and
 * out(i, h, y) takes what comes out. While the coefficients glide they step once per host sample,
 * before its samples, as they always did.
 */
template <int N, int Tap, bool Coupled, class TapFn, class Out>
inline void RunBiquadsAs(GlidingBiquad* filters, StageFilter* coupling, int ch, int hostCount, const ChunkControls& c,
                         const float* x, TapFn tap, Out out)
{
    double z1[N];
    double z2[N];
    bool settled = true;

    for (int k = 0; k < N; ++k)
    {
        z1[k] = filters[k].s1[ch];
        z2[k] = filters[k].s2[ch];
        settled = settled && filters[k].settled;
    }

    StageFilter::State s = {};
    float hp = 0.0f;
    float lp = 0.0f;

    if constexpr (Coupled)
    {
        s = coupling->state[ch];
        hp = coupling->hpCoefficient;
        lp = coupling->lpStep;
        settled = settled && coupling->settled;
    }

    const auto sample = [&](int i, int h) {
        double v = x[i];

        if constexpr (Coupled)
        {
            v = StageFilter::Step(hp, lp, s, x[i]);
        }

        for (int k = 0; k < Tap; ++k)
        {
            v = GlidingBiquad::Step(filters[k].current, z1[k], z2[k], v);
        }

        if constexpr (Tap < N)
        {
            v = tap(i, h, v);

            for (int k = Tap; k < N; ++k)
            {
                v = GlidingBiquad::Step(filters[k].current, z1[k], z2[k], v);
            }
        }

        out(i, h, v);
    };

    const int shift = c.shift;
    const int count = hostCount << shift;
    int i = 0;

    // A host sample at a time while the coefficients glide, then the rest in one loop. Which loop
    // a sample runs in depends only on whether they still glided at it, never on where its chunk
    // began: the compilers do the arithmetic differently in each (/fp:fast), and the output would
    // otherwise move with the host's block size.
    for (int h = 0; h < hostCount && !settled; ++h)
    {
        settled = true;

        for (int k = 0; k < N; ++k)
        {
            filters[k].Advance(c.filterStep);
            settled = settled && filters[k].settled;
        }

        if constexpr (Coupled)
        {
            coupling->Advance(static_cast<float>(c.filterStep));
            hp = coupling->hpCoefficient;
            lp = coupling->lpStep;
            settled = settled && coupling->settled;
        }

        for (; i < (h + 1) << shift; ++i)
        {
            sample(i, h);
        }
    }

    for (; i < count; ++i)
    {
        sample(i, i >> shift);
    }

    for (int k = 0; k < N; ++k)
    {
        filters[k].s1[ch] = z1[k];
        filters[k].s2[ch] = z2[k];
    }

    if constexpr (Coupled)
    {
        coupling->state[ch] = s;
    }
}

/// RunBiquadsAs, with a preamp stage's coupling filter in front when `coupling` is one.
template <int N, int Tap = N, class TapFn = NoTap, class Out>
inline void RunBiquads(GlidingBiquad* filters, StageFilter* coupling, int ch, int hostCount, const ChunkControls& c,
                       const float* x, TapFn tap, Out out)
{
    if (coupling)
    {
        RunBiquadsAs<N, Tap, true>(filters, coupling, ch, hostCount, c, x, tap, out);
    }
    else
    {
        RunBiquadsAs<N, Tap, false>(filters, nullptr, ch, hostCount, c, x, tap, out);
    }
}

/// One preamp stage's coupling filter over host samples [0, hostCount) of `x`, in place, as
/// RunBiquads runs a section.
inline void RunCoupling(StageFilter& filter, int ch, int hostCount, const ChunkControls& c, float* x)
{
    StageFilter::State s = filter.state[ch];
    float hp = filter.hpCoefficient;
    float lp = filter.lpStep;
    const int count = hostCount << c.shift;
    int i = 0;

    for (int h = 0; h < hostCount && !filter.settled; ++h)
    {
        filter.Advance(static_cast<float>(c.filterStep));
        hp = filter.hpCoefficient;
        lp = filter.lpStep;

        for (; i < (h + 1) << c.shift; ++i)
        {
            x[i] = StageFilter::Step(hp, lp, s, x[i]);
        }
    }

    for (; i < count; ++i)
    {
        x[i] = StageFilter::Step(hp, lp, s, x[i]);
    }

    filter.state[ch] = s;
}

// ---------------------------------------------------------------------------------------
// Clipping a chunk
// ---------------------------------------------------------------------------------------

// The clip loops run with precise floating point, whatever the build's, so the output does not
// depend on how the host splits its blocks (TestDynamicsAndBlocks), as it never did a sample at a
// time. Where a chunk ends decides which samples a vectorised loop works out in its vector lanes
// and which one at a time after them, and while Character glides they fall back to a sample at a
// time. Under /fp:fast MSVC does different arithmetic in each (a vector division is an estimate
// refined once, and multiply-adds fuse in one and not the other), and the output moved by up to
// 1e-6. Precise, each does the same IEEE arithmetic. GCC has no such pragma.
#if defined(_MSC_VER) || defined(__clang__)
    #pragma float_control(precise, on, push)
#endif

/// Which of the three knees Character has live. A clip loop is compiled once per kind, so it runs
/// with no branch per sample and vectorises.
enum class KneeKind
{
    Tanh,
    SoftTanh,
    TanhHard,
    Soft,
    Hard
};

[[nodiscard]] inline KneeKind KindOf(const KneeBlend& knee)
{
    if (knee.tanhWeight == 1.0f)
    {
        return KneeKind::Tanh;
    }

    if (knee.tanhWeight > 0.0f)
    {
        return knee.softWeight > 0.0f ? KneeKind::SoftTanh : KneeKind::TanhHard;
    }

    return knee.softWeight > 0.0f ? KneeKind::Soft : KneeKind::Hard;
}

/// KneeBlend::Shape for a knee of kind `Kind`: the same sums, in the same order.
template <KneeKind Kind> [[nodiscard]] inline float ShapeAs(const KneeBlend& knee, float x)
{
    if constexpr (Kind == KneeKind::Tanh)
    {
        return KneeTanh(x);
    }
    else
    {
        float y = 0.0f;

        if constexpr (Kind == KneeKind::SoftTanh || Kind == KneeKind::TanhHard)
        {
            y += knee.tanhWeight * KneeTanh(x);
        }

        if constexpr (Kind == KneeKind::SoftTanh || Kind == KneeKind::Soft)
        {
            y += knee.softWeight * SoftKneeShape(x);
        }

        if constexpr (Kind == KneeKind::TanhHard || Kind == KneeKind::Hard)
        {
            y += knee.hardWeight * HardKneeShape(x);
        }

        return y;
    }
}

/// Calls f with the knee's kind as a compile-time constant (std::integral_constant).
template <class F> inline void WithKneeKind(const KneeBlend& knee, F&& f)
{
    switch (KindOf(knee))
    {
    case KneeKind::Tanh:
        f(std::integral_constant<KneeKind, KneeKind::Tanh>{});
        break;
    case KneeKind::SoftTanh:
        f(std::integral_constant<KneeKind, KneeKind::SoftTanh>{});
        break;
    case KneeKind::TanhHard:
        f(std::integral_constant<KneeKind, KneeKind::TanhHard>{});
        break;
    case KneeKind::Soft:
        f(std::integral_constant<KneeKind, KneeKind::Soft>{});
        break;
    case KneeKind::Hard:
        f(std::integral_constant<KneeKind, KneeKind::Hard>{});
        break;
    }
}

/// A control the loops read per oversampled sample: one value for the whole chunk, or each
/// sample's own.
struct Held
{
    float operator[](int) const
    {
        return value;
    }

    float value;
};

struct PerSample
{
    float operator[](int i) const
    {
        return values[i];
    }

    const float* values;
};

/**
 * n rounded up to a whole number of vector lanes. The clip loops run over this many samples, the
 * ones past n being stale and unused, so none of a chunk's samples falls to a loop's scalar
 * remainder: under -ffast-math clang estimates a vector square root or division whatever the
 * pragma above, and works out the scalar ones exactly. The scratch buffers are kOversampledChunk
 * long, a whole number of lanes.
 */
inline constexpr int kLanes = 16;
static_assert(kOversampledChunk % kLanes == 0, "the buffers hold whole lanes");

[[nodiscard]] inline int WholeLanes(int n)
{
    return (n + kLanes - 1) / kLanes * kLanes;
}

/// Each host sample's value, repeated for its oversampled samples.
inline void HoldEach(const HostValues& values, const ChunkControls& c, float* out)
{
    const int n = c.count << c.shift;

    for (int i = 0; i < n; ++i)
    {
        out[i] = values[i >> c.shift];
    }
}

/// The knee's inputs `u`, shaped in place. MSVC won't vectorise the soft and tanh knees in one
/// loop, but does in two, so that blend takes two (`part` holds the soft knee's share): the same
/// sum. Each of the others is one loop.
template <KneeKind Kind> inline void ShapeEach(const KneeBlend& knee, float* u, float* part, int n)
{
    if constexpr (Kind == KneeKind::SoftTanh)
    {
        const float softWeight = knee.softWeight;
        const float tanhWeight = knee.tanhWeight;

        for (int i = 0; i < n; ++i)
        {
            part[i] = softWeight * SoftKneeShape(u[i]);
        }

        for (int i = 0; i < n; ++i)
        {
            u[i] = tanhWeight * KneeTanh(u[i]) + part[i];
        }
    }
    else
    {
        const KneeBlend local = knee;

        for (int i = 0; i < n; ++i)
        {
            u[i] = ShapeAs<Kind>(local, u[i]);
        }
    }
}

/// Clippers::Clip of x × drive, then × post, in place over n samples. `u` and `part` are scratch.
template <KneeKind Kind, class Drive>
inline void ClipAs(const Clippers& clippers, int index, float post, float* x, Drive drive, float* u, float* part, int n)
{
    const float bias = clippers.clipBias[index];
    const float offset = clippers.clipOffset[index];
    const float invSlope = clippers.clipInvSlope[index];

    for (int i = 0; i < n; ++i)
    {
        u[i] = x[i] * drive[i] + bias;
    }

    ShapeEach<Kind>(clippers.knee, u, part, n);

    for (int i = 0; i < n; ++i)
    {
        x[i] = (u[i] - offset) * invSlope * post;
    }
}

/// The power stage in place over n samples: Clippers::PowerClip against each sample's sag
/// `headroom`, blended in by Power Drive. `u` and `part` are scratch.
template <KneeKind Kind, class Drive>
inline void PowerClipAs(const Clippers& clippers, float* x, const float* headroom, Drive drive, float* u, float* part,
                        int n)
{
    const float gain = clippers.powerGain;
    const float bias = clippers.powerBias;
    const float offset = clippers.powerOffset;
    const float invScale = clippers.powerInvScale;

    for (int i = 0; i < n; ++i)
    {
        u[i] = gain * (x[i] / headroom[i] + bias);
    }

    ShapeEach<Kind>(clippers.knee, u, part, n);

    for (int i = 0; i < n; ++i)
    {
        const float input = x[i];
        const float clipped = headroom[i] * (u[i] - offset) * invScale;
        x[i] = drive[i] != 0.0f ? input + drive[i] * (clipped - input) : input;
    }
}

/// Stage one's legs crossfaded by Voice, held between the two for the chunk.
inline void BlendVoices(const float* clean, const float* drive, Held voice, float* x, int n)
{
    const float v = voice.value;

    for (int i = 0; i < n; ++i)
    {
        x[i] = (clean[i] + (drive[i] - clean[i]) * v) * 0.9f;
    }
}

/// The same while Voice glides: exactly the drive leg where it reaches 1 (and the clean one at 0).
inline void BlendVoices(const float* clean, const float* drive, PerSample voice, float* x, int n)
{
    for (int i = 0; i < n; ++i)
    {
        const float v = voice[i];
        x[i] = (v == 1.0f ? drive[i] : clean[i] + (drive[i] - clean[i]) * v) * 0.9f;
    }
}

/// Where the clip loops work: the knee's inputs, one knee's share of a blend, and a control held
/// per oversampled sample, each kOversampledChunk long.
struct ClipScratch
{
    float* knee;
    float* part;
    float* held;
};

/// Clipper `index` in place over n samples of `x`, each scaled by its host sample's drive first
/// and by `post` after. `x` holds WholeLanes(n) finite samples.
inline void ClipChunk(const ChunkControls& c, int index, float post, float* x, int n, const ClipScratch& scratch)
{
    const HostValues& drive = c.drive[index];

    if (c.kneeMoves)
    {
        // Character is gliding, so the knee differs from one host sample to the next.
        for (int i = 0; i < n; ++i)
        {
            const int h = i >> c.shift;
            x[i] = c.clippers[h].Clip(x[i] * drive[h], index) * post;
        }

        return;
    }

    const Clippers& clippers = c.clippers[0];
    const int lanes = WholeLanes(n);

    if (IsHeld(drive, c.count))
    {
        WithKneeKind(clippers.knee, [&](auto kind) {
            ClipAs<decltype(kind)::value>(clippers, index, post, x, Held{drive[0]}, scratch.knee, scratch.part, lanes);
        });
    }
    else
    {
        HoldEach(drive, c, scratch.held);
        WithKneeKind(clippers.knee, [&](auto kind) {
            ClipAs<decltype(kind)::value>(clippers, index, post, x, PerSample{scratch.held}, scratch.knee, scratch.part,
                                          lanes);
        });
    }
}

/// The power stage in place over n samples of `x`, against each sample's sag `headroom`. Both hold
/// WholeLanes(n) finite samples, and no headroom is 0.
inline void PowerClipChunk(const ChunkControls& c, float* x, const float* headroom, int n, const ClipScratch& scratch)
{
    if (c.kneeMoves || c.powerMoves)
    {
        for (int i = 0; i < n; ++i)
        {
            const int h = i >> c.shift;

            if (const float drive = c.powerDrive[h]; drive != 0.0f)
            {
                const float clipped = c.clippers[h].PowerClip(x[i], headroom[i]);
                x[i] = x[i] + drive * (clipped - x[i]);
            }
        }

        return;
    }

    const Clippers& clippers = c.clippers[0];
    const int lanes = WholeLanes(n);

    if (IsHeld(c.powerDrive, c.count))
    {
        WithKneeKind(clippers.knee, [&](auto kind) {
            PowerClipAs<decltype(kind)::value>(clippers, x, headroom, Held{c.powerDrive[0]}, scratch.knee, scratch.part,
                                               lanes);
        });
    }
    else
    {
        HoldEach(c.powerDrive, c, scratch.held);
        WithKneeKind(clippers.knee, [&](auto kind) {
            PowerClipAs<decltype(kind)::value>(clippers, x, headroom, PerSample{scratch.held}, scratch.knee,
                                               scratch.part, lanes);
        });
    }
}

/// A preamp stage part-way through its fade: its output `engaged` crossfaded with its input `x`,
/// in place.
inline void CrossfadeStage(const ChunkControls& c, const HostValues& mix, const float* engaged, float* x, int n)
{
    for (int i = 0; i < n; ++i)
    {
        x[i] = StageFades::SwitchIn(x[i], engaged[i], mix[i >> c.shift]);
    }
}

#if defined(_MSC_VER) || defined(__clang__)
    #pragma float_control(pop)
#endif
} // namespace guitarfx::builtin_amp
