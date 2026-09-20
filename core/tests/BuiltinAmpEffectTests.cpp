#include "dsp/effects/BuiltinAmpEffect.h"
#include "dsp/effects/BuiltinAmpOversampling.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

namespace
{
constexpr double kPi = 3.14159265358979323846;
int failures = 0;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::vector<float> Render(double sampleRate, double amplitude, double frequency, int blockSize, double voice,
                          int stages, double powerDrive = 0.0)
{
    constexpr int frames = 48000;
    guitarfx::BuiltinAmpEffect amp;
    amp.SetParam("voice", voice);
    amp.SetParam("stageCount", stages);
    amp.SetParam("powerDrive", powerDrive);
    amp.Prepare(sampleRate, blockSize);

    std::vector<float> input(frames), output(frames);
    for (int i = 0; i < frames; ++i)
    {
        input[i] = static_cast<float>(amplitude * std::sin(2.0 * kPi * frequency * i / sampleRate));
    }
    for (int start = 0; start < frames; start += blockSize)
    {
        const int count = std::min(blockSize, frames - start);
        float* in[2] = {input.data() + start, input.data() + start};
        float* out[2] = {output.data() + start, nullptr};
        amp.Process(in, out, count);
    }
    return output;
}

double Rms(const std::vector<float>& signal, int start)
{
    double power = 0.0;
    for (std::size_t i = static_cast<std::size_t>(start); i < signal.size(); ++i)
    {
        power += static_cast<double>(signal[i]) * signal[i];
    }
    return std::sqrt(power / static_cast<double>(signal.size() - static_cast<std::size_t>(start)));
}

double ToneMagnitude(const std::vector<float>& signal, int start, double frequency, double sampleRate)
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

void TestHalfbandLatency()
{
    guitarfx::BuiltinAmpHalfband2x upFirst, upSecond, downSecond, downFirst;
    upFirst.Prepare();
    upSecond.Prepare();
    downSecond.Prepare();
    downFirst.Prepare();

    std::vector<float> output(128);
    for (int i = 0; i < static_cast<int>(output.size()); ++i)
    {
        float a = 0.0f, b = 0.0f, c = 0.0f, d = 0.0f;
        upFirst.Upsample(i == 0 ? 1.0f : 0.0f, a, b);
        upSecond.Upsample(a, c, d);
        const float first = downSecond.Downsample(c, d);
        upSecond.Upsample(b, c, d);
        const float second = downSecond.Downsample(c, d);
        output[i] = downFirst.Downsample(first, second);
    }

    const auto peak = std::max_element(output.begin(), output.end());
    Check(std::distance(output.begin(), peak) == 48, "4x halfband latency is 48 host samples");
    double impulseSum = 0.0;
    for (const float sample : output)
    {
        impulseSum += sample;
    }
    Check(std::abs(impulseSum - 1.0) < 0.002, "4x halfband has unity DC gain");

    guitarfx::BuiltinAmpEffect amp;
    amp.Prepare(48000.0, 256);
    Check(amp.GetLatencySamples() == 48, "48 kHz processing reports 4x latency");
    amp.Prepare(96000.0, 256);
    Check(amp.GetLatencySamples() == 32, "96 kHz processing reports 2x latency");
    amp.Prepare(192000.0, 256);
    Check(amp.GetLatencySamples() == 0, "192 kHz processing reports no resampling latency");
}

void TestDynamicsAndBlocks()
{
    const auto cleanQuiet = Render(48000.0, 0.02, 440.0, 127, 0.0, 2);
    const auto cleanLoud = Render(48000.0, 0.10, 440.0, 127, 0.0, 2);
    const auto driveQuiet = Render(48000.0, 0.02, 440.0, 127, 1.0, 2);
    const auto driveLoud = Render(48000.0, 0.10, 440.0, 127, 1.0, 2);
    const double cleanRatio = Rms(cleanLoud, 4096) / Rms(cleanQuiet, 4096);
    const double driveRatio = Rms(driveLoud, 4096) / Rms(driveQuiet, 4096);
    std::cout << "Clean RMS at 0.10 input: " << Rms(cleanLoud, 4096)
              << ", drive RMS: " << Rms(driveLoud, 4096) << '\n';
    std::cout << "Clean 0.10/0.02 RMS ratio: " << cleanRatio << ", drive ratio: " << driveRatio << '\n';
    Check(cleanRatio > 2.0, "clean path retains useful input dynamics");
    Check(driveRatio > 1.1, "drive path retains useful input dynamics");
    Check(Rms(driveQuiet, 4096) > Rms(cleanQuiet, 4096) * 1.25, "drive voice increases saturation and level");

    const auto oneBlock = Render(48000.0, 0.08, 1234.0, 48000, 1.0, 4, 0.5);
    const auto manyBlocks = Render(48000.0, 0.08, 1234.0, 127, 1.0, 4, 0.5);
    double maximumDifference = 0.0;
    for (std::size_t i = 0; i < oneBlock.size(); ++i)
    {
        maximumDifference = std::max(maximumDifference, std::abs(static_cast<double>(oneBlock[i] - manyBlocks[i])));
    }
    Check(maximumDifference < 1.0e-6, "block partition does not change the output");
}

void TestAliasingAndPower()
{
    const auto brightDrive = Render(48000.0, 0.15, 7000.0, 256, 1.0, 4);
    const double fundamental = ToneMagnitude(brightDrive, 24000, 7000.0, 48000.0);
    const double alias = ToneMagnitude(brightDrive, 24000, 13000.0, 48000.0);
    const double aliasDb = 20.0 * std::log10(std::max(alias, 1.0e-12) / std::max(fundamental, 1.0e-12));
    std::cout << "7 kHz drive fundamental: " << fundamental << ", 13 kHz folded harmonic: " << aliasDb
              << " dBc\n";
    Check(fundamental > 1.0e-4, "alias test retains a measurable fundamental");
    Check(aliasDb < -35.0, "high-gain folded fifth harmonic is attenuated");

    // A 192 kHz render uses the same amplifier without oversampling. Taking
    // every fourth sample without a decimation filter shows the fold that the
    // 48 kHz half-band path must reject.
    const auto highRate = Render(192000.0, 0.15, 7000.0, 256, 1.0, 4);
    std::vector<float> unfiltered(highRate.size() / 4);
    for (std::size_t i = 0; i < unfiltered.size(); ++i)
    {
        unfiltered[i] = highRate[4 * i];
    }
    const double unfilteredFundamental = ToneMagnitude(unfiltered, 6000, 7000.0, 48000.0);
    const double unfilteredAlias = ToneMagnitude(unfiltered, 6000, 13000.0, 48000.0);
    const double unfilteredDb = 20.0 * std::log10(std::max(unfilteredAlias, 1.0e-12) /
                                                 std::max(unfilteredFundamental, 1.0e-12));
    std::cout << "Same model without decimation filtering: " << unfilteredDb << " dBc\n";
    Check(aliasDb < unfilteredDb - 20.0, "half-band path suppresses aliasing by at least 20 dB");

    const auto cleanPower = Render(48000.0, 0.10, 440.0, 256, 0.0, 2, 0.0);
    const auto drivenPower = Render(48000.0, 0.10, 440.0, 256, 0.0, 2, 1.0);
    Check(std::abs(Rms(cleanPower, 4096) - Rms(drivenPower, 4096)) > 0.02,
          "power drive changes the nonlinear output");
}
} // namespace

int main()
{
    TestHalfbandLatency();
    TestDynamicsAndBlocks();
    TestAliasingAndPower();
    if (failures)
    {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "BuiltinAmpEffectTests passed\n";
    return 0;
}
