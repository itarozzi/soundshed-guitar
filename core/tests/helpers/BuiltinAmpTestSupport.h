#pragma once

/**
 * Measurement helpers for the Heavy American's tests (BuiltinAmpEffectTests.cpp): rendering the
 * amp over test signals and voicings, harmonic and band measurements, the heard level its makeup
 * was measured with, and the worst curvature that hears a click.
 */

#include "dsp/effects/BuiltinAmpEffect.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace builtin_amp_test
{
constexpr double kPi = 3.14159265358979323846;

inline std::vector<float> Render(double sampleRate, double amplitude, double frequency, int blockSize, double voice,
                                 int stages, double powerDrive = 0.0, double gain = 0.45)
{
    constexpr int frames = 48000;
    guitarfx::BuiltinAmpEffect amp;
    amp.SetParam("voice", voice);
    amp.SetParam("stageCount", stages);
    amp.SetParam("powerDrive", powerDrive);
    amp.SetParam("gain", gain);
    amp.Prepare(sampleRate, blockSize);

    std::vector<float> input(frames), output(frames);

    for (int i = 0; i < frames; ++i)
    {
        input[i] = static_cast<float>(amplitude * std::sin(2.0 * kPi * frequency * i / sampleRate));
    }

    for (int start = 0; start < frames; start += blockSize)
    {
        const int count = std::min(blockSize, frames - start);
        // The left of a stereo render bit for bit (TestMonoPath), without running the right.
        amp.ProcessMono(input.data() + start, output.data() + start, count);
    }

    return output;
}

inline double Rms(const std::vector<float>& signal, int start)
{
    double power = 0.0;

    for (std::size_t i = static_cast<std::size_t>(start); i < signal.size(); ++i)
    {
        power += static_cast<double>(signal[i]) * signal[i];
    }

    return std::sqrt(power / static_cast<double>(signal.size() - static_cast<std::size_t>(start)));
}

inline double ToneMagnitude(const std::vector<float>& signal, int start, double frequency, double sampleRate)
{
    double real = 0.0, imaginary = 0.0;

    for (std::size_t i = static_cast<std::size_t>(start); i < signal.size(); ++i)
    {
        const double phase = 2.0 * kPi * frequency * static_cast<double>(i) / sampleRate;
        real += signal[i] * std::cos(phase);
        imaginary += signal[i] * std::sin(phase);
    }

    return 2.0 * std::hypot(real, imaginary) / static_cast<double>(signal.size() - start);
}

// Harmonic distortion relative to the fundamental, as a fraction.
inline double Thd(const std::vector<float>& signal, double fundamental, double sampleRate)
{
    const int start = 12000;
    const double first = ToneMagnitude(signal, start, fundamental, sampleRate);
    double harmonics = 0.0;

    for (int h = 2; h <= 12 && fundamental * h < sampleRate * 0.45; ++h)
    {
        const double magnitude = ToneMagnitude(signal, start, fundamental * h, sampleRate);
        harmonics += magnitude * magnitude;
    }

    return std::sqrt(harmonics) / std::max(first, 1.0e-12);
}

enum class Signal
{
    Sine,       // A3, 220 Hz
    String,     // A3 with harmonics falling at ~9 dB/octave, like a pickup
    PowerChord, // E2 + B2
    Lead        // E5, 659.3 Hz
};

struct Voicing
{
    double gain = 0.45;
    double voice = 1.0;
    double character = 0.5;
    int stages = 2;
    double powerDrive = 0.0;
    double sag = 0.0;
};

inline std::vector<float> RenderVoicing(const Voicing& v, Signal signal, double level = 0.10, int frames = 36000)
{
    constexpr double sampleRate = 48000.0;
    guitarfx::BuiltinAmpEffect amp;
    amp.Prepare(sampleRate, 256);
    amp.SetParam("gain", v.gain);
    amp.SetParam("voice", v.voice);
    amp.SetParam("character", v.character);
    amp.SetParam("stageCount", v.stages);
    amp.SetParam("powerDrive", v.powerDrive);
    amp.SetParam("sag", v.sag);
    amp.Reset();

    std::vector<float> input(frames), output(frames);

    for (int i = 0; i < frames; ++i)
    {
        const double t = i / sampleRate;
        double x = 0.0;

        switch (signal)
        {
        case Signal::Sine:
            x = std::sin(2.0 * kPi * 220.0 * t);
            break;
        case Signal::String:

            for (int h = 1; h <= 30; ++h)
            {
                x += std::sin(2.0 * kPi * 220.0 * h * t) / std::pow(h, 1.5);
            }

            x /= 1.9;
            break;
        case Signal::PowerChord:
            x = 0.5 * (std::sin(2.0 * kPi * 82.41 * t) + std::sin(2.0 * kPi * 123.47 * t));
            break;
        case Signal::Lead:
            x = std::sin(2.0 * kPi * 659.3 * t);
            break;
        }

        input[i] = static_cast<float>(level * x);
    }

    for (int start = 0; start < frames; start += 256)
    {
        const int count = std::min(256, frames - start);
        // The left of a stereo render bit for bit (TestMonoPath), without running the right.
        amp.ProcessMono(input.data() + start, output.data() + start, count);
    }

    return output;
}

struct Biquad
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0, s1 = 0.0, s2 = 0.0;

    double Run(double x)
    {
        const double y = b0 * x + s1;
        s1 = b1 * x - a1 * y + s2;
        s2 = b2 * x - a2 * y;
        return y;
    }
};

// Level as the player hears it, the way the amp's makeup table was measured:
// a generic cab band (2nd-order 90 Hz high pass, 4.5 kHz low pass) and the
// BS.1770 K-weighting shelf, in dB.
inline double HeardDb(const std::vector<float>& signal, int start = 12000)
{
    constexpr double sampleRate = 48000.0;
    auto pass = [&](double frequency, bool high) {
        const double w = 2.0 * kPi * frequency / sampleRate, c = std::cos(w), a = std::sin(w) / std::sqrt(2.0);
        const double a0 = 1.0 + a, edge = high ? (1.0 + c) / 2.0 : (1.0 - c) / 2.0;
        return Biquad{edge / a0, (high ? -2.0 : 2.0) * edge / a0, edge / a0, -2.0 * c / a0, (1.0 - a) / a0};
    };
    const double A = std::pow(10.0, 4.0 / 40.0), w = 2.0 * kPi * 1681.0 / sampleRate;
    const double c = std::cos(w), al = std::sin(w) / std::sqrt(2.0), sA = std::sqrt(A);
    const double a0 = (A + 1.0) - (A - 1.0) * c + 2.0 * sA * al;
    Biquad shelf{A * ((A + 1.0) + (A - 1.0) * c + 2.0 * sA * al) / a0, -2.0 * A * ((A - 1.0) + (A + 1.0) * c) / a0,
                 A * ((A + 1.0) + (A - 1.0) * c - 2.0 * sA * al) / a0, 2.0 * ((A - 1.0) - (A + 1.0) * c) / a0,
                 ((A + 1.0) - (A - 1.0) * c - 2.0 * sA * al) / a0};
    Biquad highPass = pass(90.0, true), lowPass = pass(4500.0, false);

    double power = 0.0;

    for (std::size_t i = 0; i < signal.size(); ++i)
    {
        const double y = shelf.Run(lowPass.Run(highPass.Run(signal[i])));

        if (static_cast<int>(i) >= start)
        {
            power += y * y;
        }
    }

    return 10.0 * std::log10(power / static_cast<double>(signal.size() - start) + 1.0e-30);
}

inline double HeardLevel(const Voicing& v)
{
    return (HeardDb(RenderVoicing(v, Signal::Sine)) + HeardDb(RenderVoicing(v, Signal::PowerChord)) +
            HeardDb(RenderVoicing(v, Signal::Lead))) /
           3.0;
}

inline double BandShareDb(const std::vector<float>& signal, double fundamental, double low, double high, double top)
{
    double band = 0.0, all = 0.0;

    for (int h = 1; fundamental * h < top; ++h)
    {
        const double magnitude = ToneMagnitude(signal, 12000, fundamental * h, 48000.0);
        all += magnitude * magnitude;

        if (fundamental * h >= low && fundamental * h < high)
        {
            band += magnitude * magnitude;
        }
    }

    return 10.0 * std::log10(band / all + 1.0e-30);
}

inline double WorstCurvature(const std::vector<float>& output, int from, int to)
{
    double worst = 0.0;

    for (int i = from + 1; i < to - 1; ++i)
    {
        worst = std::max(worst, std::abs(static_cast<double>(output[i + 1]) - 2.0 * output[i] + output[i - 1]));
    }

    return worst;
}
} // namespace builtin_amp_test
