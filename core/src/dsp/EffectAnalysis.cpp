#include "dsp/EffectAnalysis.h"

#include "dsp/EffectRegistry.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace guitarfx::effect_analysis
{
namespace
{
constexpr int kBlockSize = 512;
/// Quiet enough that level-dependent stages are transparent, loud enough that float keeps
/// a cabinet tail well above its own rounding.
constexpr float kImpulseLevel = 1.0e-3f;

bool HasLinearResponse(const EffectProcessor& effect)
{
    const std::array<double, 1> probe = {1000.0};
    std::array<double, 1> magnitude = {};
    return effect.GetFrequencyResponse(probe, magnitude);
}
} // namespace

std::vector<double> LogFrequencies(int count)
{
    constexpr double kLowHz = 20.0;
    constexpr double kHighHz = 20000.0;
    count = std::max(count, 2);
    std::vector<double> frequencies(static_cast<std::size_t>(count));

    for (int i = 0; i < count; ++i)
    {
        frequencies[static_cast<std::size_t>(i)] =
            kLowHz * std::pow(kHighHz / kLowHz, static_cast<double>(i) / static_cast<double>(count - 1));
    }

    return frequencies;
}

std::unique_ptr<EffectProcessor> CreateConfigured(const std::string& type, const std::map<std::string, double>& params)
{
    auto effect = EffectRegistry::Instance().Create(type);

    if (!effect)
    {
        return nullptr;
    }

    for (const auto& [key, value] : params)
    {
        effect->SetParam(key, value);
    }

    effect->Prepare(kSampleRate, kBlockSize);
    effect->Reset();
    return effect;
}

std::optional<std::vector<double>> MagnitudeResponseDb(const std::string& type,
                                                       const std::map<std::string, double>& params,
                                                       const std::vector<double>& frequencies)
{
    const auto effect = CreateConfigured(type, params);
    std::vector<double> magnitudes(frequencies.size());

    if (!effect || !effect->GetFrequencyResponse(frequencies, magnitudes))
    {
        return std::nullopt;
    }

    return magnitudes;
}

std::optional<ImpulseResponse> RenderImpulse(const std::string& type, const std::map<std::string, double>& params,
                                             int length)
{
    const auto effect = CreateConfigured(type, params);

    if (!effect || length <= 0 || !HasLinearResponse(*effect))
    {
        return std::nullopt;
    }

    std::vector<float> left(static_cast<std::size_t>(length)), right(static_cast<std::size_t>(length));
    std::array<float, kBlockSize> inL = {}, inR = {};

    for (int start = 0; start < length; start += kBlockSize)
    {
        const int count = std::min(kBlockSize, length - start);
        inL.fill(0.0f);
        inR.fill(0.0f);

        if (start == 0)
        {
            inL[0] = kImpulseLevel;
            inR[0] = kImpulseLevel;
        }

        float* inputs[2] = {inL.data(), inR.data()};
        float* outputs[2] = {left.data() + start, right.data() + start};
        effect->Process(inputs, outputs, count);
    }

    const int fadeLength = std::max(1, length / 10);

    for (auto* channel : {&left, &right})
    {
        for (int i = 0; i < length; ++i)
        {
            const int intoFade = i - (length - fadeLength);
            const float fade = intoFade <= 0
                                   ? 1.0f
                                   : 0.5f * (1.0f + std::cos(3.14159265f * static_cast<float>(intoFade) / fadeLength));
            (*channel)[static_cast<std::size_t>(i)] *= fade / kImpulseLevel;
        }
    }

    ImpulseResponse response;
    response.channels.push_back(std::move(left));

    if (right != response.channels.front())
    {
        response.channels.push_back(std::move(right));
    }

    return response;
}
} // namespace guitarfx::effect_analysis
