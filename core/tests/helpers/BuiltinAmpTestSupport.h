#pragma once

/**
 * Measurement helpers for the Heavy American's tests (BuiltinAmpEffectTests.cpp): rendering the
 * amp over test signals, voicings and the demo guitar at the nominal level, harmonic and band
 * measurements, the heard level its makeup is measured with, and the worst curvature that hears a
 * click.
 */

#include "dsp/IRWavLoader.h"
#include "dsp/effects/BuiltinAmpEffect.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <thread>
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
    double bias = 0.0;
    double trimDb = 0.0; ///< Input Trim
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
    amp.SetParam("bias", v.bias);
    amp.SetParam("stageGain", v.trimDb);
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

// The signal as the player hears it, the way the amp's makeup table was measured: through a
// generic cab band (2nd-order 90 Hz high pass, 4.5 kHz low pass) and the BS.1770 K-weighting
// shelf.
inline std::vector<double> Heard(const std::vector<float>& signal)
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
    std::vector<double> heard(signal.size());

    for (std::size_t i = 0; i < signal.size(); ++i)
    {
        heard[i] = shelf.Run(lowPass.Run(highPass.Run(signal[i])));
    }

    return heard;
}

// Heard level in dB, from `start` on.
inline double HeardDb(const std::vector<float>& signal, int start = 12000)
{
    const auto heard = Heard(signal);
    double power = 0.0;

    for (std::size_t i = static_cast<std::size_t>(start); i < heard.size(); ++i)
    {
        power += heard[i] * heard[i];
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

// ---------------------------------------------------------------------------------------
// A guitar at the nominal level: what the level makeup is measured on and held to.
// ---------------------------------------------------------------------------------------

constexpr std::size_t kLevelBlock = 4800;

// Which 100 ms blocks of `input` are playing: those within 30 dB of its loudest.
inline std::vector<bool> PlayingBlocks(const std::vector<float>& input)
{
    std::vector<double> power(input.size() / kLevelBlock, 0.0);

    for (std::size_t i = 0; i < power.size() * kLevelBlock; ++i)
    {
        power[i / kLevelBlock] += static_cast<double>(input[i]) * input[i];
    }

    const double loudest = power.empty() ? 0.0 : *std::max_element(power.begin(), power.end());
    std::vector<bool> playing(power.size());

    for (std::size_t block = 0; block < power.size(); ++block)
    {
        playing[block] = power[block] > loudest * 1.0e-3;
    }

    return playing;
}

// A demo recording (core/ui/demo, 48 kHz), `seconds` of it from `from` on (all of it at 0), scaled
// to play at -18 dBFS RMS over its playing blocks. Empty if it does not load.
inline std::vector<float> NominalGuitar(const char* file, double from = 0.0, double seconds = 0.0)
{
    guitarfx::IRWavData data;
    std::vector<float> guitar;

    if (!guitarfx::irwav::LoadWavFile(std::filesystem::path(GUITARFX_DEMO_AUDIO_DIR) / file, data) ||
        data.sampleRate != 48000.0)
    {
        return guitar;
    }

    guitarfx::irwav::DownmixToMono(data, guitar);
    const auto first = std::min(guitar.size(), static_cast<std::size_t>(from * 48000.0));
    const auto last =
        seconds > 0.0 ? std::min(guitar.size(), first + static_cast<std::size_t>(seconds * 48000.0)) : guitar.size();
    guitar = std::vector<float>(guitar.begin() + static_cast<std::ptrdiff_t>(first),
                                guitar.begin() + static_cast<std::ptrdiff_t>(last));

    const auto playing = PlayingBlocks(guitar);
    double power = 0.0;
    std::size_t counted = 0;

    for (std::size_t i = 0; i < playing.size() * kLevelBlock; ++i)
    {
        if (playing[i / kLevelBlock])
        {
            power += static_cast<double>(guitar[i]) * guitar[i];
            ++counted;
        }
    }

    const auto gain =
        static_cast<float>(std::pow(10.0, -18.0 / 20.0) / std::sqrt(power / static_cast<double>(counted)));

    for (float& sample : guitar)
    {
        sample *= gain;
    }

    return guitar;
}

// An effect's output for `guitar`, lined up with it (its latency taken out). It has to be prepared
// at 48 kHz for blocks of 256.
inline std::vector<float> RenderGuitar(guitarfx::EffectProcessor& effect, const std::vector<float>& guitar)
{
    const auto latency = static_cast<std::size_t>(effect.GetLatencySamples());
    std::vector<float> input(guitar), output(guitar.size() + latency);
    input.resize(output.size(), 0.0f);

    for (std::size_t start = 0; start < input.size(); start += 256)
    {
        const int count = static_cast<int>(std::min<std::size_t>(256, input.size() - start));
        effect.ProcessMono(input.data() + start, output.data() + start, count);
    }

    return std::vector<float>(output.begin() + static_cast<std::ptrdiff_t>(latency), output.end());
}

// The amp's output for `guitar` at `v`.
inline std::vector<float> RenderGuitar(const Voicing& v, const std::vector<float>& guitar)
{
    guitarfx::BuiltinAmpEffect amp;
    amp.Prepare(48000.0, 256);
    amp.SetParam("gain", v.gain);
    amp.SetParam("voice", v.voice);
    amp.SetParam("character", v.character);
    amp.SetParam("stageCount", v.stages);
    amp.SetParam("powerDrive", v.powerDrive);
    amp.SetParam("sag", v.sag);
    amp.SetParam("bias", v.bias);
    amp.SetParam("stageGain", v.trimDb);
    amp.Reset();
    return RenderGuitar(amp, guitar);
}

// How much louder `output` is than `guitar`, as heard, in dB, over the blocks where it plays.
inline double HeardGainDb(const std::vector<float>& guitar, const std::vector<float>& output)
{
    const auto playing = PlayingBlocks(guitar);
    const auto in = Heard(guitar);
    const auto out = Heard(output);
    double inPower = 0.0, outPower = 0.0;

    for (std::size_t i = 0; i < playing.size() * kLevelBlock; ++i)
    {
        if (playing[i / kLevelBlock])
        {
            inPower += in[i] * in[i];
            outPower += out[i] * out[i];
        }
    }

    return 10.0 * std::log10((outPower + 1.0e-30) / (inPower + 1.0e-30));
}

// How much louder the amp at `v` makes `guitar`, as heard, in dB.
inline double HeardGainDb(const Voicing& v, const std::vector<float>& guitar)
{
    return HeardGainDb(guitar, RenderGuitar(v, guitar));
}

// The guitar the level makeup is measured on: 10 s of the demo DI and both demo riffs, each at the
// nominal level.
inline std::vector<std::vector<float>> MeasuringGuitar()
{
    return {NominalGuitar("DI_Guitar_L.wav", 10.0, 10.0), NominalGuitar("guitar-riff-01.wav"),
            NominalGuitar("guitar-riff-02.wav")};
}

// The amp's own heard gain at `v` over `guitar`: what was heard, less the makeup the amp applied
// there. What --measure-levels re-fits the makeup to.
inline double RawGainDb(const Voicing& v, const std::vector<std::vector<float>>& guitar)
{
    using namespace guitarfx::builtin_amp;
    const auto gain = static_cast<float>(v.gain), voice = static_cast<float>(v.voice);
    const auto character = static_cast<float>(v.character);
    double sum = 0.0;
    for (const auto& take : guitar)
    {
        sum += HeardGainDb(v, take);
    }
    const double makeupDb =
        LevelMakeupDb(gain, voice, character, v.stages) +
        20.0 * std::log10(PowerDriveMakeup(gain, voice, v.stages, character, static_cast<float>(v.powerDrive),
                                           static_cast<float>(v.bias), static_cast<float>(v.sag),
                                           static_cast<float>(v.trimDb)));
    return sum / static_cast<double>(guitar.size()) - makeupDb;
}

// Runs body(0) .. body(count - 1) across the machine's cores, for --measure-levels.
template <class Body> void ParallelFor(int count, const Body& body)
{
    std::atomic<int> next{0};
    std::vector<std::thread> workers;

    for (unsigned w = 0; w < std::max(1u, std::thread::hardware_concurrency()); ++w)
    {
        workers.emplace_back([&] {
            for (int i = next++; i < count; i = next++)
            {
                body(i);
            }
        });
    }

    for (auto& worker : workers)
    {
        worker.join();
    }
}
} // namespace builtin_amp_test
