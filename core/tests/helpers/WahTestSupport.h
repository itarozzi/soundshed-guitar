#pragma once

/**
 * @file WahTestSupport.h
 * @brief What the wah's test suites share: a wah built from the registry, renders, test signals, the
 * resonant peak and Q of an impulse response, and the pass/fail tally.
 *
 * Used by WahEffectTests, WahEnvelopeTests and WahLegacyMigrationTests. Each is its own
 * executable, so the tally is per suite.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"

namespace wah_test
{
inline constexpr double kSampleRate = 48000.0;
inline constexpr int kBlockSize = 256;
inline constexpr double kPi = 3.14159265358979323846;

using Params = std::vector<std::pair<std::string, double>>;

inline int gFailures = 0;
inline int gChecks = 0;

inline void Check(bool condition, const std::string& what, const std::string& detail = "")
{
    ++gChecks;

    if (condition)
    {
        std::cout << "  [PASS] " << what;
    }
    else
    {
        ++gFailures;
        std::cout << "  [FAIL] " << what;
    }

    if (!detail.empty())
    {
        std::cout << "  (" << detail << ")";
    }

    std::cout << std::endl;
}

inline std::string Num(double v, int precision = 3)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", precision, v);
    return std::string(buffer);
}

inline std::string Db(double gain)
{
    return Num(20.0 * std::log10(gain), 2) + " dB";
}

/// A fixed sweep with the tone shaping off, so the wah is a plain linear bandpass: heel 450 Hz
/// at Q 8, toe 2200 Hz at Q 2, even taper, no low-end bleed, flat treble, no saturation. Tests
/// set it explicitly so they do not move when the default voicing is retuned.
inline Params Linear(Params extra)
{
    Params params = {{"heelFreq", 450.0}, {"toeFreq", 2200.0}, {"taper", 0.0},  {"q", 8.0},         {"toeQScale", 0.25},
                     {"toeGain", 0.0},    {"lowEnd", 0.0},     {"treble", 0.0}, {"saturation", 0.0}};
    params.insert(params.end(), extra.begin(), extra.end());
    return params;
}

inline std::unique_ptr<guitarfx::EffectProcessor> MakeWah(const Params& params, double sampleRate = kSampleRate)
{
    auto effect = guitarfx::EffectRegistry::Instance().Create(guitarfx::EffectGuids::kWah);

    if (!effect)
    {
        return nullptr;
    }

    for (const auto& [key, value] : params)
    {
        effect->SetParam(key, value);
    }

    effect->Prepare(sampleRate, kBlockSize);
    return effect;
}

/// Runs `input` through both channels in blocks and returns the left output.
inline std::vector<float> Render(guitarfx::EffectProcessor& effect, const std::vector<float>& input)
{
    std::vector<float> scratch(input);
    std::vector<float> outL(input.size(), 0.0f);
    std::vector<float> outR(input.size(), 0.0f);

    for (std::size_t start = 0; start < input.size(); start += kBlockSize)
    {
        const int n = static_cast<int>(std::min<std::size_t>(kBlockSize, input.size() - start));
        float* inputs[2] = {scratch.data() + start, scratch.data() + start};
        float* outputs[2] = {outL.data() + start, outR.data() + start};
        effect.Process(inputs, outputs, n);
    }

    return outL;
}

inline std::vector<float> Silence(double seconds)
{
    return std::vector<float>(static_cast<std::size_t>(seconds * kSampleRate), 0.0f);
}

inline std::vector<float> Sine(double hz, double amplitude, double seconds)
{
    std::vector<float> out(static_cast<std::size_t>(seconds * kSampleRate));

    for (std::size_t i = 0; i < out.size(); ++i)
    {
        out[i] = static_cast<float>(amplitude * std::sin(2.0 * kPi * hz * static_cast<double>(i) / kSampleRate));
    }

    return out;
}

struct Noise
{
    std::uint32_t state = 0x2545F491u;
    float b0 = 0.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;

    float White()
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<float>(state) / 2147483648.0f - 1.0f;
    }

    /// Paul Kellet's economy pink filter: within about 0.5 dB of -3 dB/octave over the audio band.
    float Pink()
    {
        const float white = White();
        b0 = 0.99765f * b0 + white * 0.0990460f;
        b1 = 0.96300f * b1 + white * 0.2965164f;
        b2 = 0.57000f * b2 + white * 1.0526913f;
        return 0.25f * (b0 + b1 + b2 + white * 0.1848f);
    }
};

inline std::vector<float> WhiteNoise(double seconds, double sampleRate = kSampleRate)
{
    Noise noise;
    std::vector<float> out(static_cast<std::size_t>(seconds * sampleRate));

    for (auto& sample : out)
    {
        sample = noise.White();
    }

    return out;
}

inline std::vector<float> PinkNoise(double seconds)
{
    Noise noise;
    std::vector<float> out(static_cast<std::size_t>(seconds * kSampleRate));

    for (auto& sample : out)
    {
        sample = noise.Pink();
    }

    return out;
}

inline double Rms(const std::vector<float>& signal, std::size_t start = 0)
{
    double sum = 0.0;

    for (std::size_t i = start; i < signal.size(); ++i)
    {
        sum += static_cast<double>(signal[i]) * signal[i];
    }

    return std::sqrt(sum / static_cast<double>(std::max<std::size_t>(1, signal.size() - start)));
}

inline double MaxDifference(const std::vector<float>& a, const std::vector<float>& b)
{
    double largest = 0.0;

    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i)
    {
        largest = std::max(largest, static_cast<double>(std::abs(a[i] - b[i])));
    }

    return largest;
}

inline std::vector<float> ImpulseResponse(guitarfx::EffectProcessor& effect)
{
    std::vector<float> impulse(16384, 0.0f);
    impulse[0] = 1.0f;
    return Render(effect, impulse);
}

inline double MagnitudeAt(const std::vector<float>& ir, double hz)
{
    const double w = 2.0 * kPi * hz / kSampleRate;
    double re = 0.0;
    double im = 0.0;

    for (std::size_t n = 0; n < ir.size(); ++n)
    {
        re += ir[n] * std::cos(w * static_cast<double>(n));
        im -= ir[n] * std::sin(w * static_cast<double>(n));
    }

    return std::sqrt(re * re + im * im);
}

struct Peak
{
    double hz = 0.0;
    double gain = 0.0;
};

/// A 1/24-octave search from 100 Hz to 8 kHz, refined by a parabola through the dB values.
inline Peak FindPeak(const std::vector<float>& ir)
{
    constexpr double kLowHz = 100.0;
    constexpr double kStepsPerOctave = 24.0;
    const int steps = static_cast<int>(std::ceil(std::log2(8000.0 / kLowHz) * kStepsPerOctave));
    std::vector<double> db(static_cast<std::size_t>(steps) + 1);
    std::size_t best = 0;

    for (std::size_t s = 0; s < db.size(); ++s)
    {
        db[s] = 20.0 * std::log10(MagnitudeAt(ir, kLowHz * std::pow(2.0, s / kStepsPerOctave)) + 1.0e-12);
        best = (db[s] > db[best]) ? s : best;
    }

    double offset = 0.0;

    if (best > 0 && best + 1 < db.size())
    {
        const double curvature = db[best - 1] - 2.0 * db[best] + db[best + 1];
        offset = (curvature < 0.0) ? 0.5 * (db[best - 1] - db[best + 1]) / curvature : 0.0;
    }

    Peak peak;
    peak.hz = kLowHz * std::pow(2.0, (static_cast<double>(best) + offset) / kStepsPerOctave);
    peak.gain = MagnitudeAt(ir, peak.hz);
    return peak;
}

/// Centre frequency over the -3 dB bandwidth, with each edge found by bisection.
inline double MeasureQ(const std::vector<float>& ir, const Peak& peak)
{
    const double halfPower = peak.gain / std::sqrt(2.0);
    const auto edge = [&](double inside, double outside) {
        for (int i = 0; i < 40; ++i)
        {
            const double mid = std::sqrt(inside * outside);
            (MagnitudeAt(ir, mid) > halfPower ? inside : outside) = mid;
        }

        return std::sqrt(inside * outside);
    };

    const double lower = edge(peak.hz, peak.hz / 8.0);
    const double upper = edge(peak.hz, std::min(peak.hz * 8.0, 0.49 * kSampleRate));
    return peak.hz / (upper - lower);
}

inline bool Near(double actual, double expected, double relativeTolerance)
{
    return std::abs(actual - expected) <= std::abs(expected) * relativeTolerance;
}
} // namespace wah_test
