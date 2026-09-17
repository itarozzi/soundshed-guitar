#include "dsp/SpectrumTap.h"

#include "dsp/FiniteCheck.h"
#include "dsp/SimdFFT.h"

#include <algorithm>
#include <cmath>

namespace guitarfx
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

/// -120 dB: below anything the display shows, and keeps log10 finite on silence.
constexpr double kMinPower = 1.0e-12;

double SecondsBetween(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to)
{
    return std::chrono::duration<double>(to - from).count();
}
} // namespace

SpectrumTap::SpectrumTap() : mRing(std::make_unique<float[]>(kRingSize))
{
    std::fill(mRing.get(), mRing.get() + kRingSize, 0.0f);
    mTarget.fill(static_cast<float>(kFloorDb));
    mSmoothed.fill(static_cast<float>(kFloorDb));
}

SpectrumTap::~SpectrumTap() = default;

int SpectrumTap::FftSizeFor(double sampleRate)
{
    constexpr int kMinSize = 2048;
    constexpr int kMaxSize = static_cast<int>(kRingSize / 2);
    constexpr double kTargetBinHz = 6.0;

    const double rate = (IsFinite(sampleRate) && sampleRate > 0.0) ? sampleRate : 48000.0;
    int size = kMinSize;

    while (size < kMaxSize && static_cast<double>(size) < rate / kTargetBinHz)
    {
        size *= 2;
    }

    return size;
}

double SpectrumTap::BinFrequencyHz(int bin)
{
    const double t = static_cast<double>(bin) / static_cast<double>(kBins - 1);
    return kMinFrequencyHz * std::pow(kMaxFrequencyHz / kMinFrequencyHz, t);
}

void SpectrumTap::Push(const float* left, const float* right, int numSamples) noexcept
{
    if (left == nullptr || numSamples <= 0)
    {
        return;
    }

    // A block longer than the ring only has its tail kept.
    const auto count = static_cast<std::size_t>(numSamples);
    const std::size_t skip = count > kRingSize ? count - kRingSize : 0;
    // Acquire, not relaxed: with parallel graph levels, consecutive blocks can push from
    // different threads.
    const std::uint64_t written = mWritten.load(std::memory_order_acquire);
    float* ring = mRing.get();
    std::uint64_t position = written;

    if (right != nullptr)
    {
        for (std::size_t i = skip; i < count; ++i, ++position)
        {
            ring[position & kRingMask] = 0.5f * (left[i] + right[i]);
        }
    }
    else
    {
        for (std::size_t i = skip; i < count; ++i, ++position)
        {
            ring[position & kRingMask] = left[i];
        }
    }

    mWritten.store(position, std::memory_order_release);
}

void SpectrumTap::Restart()
{
    mStartedAt = mWritten.load(std::memory_order_acquire);
    mLastAnalyzedAt = mStartedAt;
    mLastNewSamplesAt = {};
    mHasSmoothed = false;
    mTarget.fill(static_cast<float>(kFloorDb));
}

void SpectrumTap::Configure(double sampleRate)
{
    mSampleRate = sampleRate;
    const double rate = (IsFinite(sampleRate) && sampleRate > 0.0) ? sampleRate : 48000.0;
    const int size = FftSizeFor(rate);

    if (size != mFftSize || !mFft)
    {
        mFftSize = size;
        mFft = std::make_unique<SimdFFT>(static_cast<std::size_t>(size));
        mWindow.resize(static_cast<std::size_t>(size));

        for (int i = 0; i < size; ++i)
        {
            // Periodic Hann: coherent gain 0.5, which ComputeTarget() scales back out.
            mWindow[static_cast<std::size_t>(i)] =
                static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / size));
        }

        mFftIn.assign(static_cast<std::size_t>(size), {});
        mFftOut.assign(static_cast<std::size_t>(size), {});
        mPower.assign(static_cast<std::size_t>(size / 2), 0.0f);
    }

    const double binsPerHz = static_cast<double>(size) / rate;
    const double halfStep = std::pow(kMaxFrequencyHz / kMinFrequencyHz, 0.5 / static_cast<double>(kBins - 1));
    const int lastFftBin = size / 2 - 1;

    for (int bin = 0; bin < kBins; ++bin)
    {
        const double centreHz = BinFrequencyHz(bin);
        const double lowHz = centreHz / halfStep;
        const double highHz = centreHz * halfStep;
        auto& source = mBinSources[static_cast<std::size_t>(bin)];

        source.aboveNyquist = centreHz >= rate * 0.49;
        source.first = std::max(1, static_cast<int>(std::ceil(lowHz * binsPerHz)));
        source.last = std::min(lastFftBin, static_cast<int>(std::ceil(highHz * binsPerHz)) - 1);
        source.centre = std::clamp(centreHz * binsPerHz, 0.0, static_cast<double>(lastFftBin - 1));
        source.tiltDb = static_cast<float>(kTiltDbPerOctave * std::log2(centreHz / kTiltPivotHz));
    }
}

void SpectrumTap::ComputeTarget()
{
    const auto size = static_cast<std::size_t>(mFftSize);
    const std::uint64_t written = mLastAnalyzedAt;
    const std::uint64_t available = std::min<std::uint64_t>(written - mStartedAt, size);
    const float* ring = mRing.get();

    // Oldest sample first. Anything from before the tap started is silence, so a tap that
    // has only just attached shows what it has rather than a neighbour's leftovers.
    for (std::size_t i = 0; i < size; ++i)
    {
        const std::uint64_t age = size - i;
        const float sample = age <= available ? ring[(written - age) & kRingMask] : 0.0f;
        mFftIn[i] = {sample * mWindow[i], 0.0f};
    }

    mFft->Forward(mFftOut.data(), mFftIn.data());

    // A full-scale sine lands in its bin at N/4 (N/2, halved by the window), so this
    // scales it to 0 dB.
    const double amplitudeScale = 4.0 / static_cast<double>(size);
    const auto powerScale = static_cast<float>(amplitudeScale * amplitudeScale);

    for (std::size_t k = 0; k < mPower.size(); ++k)
    {
        mPower[k] = std::norm(mFftOut[k]) * powerScale;
    }

    for (int bin = 0; bin < kBins; ++bin)
    {
        const auto& source = mBinSources[static_cast<std::size_t>(bin)];

        if (source.aboveNyquist)
        {
            mTarget[static_cast<std::size_t>(bin)] = static_cast<float>(kFloorDb);
            continue;
        }

        double power = 0.0;

        if (source.first <= source.last)
        {
            // The loudest FFT bin in the band, so a harmonic reads at its own level rather
            // than diluted by the empty bins around it.
            for (int k = source.first; k <= source.last; ++k)
            {
                power = std::max(power, static_cast<double>(mPower[static_cast<std::size_t>(k)]));
            }
        }
        else
        {
            const auto k0 = static_cast<std::size_t>(source.centre);
            const double fraction = source.centre - static_cast<double>(k0);
            power = static_cast<double>(mPower[k0]) * (1.0 - fraction) + static_cast<double>(mPower[k0 + 1]) * fraction;
        }

        const double db = 10.0 * std::log10(std::max(power, kMinPower)) + source.tiltDb;
        mTarget[static_cast<std::size_t>(bin)] =
            IsFinite(db) ? static_cast<float>(std::clamp(db, kFloorDb, kCeilingDb)) : static_cast<float>(kFloorDb);
    }
}

void SpectrumTap::Analyze(double sampleRate, Bins& out, std::chrono::steady_clock::time_point now)
{
    if (!mFft || sampleRate != mSampleRate)
    {
        Configure(sampleRate);
    }

    const std::uint64_t written = mWritten.load(std::memory_order_acquire);

    if (written != mLastAnalyzedAt && written > mStartedAt)
    {
        mLastAnalyzedAt = written;
        mLastNewSamplesAt = now;
        ComputeTarget();
    }
    else if (SecondsBetween(mLastNewSamplesAt, now) > kSilentAfterSeconds)
    {
        // The node has stopped receiving signal. Let the display fall away rather than
        // freeze on the last thing it heard. Short gaps keep the last target: a host block
        // can be longer than the time between two analyses.
        mTarget.fill(static_cast<float>(kFloorDb));
    }

    if (!mHasSmoothed)
    {
        mSmoothed = mTarget;
        mHasSmoothed = true;
    }
    else
    {
        const double elapsed = std::clamp(SecondsBetween(mLastAnalyzeAt, now), 0.0, 1.0);
        const auto attack = static_cast<float>(1.0 - std::exp(-elapsed / kAttackSeconds));
        const auto release = static_cast<float>(1.0 - std::exp(-elapsed / kReleaseSeconds));

        for (std::size_t bin = 0; bin < mSmoothed.size(); ++bin)
        {
            const float target = mTarget[bin];
            float& smoothed = mSmoothed[bin];
            smoothed += (target - smoothed) * (target > smoothed ? attack : release);
        }
    }

    mLastAnalyzeAt = now;
    out = mSmoothed;
}
} // namespace guitarfx
