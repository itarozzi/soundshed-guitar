/**
 * @file NamResamplerLatencyTests.cpp
 * @brief The latency a NAM resampler reports is the delay it adds.
 *
 * A NAM amp's dry path at Mix below 1, a blend's lanes and the host's delay compensation are all
 * aligned by NamOversamplingProcessor::GetLatencySamples(). The resampler's own figure counted
 * only the upsampler's pre-roll, so at a rate it had to convert it came out short: 27 samples
 * against 33 at 44.1 kHz with a 48 kHz model, 91 against 210 with a linear-phase filter. Each
 * case here runs a 30 Hz sine through with the model passed by, reads the delay from its
 * phase, and holds the report to it within a sample and a half.
 */

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "dsp/effects/NAMOversampling.h"

namespace
{
constexpr double kPi = 3.14159265358979323846;

class PassThroughNamDSP final : public ::nam::DSP
{
  public:
    PassThroughNamDSP() : ::nam::DSP(1, 1, 48000.0)
    {
    }

    void process(NAM_SAMPLE** input, NAM_SAMPLE** output, int numFrames) override
    {
        std::copy_n(input[0], numFrames, output[0]);
    }
};

struct Case
{
    double hostRate;
    double modelRate;
    int factor;
    int phaseIndex;
    const char* name;
};

/// The delay, in host samples, a 30 Hz sine picks up through the processor: from its phase
/// over the last second, low enough that no delay here is a whole period or more, by correlation against the input's
/// sine and cosine.
double MeasuredDelay(const Case& c, int& reported)
{
    constexpr double hz = 30.0;
    constexpr int block = 128;
    PassThroughNamDSP model;
    guitarfx::NamOversamplingProcessor processor;
    processor.Prepare(model, c.hostRate, c.modelRate, block, c.factor,
                      guitarfx::NamAntiAliasPhaseFromIndex(c.phaseIndex));
    reported = processor.GetLatencySamples();

    const auto total = static_cast<int>(2.0 * c.hostRate) / block * block;
    std::vector<NAM_SAMPLE> input(static_cast<std::size_t>(total));
    std::vector<NAM_SAMPLE> output(static_cast<std::size_t>(total));

    for (int n = 0; n < total; ++n)
    {
        input[static_cast<std::size_t>(n)] = static_cast<NAM_SAMPLE>(0.5 * std::sin(2.0 * kPi * hz * n / c.hostRate));
    }

    for (int start = 0; start < total; start += block)
    {
        processor.Process(model, input.data() + start, output.data() + start, block);
    }

    double re = 0.0;
    double im = 0.0;

    for (int n = total / 2; n < total; ++n)
    {
        const double phase = 2.0 * kPi * hz * n / c.hostRate;
        re += output[static_cast<std::size_t>(n)] * std::sin(phase);
        im += output[static_cast<std::size_t>(n)] * std::cos(phase);
    }

    // y = sin(w (n - d)) = sin(wn) cos(wd) - cos(wn) sin(wd): the correlations give cos and -sin.
    const double lag = std::atan2(-im, re);
    double delay = lag * c.hostRate / (2.0 * kPi * hz);

    if (delay < -0.5)
    {
        delay += c.hostRate / hz;
    }

    return delay;
}
} // namespace

int main()
{
    std::cout << "=== NamResamplerLatencyTests ===" << std::endl;
    const Case cases[] = {
        {44100.0, 48000.0, 1, 0, "44.1 kHz host, 48 kHz model, minimum phase"},
        {96000.0, 48000.0, 1, 0, "96 kHz host, 48 kHz model, minimum phase"},
        {192000.0, 48000.0, 1, 0, "192 kHz host, 48 kHz model, minimum phase"},
        {48000.0, 44100.0, 1, 0, "48 kHz host, 44.1 kHz model, minimum phase"},
        {44100.0, 48000.0, 1, 1, "44.1 kHz host, 48 kHz model, linear phase"},
        {48000.0, 48000.0, 2, 2, "48 kHz, 2x oversampling, linear phase"},
        {48000.0, 48000.0, 2, 0, "48 kHz, 2x oversampling, minimum phase"},
    };
    int failures = 0;

    for (const Case& c : cases)
    {
        int reported = 0;
        const double measured = MeasuredDelay(c, reported);
        const bool ok = std::fabs(measured - reported) <= 1.5;
        failures += ok ? 0 : 1;
        std::cout << (ok ? "  [PASS] " : "  [FAIL] ") << c.name << ": reports " << reported << ", measured " << measured
                  << std::endl;
    }

    std::cout << (failures == 0 ? "All passed" : std::to_string(failures) + " failed") << std::endl;
    return failures == 0 ? 0 : 1;
}
