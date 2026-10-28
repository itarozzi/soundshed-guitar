#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>

namespace guitarfx
{
/**
 * The raw input's peak while an input levelling profile is being trained: the loudest sample
 * since training started (the captured peak), the loudest since the last read (the live
 * peak), and whether any sample reached full scale. The audio thread feeds it only while it is
 * armed, so it costs nothing otherwise and works with signal diagnostics off.
 *
 * Linear amplitudes; the controller turns them into dBFS. One writer (the audio thread) and
 * one reader (the message thread), so plain stores of a running max are enough on the writer
 * side and the reader takes the live peak with an exchange.
 */
class InputLevelHold
{
  public:
    void Arm(bool armed) noexcept
    {
        if (armed)
        {
            Reset();
        }

        mArmed.store(armed, std::memory_order_release);
    }

    [[nodiscard]] bool IsArmed() const noexcept
    {
        return mArmed.load(std::memory_order_acquire);
    }

    /// Starts the capture again: the captured peak, the live peak and the clip flag.
    void Reset() noexcept
    {
        mCaptured.store(0.0f, std::memory_order_relaxed);
        mLive.store(0.0f, std::memory_order_relaxed);
        mClipped.store(false, std::memory_order_relaxed);
    }

    /// Audio thread: one block of the raw input, before the mono fold and any levelling trim.
    void Feed(const float* left, const float* right, int numSamples) noexcept
    {
        if (!mArmed.load(std::memory_order_relaxed) || numSamples <= 0)
        {
            return;
        }

        float peak = 0.0f;

        for (const float* channel : {left, right})
        {
            if (channel == nullptr)
            {
                continue;
            }

            for (int i = 0; i < numSamples; ++i)
            {
                const float magnitude = std::fabs(channel[i]);
                peak = magnitude > peak ? magnitude : peak;
            }
        }

        if (!(peak == peak)) // NaN input never becomes a peak
        {
            return;
        }

        if (peak > mCaptured.load(std::memory_order_relaxed))
        {
            mCaptured.store(peak, std::memory_order_relaxed);
        }

        if (peak > mLive.load(std::memory_order_relaxed))
        {
            mLive.store(peak, std::memory_order_relaxed);
        }

        // Same threshold as MixerTelemetry's clip count.
        if (peak > 1.0f)
        {
            mClipped.store(true, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] float CapturedPeak() const noexcept
    {
        return mCaptured.load(std::memory_order_relaxed);
    }

    /// The loudest sample since the previous call.
    [[nodiscard]] float TakeLivePeak() noexcept
    {
        return mLive.exchange(0.0f, std::memory_order_relaxed);
    }

    [[nodiscard]] bool Clipped() const noexcept
    {
        return mClipped.load(std::memory_order_relaxed);
    }

  private:
    std::atomic<bool> mArmed{false};
    std::atomic<float> mCaptured{0.0f};
    std::atomic<float> mLive{0.0f};
    std::atomic<bool> mClipped{false};
};
} // namespace guitarfx
