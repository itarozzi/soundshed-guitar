#include "dsp/LoudnessTap.h"

#include "dsp/FiniteCheck.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace guitarfx
{
namespace
{
double SecondsBetween(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to)
{
    return std::chrono::duration<double>(to - from).count();
}

/// Copies `count` samples into a ring at `position`, wrapping at its end. `count` is at most the
/// ring's size.
void CopyIntoRing(float* ring, std::size_t ringFrames, std::uint64_t position, const float* source,
                  std::size_t count) noexcept
{
    const std::size_t offset = static_cast<std::size_t>(position & (ringFrames - 1));
    const std::size_t first = std::min(count, ringFrames - offset);
    std::memcpy(ring + offset, source, first * sizeof(float));
    std::memcpy(ring, source + first, (count - first) * sizeof(float));
}
} // namespace

LoudnessTap::LoudnessTap()
    : mLeft(std::make_unique<float[]>(kRingFrames)), mRight(std::make_unique<float[]>(kRingFrames))
{
    std::fill(mLeft.get(), mLeft.get() + kRingFrames, 0.0f);
    std::fill(mRight.get(), mRight.get() + kRingFrames, 0.0f);
}

LoudnessTap::~LoudnessTap() = default;

void LoudnessTap::Push(const float* left, const float* right, int numSamples) noexcept
{
    if (left == nullptr || numSamples <= 0)
    {
        return;
    }

    // A block longer than the ring only has its tail kept.
    const auto count = static_cast<std::size_t>(numSamples);
    const std::size_t skip = count > kRingFrames ? count - kRingFrames : 0;
    const std::size_t kept = count - skip;
    // Acquire, not relaxed: with parallel graph levels, consecutive blocks can push from
    // different threads.
    const std::uint64_t written = mWritten.load(std::memory_order_acquire);

    // A mono signal is written to both sides, so the reader never has to know which blocks were.
    CopyIntoRing(mLeft.get(), kRingFrames, written, left + skip, kept);
    CopyIntoRing(mRight.get(), kRingFrames, written, (right != nullptr ? right : left) + skip, kept);
    mWritten.store(written + kept, std::memory_order_release);
}

void LoudnessTap::Restart()
{
    mRead = mWritten.load(std::memory_order_acquire);
    mMeter.Reset();
    mClockStarted = false;
}

LoudnessTap::Reading LoudnessTap::Analyze(double sampleRate, std::chrono::steady_clock::time_point now)
{
    const double rate = (IsFinite(sampleRate) && sampleRate > 0.0) ? sampleRate : 48000.0;

    if (rate != mSampleRate)
    {
        // A new rate needs a new filter, and makes what was measured at the old one meaningless,
        // along with anything still in the ring from before the change. The first read has no
        // old rate, and keeps what was pushed ahead of it.
        const bool rateChanged = mSampleRate > 0.0;
        mSampleRate = rate;
        mMeter.Prepare(rate);
        mClockStarted = false;

        if (rateChanged)
        {
            mRead = mWritten.load(std::memory_order_acquire);
        }
    }

    if (!mClockStarted)
    {
        mLastNewSamplesAt = now;
        mSilenceFedUntil = now;
        mClockStarted = true;
    }

    const std::uint64_t written = mWritten.load(std::memory_order_acquire);
    std::uint64_t from = mRead;

    if (written - from > kMaxBacklogFrames)
    {
        from = written - kMaxBacklogFrames;
    }

    // Straight from the ring, at most two runs, so the reader needs no buffer of its own.
    while (from < written)
    {
        const std::size_t offset = static_cast<std::size_t>(from & kRingMask);
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(written - from, kRingFrames - offset));
        mMeter.Push(mLeft.get() + offset, mRight.get() + offset, count);
        from += count;
    }

    if (written != mRead)
    {
        mRead = written;
        mLastNewSamplesAt = now;
        mSilenceFedUntil = now;
    }
    else if (SecondsBetween(mLastNewSamplesAt, now) >= kSilentAfterSeconds)
    {
        const double seconds = std::min(kMaxSilenceSeconds, SecondsBetween(mSilenceFedUntil, now));

        if (seconds > 0.0)
        {
            mMeter.PushSilence(static_cast<std::size_t>(std::llround(seconds * rate)));
        }

        mSilenceFedUntil = now;
    }

    Reading reading;
    reading.momentaryLufs = mMeter.MomentaryLufs();
    reading.shortTermLufs = mMeter.ShortTermLufs();
    reading.integratedLufs = mMeter.IntegratedLufs();
    reading.measuredSeconds = mMeter.MeasuredSeconds();
    return reading;
}
} // namespace guitarfx
