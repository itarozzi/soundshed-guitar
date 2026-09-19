#include "dsp/PresetInstance.h"

#include <algorithm>
#include <cmath>

namespace guitarfx
{
void PresetInstance::ResizeBuffers(int maxBlockSize)
{
    const auto size = static_cast<size_t>(std::max(0, maxBlockSize));
    outL.assign(size, 0.0f);
    outR.assign(size, 0.0f);
    tailInL.assign(size, 0.0f);
    tailInR.assign(size, 0.0f);
}

float PresetInstance::CurrentFadeGain() const
{
    if (phase == InstancePhase::Tailing)
    {
        return tailGain;
    }

    if (phase == InstancePhase::Active || fadeTotalSamples <= 0)
    {
        return 1.0f;
    }

    const float fraction = static_cast<float>(fadeSamplesRemaining) / static_cast<float>(fadeTotalSamples);
    return (phase == InstancePhase::FadingOut) ? fraction : (1.0f - fraction);
}

void PresetInstance::BeginFadeIn(int fadeSamples)
{
    phase = InstancePhase::FadingIn;
    fadeTotalSamples = fadeSamples;
    fadeSamplesRemaining = fadeSamples;
}

void PresetInstance::BeginTail(int inputFadeSamples, int holdSamples)
{
    // Hold whatever gain the instance is at rather than snapping to unity: an instance
    // superseded halfway through its own fade-in must not get louder on its way out.
    tailGain = CurrentFadeGain();
    tailInput = true;
    phase = InstancePhase::Tailing;
    fadeTotalSamples = 0;
    fadeSamplesRemaining = 0;
    inputFadeTotalSamples = std::max(1, inputFadeSamples);
    inputFadeSamplesRemaining = inputFadeTotalSamples;
    tailSamplesRemaining = std::max(0, holdSamples);
}

void PresetInstance::FillTailInput(const float* inL, const float* inR, int numSamples)
{
    // Same ramp shape as the output crossfade, one block at a time: start where the last
    // block ended and step down to where the next one starts.
    const float total = static_cast<float>(std::max(1, inputFadeTotalSamples));
    const float start = static_cast<float>(inputFadeSamplesRemaining) / total;
    const float end = static_cast<float>(std::max(0, inputFadeSamplesRemaining - numSamples)) / total;
    const float step = (end - start) / static_cast<float>(std::max(1, numSamples));

    float gain = start;

    for (int i = 0; i < numSamples; ++i, gain += step)
    {
        const auto index = static_cast<size_t>(i);
        tailInL[index] = (inL != nullptr) ? inL[index] * gain : 0.0f;
        tailInR[index] = (inR != nullptr) ? inR[index] * gain : 0.0f;
    }
}

void PresetInstance::BeginFadeOut(int fadeSamples)
{
    // Resume the ramp from the gain we are actually at, so a switch landing mid-fade-in
    // continues smoothly downward instead of jumping to unity first.
    const float gain = CurrentFadeGain();
    phase = InstancePhase::FadingOut;
    fadeTotalSamples = std::max(1, fadeSamples);
    fadeSamplesRemaining =
        std::clamp(static_cast<int>(std::lround(gain * static_cast<double>(fadeTotalSamples))), 0, fadeTotalSamples);
}

void PresetInstance::GetFadeGains(int numSamples, float& startGain, float& endGain) const
{
    if (phase == InstancePhase::Tailing)
    {
        // Flat for the whole hold. The decay is happening inside the chain, not here.
        startGain = tailGain;
        endGain = tailGain;
        return;
    }

    if (phase == InstancePhase::Active || fadeTotalSamples <= 0)
    {
        startGain = 1.0f;
        endGain = 1.0f;
        return;
    }

    const float total = static_cast<float>(fadeTotalSamples);
    const int endRemaining = std::max(0, fadeSamplesRemaining - numSamples);
    const float startFraction = static_cast<float>(fadeSamplesRemaining) / total;
    const float endFraction = static_cast<float>(endRemaining) / total;

    if (phase == InstancePhase::FadingOut)
    {
        // remaining/total: 1 -> 0
        startGain = startFraction;
        endGain = endFraction;
    }
    else
    {
        // 1 - remaining/total: 0 -> 1
        startGain = 1.0f - startFraction;
        endGain = 1.0f - endFraction;
    }
}

void PresetInstance::AdvanceRamps(int numSamples, int releaseSamples)
{
    if (phase == InstancePhase::Active)
    {
        return;
    }

    if (tailInput)
    {
        inputFadeSamplesRemaining = std::max(0, inputFadeSamplesRemaining - numSamples);
    }

    if (phase == InstancePhase::Tailing)
    {
        tailSamplesRemaining = std::max(0, tailSamplesRemaining - numSamples);

        // Output silence cannot prove a delay line is empty: repeats and predelays
        // can be arbitrarily far apart. Keep state until the configured budget ends.
        if (tailSamplesRemaining == 0)
        {
            BeginFadeOut(releaseSamples);
        }

        return;
    }

    fadeSamplesRemaining = std::max(0, fadeSamplesRemaining - numSamples);

    if (fadeSamplesRemaining == 0 && phase == InstancePhase::FadingIn)
    {
        phase = InstancePhase::Active;
        fadeTotalSamples = 0;
    }
}
} // namespace guitarfx
