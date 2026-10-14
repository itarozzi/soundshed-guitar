#pragma once

#include <cstdint>
#include <cstring>

namespace guitarfx
{
/// Spots a stereo input whose two sides carry the same samples, bit for bit: a mono guitar on a
/// stereo DAW track, which hosts feed to both sides. While the sides match, MultiPresetMixer runs
/// the rigs on a mono input layout, so every amp model and cab convolver runs once rather than
/// twice; the output is the same, because a mono connection is defined to hold identical sides.
/// The first block whose sides differ puts the rigs back on stereo, through the same in-place
/// layout change a bus change makes: each node that ran mono fades its right side in
/// (StereoFadeIn).
///
/// It only ever chooses between two correct layouts. A real stereo source differs from its first
/// non-silent block and never folds; digital silence counts for neither, since any source is
/// silent at times. Folding waits for a second of matching signal, so a stereo source with a
/// moment of identical sides does not flip back and forth.
///
/// Audio thread, apart from Prepare(); no allocation.
class IdenticalInputDetector
{
  public:
    static constexpr double kFoldAfterSeconds = 1.0;

    void Prepare(double sampleRate) noexcept
    {
        mFoldAfterSamples = static_cast<std::int64_t>(sampleRate * kFoldAfterSeconds);
        Reset();
    }

    void Reset() noexcept
    {
        mMatchingSamples = 0;
        mFolded = false;
    }

    /// Takes one block of the input as the rigs will get it, and says whether they run it mono.
    bool Update(const float* left, const float* right, int numSamples) noexcept
    {
        if (!left || !right || numSamples <= 0)
        {
            Reset();
            return false;
        }

        if (left != right && std::memcmp(left, right, static_cast<std::size_t>(numSamples) * sizeof(float)) != 0)
        {
            Reset();
            return false;
        }

        if (IsDigitalSilence(left, numSamples))
        {
            return mFolded;
        }

        mMatchingSamples += numSamples;

        if (mMatchingSamples >= mFoldAfterSamples)
        {
            mFolded = true;
        }

        return mFolded;
    }

    [[nodiscard]] bool IsFolded() const noexcept
    {
        return mFolded;
    }

  private:
    static bool IsDigitalSilence(const float* samples, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            if (samples[i] != 0.0f)
            {
                return false;
            }
        }

        return true;
    }

    std::int64_t mFoldAfterSamples = 48000;
    std::int64_t mMatchingSamples = 0;
    bool mFolded = false;
};
} // namespace guitarfx
