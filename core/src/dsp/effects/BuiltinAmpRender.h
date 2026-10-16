#pragma once

#include "dsp/effects/BuiltinAmpChunk.h"
#include "dsp/effects/BuiltinAmpFilters.h"
#include "dsp/effects/BuiltinAmpOversampling.h"
#include "dsp/effects/BuiltinAmpVoicing.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

/**
 * How the Heavy American's signal runs, as opposed to what its controls do (BuiltinAmpEffect.h): a
 * chunk at a time, each stage over the whole oversampled chunk before the next.
 *
 * One oversampled sample at a time through the whole chain, every clipper waited on the one before
 * it and every filter's state went back to memory between calls. A chunk at a time, a clipper,
 * having no memory, is one loop that vectorises across time. A linear section (the pre-filters, the
 * tone stack, the voicing and speaker filters) is one loop with all its biquads per sample and
 * their state in locals, so one sample's later filters overlap the next sample's earlier ones. A
 * loop per biquad was measured no faster than a sample at a time: each filter then waits on its
 * own recursion. See docs/heavy-american-performance-review-2026-10-16.md, finding 5.
 *
 * The controls still glide once per host sample. The effect steps them through the chunk first
 * (ChunkControls) and the loops read each host sample's values, so the output is the per-sample
 * path's to float rounding.
 */
namespace guitarfx::builtin_amp
{
/**
 * Everything between the amp's input and output that has state per channel, and the loops that
 * run a chunk of one channel through it. The filters' coefficients, and their glides, are shared by
 * the channels.
 */
class SignalPath
{
  public:
    void Prepare()
    {
        for (auto* bank : {&mUpFirst, &mDownFirst, &mUpSecond, &mDownSecond})
        {
            for (auto& filter : *bank)
            {
                filter.Prepare();
            }
        }

        // The power clip divides by every lane's headroom, those past a chunk's end too.
        mHeadroom.fill(1.0f);
    }

    /// Silence on both channels, with every coefficient glide landed.
    void Reset()
    {
        for (auto* bank : {&mUpFirst, &mDownFirst, &mUpSecond, &mDownSecond})
        {
            for (auto& filter : *bank)
            {
                filter.Reset();
            }
        }

        for (auto& filter : stageFilters)
        {
            filter.Reset();
        }

        for (auto& filter : filters)
        {
            filter.Reset();
        }

        sagEnvelope.fill(0.0f);

        for (auto& filter : filters)
        {
            filter.Advance(1.0);
        }

        for (auto& filter : stageFilters)
        {
            filter.Advance(1.0f);
        }
    }

    // Every piece of per-channel state; the same list Reset() clears. If a new filter or envelope
    // is added to one, add it here too, or the right channel comes back from a mono stretch out of
    // step with the left (TestMonoPath catches that).
    void CopyChannel(int from, int to)
    {
        for (auto* bank : {&mUpFirst, &mDownFirst, &mUpSecond, &mDownSecond})
        {
            (*bank)[to] = (*bank)[from];
        }

        for (auto& filter : stageFilters)
        {
            filter.CopyChannel(from, to);
        }

        for (auto& filter : filters)
        {
            filter.CopyChannel(from, to);
        }

        sagEnvelope[to] = sagEnvelope[from];
    }

    /// One chunk of host samples [start, start + c.count) for each channel. Channels with no input
    /// hear silence; ones with no output still run, but leave their decimators where they were.
    template <int Channels>
    void Render(const float* const* inputs, float* const* outputs, int start, const ChunkControls& c)
    {
        // The coefficient glides step once per host sample, for both channels, but each channel
        // runs the chunk on its own: the second starts them where the first did.
        if constexpr (Channels > 1)
        {
            mFiltersAtStart = filters;
            mStageFiltersAtStart = stageFilters;
        }

        for (int ch = 0; ch < Channels; ++ch)
        {
            if (ch > 0)
            {
                RewindGlides();
            }

            const float* in = inputs[ch] ? inputs[ch] + start : nullptr;
            float* out = outputs[ch] ? outputs[ch] + start : nullptr;
            RenderChannel(ch, in, out, c);
        }
    }

    std::array<GlidingBiquad, kFilterCount> filters = {};
    std::array<StageFilter, kMaxStages> stageFilters = {};
    std::array<float, 2> sagEnvelope = {};
    float sagAttack = 0.0f;
    float sagRelease = 0.0f;

  private:
    void RenderChannel(int ch, const float* in, float* out, const ChunkControls& c)
    {
        float* x = mSignal.data();
        const int n = c.count << c.shift;
        Upsample(ch, in, c);

        // The pre-filters, then Input Trim: what drives the first stage.
        const float* stageGain = c.stageGain.data();
        RunBiquads<2>(&filters[kPreHighPass], nullptr, ch, c.count, c, x, NoTap{},
                      [x, stageGain](int i, int h, double y) { x[i] = static_cast<float>(y) * stageGain[h]; });

        // A stage's coupling filter runs in the loop of the linear section after it where it
        // can, overlapping its filters, which costs next to nothing; it waits as `coupling` until
        // then. Where another clipper or a fade comes first, it runs on its own.
        FirstStage(c, x, n);
        StageFilter* coupling = LaterStage(1, kClipStage2, 0.85f, ch, c, &stageFilters[0]);
        RunBiquads<4>(&filters[kBassShelf], coupling, ch, c.count, c, x, NoTap{},
                      [x](int i, int, double y) { x[i] = static_cast<float>(y); });

        coupling = LaterStage(2, kClipStage3, 0.8f, ch, c, nullptr);
        coupling = LaterStage(3, kClipStage4, 0.8f, ch, c, coupling);
        PowerStage(ch, c, coupling);
        Downsample(ch, out, c);
    }

    void Clip(const ChunkControls& c, int index, float post, float* x, int n)
    {
        ClipChunk(c, index, post, x, n, {mKneeInput.data(), mKneePart.data(), mHeld.data()});
    }

    void FirstStage(const ChunkControls& c, float* x, int n)
    {
        // Voice snaps to 0 or 1, so outside its glide only one leg is heard, and only that one is
        // clipped.
        const bool held = IsHeld(c.voice, c.count);

        if (held && (c.voice[0] == 0.0f || c.voice[0] == 1.0f))
        {
            Clip(c, c.voice[0] == 0.0f ? kClipClean : kClipDrive, 0.9f, x, n);
            return;
        }

        float* clean = mClean.data();
        float* drive = mEngaged.data();
        std::copy(x, x + n, clean);
        std::copy(x, x + n, drive);
        Clip(c, kClipClean, 1.0f, clean, n);
        Clip(c, kClipDrive, 1.0f, drive, n);

        if (held)
        {
            BlendVoices(clean, drive, Held{c.voice[0]}, x, WholeLanes(n));
        }
        else
        {
            HoldEach(c.voice, c, mHeld.data());
            BlendVoices(clean, drive, PerSample{mHeld.data()}, x, WholeLanes(n));
        }
    }

    // Preamp stages 2 to 4: clipped, coupled on, and faded in or out (StageFades). `waiting` is
    // the coupling filter of the stage before, not yet run; returns this stage's, or the one before
    // if this one is out, for the next section to run. A fade moves one way through a chunk, so a
    // stage fading out plays until its mix reaches 0, and one fading in plays from the start.
    StageFilter* LaterStage(int stage, int index, float post, int ch, const ChunkControls& c, StageFilter* waiting)
    {
        const HostValues& mix = c.mix[stage];
        StageFilter& coupling = stageFilters[stage];
        int active = 0;

        while (active < c.count && mix[active] > 0.0f)
        {
            ++active;
        }

        if (active > 0)
        {
            float* x = mSignal.data();
            const int n = active << c.shift;

            if (waiting)
            {
                RunCoupling(*waiting, ch, c.count, c, x);
            }

            // Fully in, the stage's own output is the signal; while fading it is crossfaded with
            // its input.
            if (mix[0] == 1.0f && mix[c.count - 1] == 1.0f)
            {
                Clip(c, index, post, x, n);
                return &coupling;
            }

            float* engaged = mEngaged.data();
            std::copy(x, x + n, engaged);
            Clip(c, index, post, engaged, n);
            RunCoupling(coupling, ch, active, c, engaged);

            CrossfadeStage(c, mix, engaged, x, n);
        }

        // Out for the rest of the chunk, its coupling still glides, as it did a sample at a time.
        for (int h = active; h < c.count && !coupling.settled; ++h)
        {
            coupling.Advance(static_cast<float>(c.filterStep));
        }

        return active > 0 ? nullptr : waiting;
    }

    // The voicing and speaker filters, the power stage and its sag, the 25 Hz high pass, and the
    // output's gains. The speaker filters come before the power stage, a frequency-dependent drive:
    // they shape the distortion as well as the level. The external IR supplies cabinet and
    // microphone tone. `coupling` is the last preamp stage's coupling filter, not yet run.
    void PowerStage(int ch, const ChunkControls& c, StageFilter* coupling)
    {
        float* x = mSignal.data();
        const int n = c.count << c.shift;
        float envelope = sagEnvelope[ch];
        const float attack = sagAttack;
        const float release = sagRelease;

        // Sag is the supply giving way as the power stage works: the envelope pulls the stage's
        // ceiling down to `headroom`, and the knee meets the signal against that lower ceiling, so
        // loud playing clips harder and is squeezed the most. It acts only on the clipped share of
        // the stage, so with no Power Drive it changes nothing. The level it costs at the nominal
        // level is made up (PowerDriveMakeup).
        const auto follow = [&envelope, attack, release](float powerInput) {
            const float detector = std::abs(powerInput);
            const float coefficient = detector > envelope ? attack : release;
            envelope = coefficient * envelope + (1.0f - coefficient) * detector;
        };
        const float* outputGain = c.outputGain.data();
        const float* levelMakeup = c.levelMakeup.data();
        const float* powerMakeup = c.powerMakeup.data();
        const auto output = [x, outputGain, levelMakeup, powerMakeup](int i, int h, double y) {
            x[i] = static_cast<float>(y) * outputGain[h] * levelMakeup[h] * powerMakeup[h];
        };

        if (!IsDriven(c))
        {
            // With no Power Drive (the default; its glide lands on exactly 0) the clipped share is
            // nothing, so the clipper is skipped. The envelope follows either way, so turning the
            // drive up finds it current.
            const auto passThrough = [&follow](int, int, double y) {
                const float powerInput = static_cast<float>(y);
                follow(powerInput);
                return static_cast<double>(powerInput);
            };
            RunBiquads<6, 5>(&filters[kVoicingPeak], coupling, ch, c.count, c, x, passThrough, output);
        }
        else
        {
            float* headroom = mHeadroom.data();
            const float* sag = c.sag.data();
            RunBiquads<5>(&filters[kVoicingPeak], coupling, ch, c.count, c, x, NoTap{},
                          [x, headroom, sag, &follow, &envelope](int i, int h, double y) {
                              const float powerInput = static_cast<float>(y);
                              follow(powerInput);
                              x[i] = powerInput;
                              headroom[i] = SagHeadroom(sag[h], envelope);
                          });
            PowerClip(c, x, n);
            RunBiquads<1>(&filters[kPostHighPass], nullptr, ch, c.count, c, x, NoTap{}, output);
        }

        sagEnvelope[ch] = envelope;
    }

    void PowerClip(const ChunkControls& c, float* x, int n)
    {
        PowerClipChunk(c, x, mHeadroom.data(), n, {mKneeInput.data(), mKneePart.data(), mHeld.data()});
    }

    [[nodiscard]] static bool IsDriven(const ChunkControls& c)
    {
        for (int h = 0; h < c.count; ++h)
        {
            if (c.powerDrive[h] != 0.0f)
            {
                return true;
            }
        }

        return false;
    }

    void RewindGlides()
    {
        for (std::size_t k = 0; k < filters.size(); ++k)
        {
            filters[k].current = mFiltersAtStart[k].current;
            filters[k].settled = mFiltersAtStart[k].settled;
        }

        for (std::size_t k = 0; k < stageFilters.size(); ++k)
        {
            stageFilters[k].hpCoefficient = mStageFiltersAtStart[k].hpCoefficient;
            stageFilters[k].lpStep = mStageFiltersAtStart[k].lpStep;
            stageFilters[k].settled = mStageFiltersAtStart[k].settled;
        }
    }

    void Upsample(int ch, const float* in, const ChunkControls& c)
    {
        float* x = mSignal.data();

        for (int h = 0; h < c.count; ++h)
        {
            const float sample = in ? in[h] : 0.0f;

            if (c.shift == 0)
            {
                x[h] = sample;
                continue;
            }

            float even = 0.0f, odd = 0.0f;
            mUpFirst[ch].Upsample(sample, even, odd);

            if (c.shift == 1)
            {
                x[2 * h] = even;
                x[2 * h + 1] = odd;
            }
            else
            {
                mUpSecond[ch].Upsample(even, x[4 * h], x[4 * h + 1]);
                mUpSecond[ch].Upsample(odd, x[4 * h + 2], x[4 * h + 3]);
            }
        }
    }

    void Downsample(int ch, float* out, const ChunkControls& c)
    {
        if (!out)
        {
            return;
        }

        const float* x = mSignal.data();

        for (int h = 0; h < c.count; ++h)
        {
            if (c.shift == 0)
            {
                out[h] = x[h];
            }
            else if (c.shift == 1)
            {
                out[h] = mDownFirst[ch].Downsample(x[2 * h], x[2 * h + 1]);
            }
            else
            {
                const float first = mDownSecond[ch].Downsample(x[4 * h], x[4 * h + 1]);
                const float second = mDownSecond[ch].Downsample(x[4 * h + 2], x[4 * h + 3]);
                out[h] = mDownFirst[ch].Downsample(first, second);
            }
        }
    }

    using Oversampled = std::array<float, kOversampledChunk>;

    std::array<BuiltinAmpHalfband2x, 2> mUpFirst = {}, mDownFirst = {}, mUpSecond = {}, mDownSecond = {};
    alignas(32) Oversampled mSignal = {};
    alignas(32) Oversampled mEngaged = {};
    alignas(32) Oversampled mClean = {};
    alignas(32) Oversampled mHeld = {};
    alignas(32) Oversampled mHeadroom = {};
    alignas(32) Oversampled mKneeInput = {};
    alignas(32) Oversampled mKneePart = {};
    std::array<GlidingBiquad, kFilterCount> mFiltersAtStart = {};
    std::array<StageFilter, kMaxStages> mStageFiltersAtStart = {};
};
} // namespace guitarfx::builtin_amp
