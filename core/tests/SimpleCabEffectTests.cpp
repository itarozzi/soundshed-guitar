#include "dsp/effects/SimpleCabEffect.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;

double GainDb(double frequency, double bass = 0.5, double presence = 0.5, double brightness = 0.5)
{
    guitarfx::SimpleCabEffect cab;
    cab.SetParam("bass", bass);
    cab.SetParam("presence", presence);
    cab.SetParam("brightness", brightness);
    cab.Prepare(kSampleRate, 512);

    constexpr int sampleCount = 24000;
    constexpr int warmup = 2400;
    std::vector<float> input(sampleCount), output(sampleCount);
    for (int i = 0; i < sampleCount; ++i)
    {
        input[i] = static_cast<float>(0.25 * std::sin(2.0 * kPi * frequency * i / kSampleRate));
    }
    float* inputs[2] = {input.data(), nullptr};
    float* outputs[2] = {output.data(), nullptr};
    cab.Process(inputs, outputs, sampleCount);

    double inputPower = 0.0, outputPower = 0.0;
    for (int i = warmup; i < sampleCount; ++i)
    {
        inputPower += static_cast<double>(input[i]) * input[i];
        outputPower += static_cast<double>(output[i]) * output[i];
    }
    return 10.0 * std::log10(outputPower / inputPower);
}

bool CheckRange(const char* label, double value, double minimum, double maximum)
{
    if (std::isfinite(value) && value >= minimum && value <= maximum)
    {
        return true;
    }
    std::cerr << label << ": expected " << minimum << ".." << maximum << " dB, got " << value << " dB\n";
    return false;
}

bool TestCabinetVoicing()
{
    const double oneKhz = GainDb(1000.0);
    // The included ENGL 4x12 and Marshall 1960 IRs put 100 Hz about 7-8 dB
    // above 1 kHz and 8 kHz about 22-28 dB below it. Broad bounds leave room
    // for a simple, general voicing while guarding against accidental retuning.
    const bool bass = CheckRange("100 Hz relative to 1 kHz", GainDb(100.0) - oneKhz, 5.0, 9.0);
    const bool mid = CheckRange("5 kHz relative to 1 kHz", GainDb(5000.0) - oneKhz, -6.0, 2.0);
    const bool treble = CheckRange("8 kHz relative to 1 kHz", GainDb(8000.0) - oneKhz, -30.0, -20.0);
    const bool brightness = CheckRange("brightness range at 8 kHz",
                                       GainDb(8000.0, 0.5, 0.5, 1.0) - GainDb(8000.0, 0.5, 0.5, 0.0),
                                       10.0, 30.0);
    return bass && mid && treble && brightness;
}

bool TestLiveBrightnessTransition()
{
    guitarfx::SimpleCabEffect changing, reference;
    changing.SetParam("brightness", 0.0);
    reference.SetParam("brightness", 0.0);
    changing.Prepare(kSampleRate, 512);
    reference.Prepare(kSampleRate, 512);

    constexpr int sampleCount = 8192;
    constexpr int changeAt = 4096;
    std::vector<float> input(sampleCount), changed(sampleCount), unchanged(sampleCount);
    for (int i = 0; i < sampleCount; ++i)
    {
        input[i] = static_cast<float>(0.5 * std::sin(2.0 * kPi * 6000.0 * i / kSampleRate));
    }

    auto process = [&](guitarfx::SimpleCabEffect& cab, std::vector<float>& output, int start, int count)
    {
        float* inputs[2] = {input.data() + start, nullptr};
        float* outputs[2] = {output.data() + start, nullptr};
        cab.Process(inputs, outputs, count);
    };
    process(changing, changed, 0, changeAt);
    process(reference, unchanged, 0, changeAt);
    changing.SetParam("brightness", 1.0);
    process(changing, changed, changeAt, sampleCount - changeAt);
    process(reference, unchanged, changeAt, sampleCount - changeAt);

    double immediateDifference = 0.0;
    for (int i = changeAt; i < changeAt + 8; ++i)
    {
        immediateDifference = std::max(immediateDifference, static_cast<double>(std::abs(changed[i] - unchanged[i])));
    }
    double changedPower = 0.0, unchangedPower = 0.0;
    for (int i = sampleCount - 2048; i < sampleCount; ++i)
    {
        changedPower += static_cast<double>(changed[i]) * changed[i];
        unchangedPower += static_cast<double>(unchanged[i]) * unchanged[i];
    }

    if (immediateDifference >= 0.05 || changedPower <= unchangedPower * 4.0)
    {
        std::cerr << "brightness transition: initial difference " << immediateDifference
                  << ", settled power ratio " << changedPower / unchangedPower << '\n';
        return false;
    }
    return true;
}

bool TestRapidAutomationStability()
{
    for (const double sampleRate : {8000.0, 48000.0})
    {
        guitarfx::SimpleCabEffect cab;
        cab.Prepare(sampleRate, 64);
        std::array<float, 64> input = {}, output = {};
        std::uint32_t noise = 0x9e3779b9u;
        float* inputs[2] = {input.data(), nullptr};
        float* outputs[2] = {output.data(), nullptr};

        for (int block = 0; block < 128; ++block)
        {
            const double setting = (block & 1) ? 1.0 : 0.0;
            cab.SetParam("bass", setting);
            cab.SetParam("presence", setting);
            cab.SetParam("brightness", setting);
            for (float& sample : input)
            {
                noise ^= noise << 13;
                noise ^= noise >> 17;
                noise ^= noise << 5;
                sample = static_cast<float>(noise) / 2147483648.0f - 1.0f;
            }
            cab.Process(inputs, outputs, static_cast<int>(output.size()));
            for (const float sample : output)
            {
                if (!std::isfinite(sample) || std::abs(sample) > 1000.0f)
                {
                    std::cerr << "nonfinite or unbounded output during automation at " << sampleRate << " Hz\n";
                    return false;
                }
            }
        }
    }
    return true;
}
} // namespace

int main()
{
    const bool voicing = TestCabinetVoicing();
    const bool transition = TestLiveBrightnessTransition();
    const bool automation = TestRapidAutomationStability();
    if (!voicing || !transition || !automation)
    {
        return 1;
    }
    std::cout << "SimpleCabEffectTests passed\n";
    return 0;
}
