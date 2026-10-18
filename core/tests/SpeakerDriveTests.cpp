/**
 * @file SpeakerDriveTests.cpp
 * @brief Speaker Drive's power compression follows the playing level, sags and recovers.
 *
 * Until 2026-10-18 the compressor read the input after the excursion pre-gain (up to +18 dB).
 * At Drive 1 its -18 dBFS threshold sat at -36 dBFS of real input, so any normal playing level
 * held the gain at its 6 dB limit. -12 and -6 dBFS came out exactly the same, so the stage was
 * a fixed volume cut. The detector also climbed far past the limit, so it reached the limit
 * within 5 ms and stayed there for seconds after the playing dropped. The cab tests only checked
 * that a loud note got quieter, and they passed either way.
 *
 * These checks use a 2 kHz tone, which the excursion stage (a band below ~180 Hz) barely
 * touches, so what they measure is the compressor.
 */

#include <cmath>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "dsp/FiniteCheck.h"
#include "dsp/effects/SpeakerDrive.h"

namespace
{
using guitarfx::SpeakerDrive;

constexpr double kPi = 3.14159265358979323846;
constexpr double kToneHz = 2000.0;

int gFailures = 0;

void Check(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << what << '\n';
        ++gFailures;
    }
}

void CheckRange(const std::string& label, double value, double minimum, double maximum)
{
    const bool ok = guitarfx::IsFinite(value) && value >= minimum && value <= maximum;
    Check(ok, label + ": expected " + std::to_string(minimum) + ".." + std::to_string(maximum) + ", got " +
                  std::to_string(value));
}

/// A stretch of the test tone at one level.
struct Segment
{
    double rmsDbfs;
    double seconds;
};

struct Rendered
{
    double sampleRate;
    std::vector<double> input;
    std::vector<double> output;

    /// Output against input over the 10 ms ending `seconds` in: 20 whole periods of the tone at
    /// 44.1, 48 and 96 kHz alike.
    [[nodiscard]] double GainDbAt(double seconds) const
    {
        const auto window = static_cast<std::size_t>(sampleRate / 100.0);
        const auto end = static_cast<std::size_t>(seconds * sampleRate);
        double in = 0.0;
        double out = 0.0;

        for (std::size_t i = end - window; i < end; ++i)
        {
            in += input[i] * input[i];
            out += output[i] * output[i];
        }

        return 10.0 * std::log10(out / in);
    }
};

/// The tone through a fresh SpeakerDrive at `drive`, both channels, in segments.
Rendered Render(double drive, const std::vector<Segment>& segments, double sampleRate = 48000.0)
{
    Rendered result{sampleRate, {}, {}};

    for (const Segment& segment : segments)
    {
        const double amplitude = std::pow(10.0, segment.rmsDbfs / 20.0) * std::sqrt(2.0);
        const auto count = static_cast<std::size_t>(segment.seconds * sampleRate);

        for (std::size_t i = 0; i < count; ++i)
        {
            const auto n = static_cast<double>(result.input.size());
            result.input.push_back(amplitude * std::sin(2.0 * kPi * kToneHz * n / sampleRate));
        }
    }

    SpeakerDrive speaker;
    speaker.Prepare(sampleRate);
    speaker.SetDrive(drive);
    speaker.Reset();
    result.output.reserve(result.input.size());

    for (const double sample : result.input)
    {
        result.output.push_back(speaker.Process(sample, 0));
        speaker.Process(sample, 1);
        speaker.AdvanceSample();
    }

    return result;
}

/// Steady-state gain of the tone at one level.
double SteadyGainDb(double drive, double rmsDbfs)
{
    return Render(drive, {{rmsDbfs, 1.5}}).GainDbAt(1.5);
}

void TestTransparentWhenOffOrQuiet()
{
    std::cout << "Speaker Drive is transparent at Drive 0, and at Drive 1 for quiet playing\n";

    const Rendered off = Render(0.0, {{-6.0, 0.5}});
    bool identical = true;

    for (std::size_t i = 0; i < off.input.size(); ++i)
    {
        identical = identical && off.output[i] == off.input[i];
    }

    Check(identical, "Drive 0 should return its input unchanged");
    CheckRange("Drive 1 at -40 dBFS (dB)", SteadyGainDb(1.0, -40.0), -0.05, 0.05);
}

void TestCompressionFollowsTheLevel()
{
    std::cout << "Speaker Drive compresses above the nominal level, more the louder it gets\n";

    const double at30 = SteadyGainDb(1.0, -30.0);
    const double at18 = SteadyGainDb(1.0, -18.0);
    const double at12 = SteadyGainDb(1.0, -12.0);
    const double at6 = SteadyGainDb(1.0, -6.0);
    const double at0 = SteadyGainDb(1.0, 0.0);

    CheckRange("Drive 1 at -30 dBFS, below the threshold (dB)", at30, -0.1, 0.05);
    CheckRange("Drive 1 at -18 dBFS, the nominal level (dB)", at18, -1.5, 0.0);
    CheckRange("Drive 1 at -12 dBFS (dB)", at12, -5.0, -2.0);
    CheckRange("Drive 1 at -6 dBFS, the limit (dB)", at6, -6.3, -5.0);
    CheckRange("Drive 1 at 0 dBFS, never past the limit (dB)", at0, -6.3, -5.5);

    // The bug this pins: with the pre-gain in the detector both read -6.0 dB.
    CheckRange("-6 dBFS against -12 dBFS at Drive 1 (dB)", at6 - at12, -4.0, -1.0);

    // Drive sets how much of the reduction is heard: half of 6 dB, in amplitude, at Drive 0.5.
    CheckRange("Drive 0.5 at -6 dBFS (dB)", SteadyGainDb(0.5, -6.0), -3.0, -2.0);
}

void TestALoudPassageSags()
{
    std::cout << "A loud passage sags into the compression rather than snapping to it\n";

    const Rendered step = Render(1.0, {{-40.0, 0.5}, {-6.0, 1.0}});
    CheckRange("gain 5 ms into a step to -6 dBFS (dB)", step.GainDbAt(0.505), -1.0, 0.05);
    CheckRange("gain 300 ms into a step to -6 dBFS (dB)", step.GainDbAt(0.8), -6.3, -4.0);
}

void TestRecoversAfterALoudPassage()
{
    std::cout << "Quiet playing after a loud passage recovers within about a second\n";

    for (const double sampleRate : {44100.0, 48000.0, 96000.0})
    {
        const std::string at = " at " + std::to_string(static_cast<int>(sampleRate)) + " Hz";
        const Rendered after = Render(1.0, {{0.0, 2.0}, {-30.0, 2.0}}, sampleRate);

        // Still recovering a quarter second on: heat drains slowly.
        CheckRange("gain 0.25 s after the loud passage" + at + " (dB)", after.GainDbAt(2.25), -6.3, -1.0);
        // Fully back after 1.2 s. With the detector left to climb it was still at -6 dB here.
        CheckRange("gain 1.2 s after the loud passage" + at + " (dB)", after.GainDbAt(3.2), -0.1, 0.05);
    }
}
} // namespace

int main()
{
    std::cout << "=== SpeakerDriveTests ===\n";

    TestTransparentWhenOffOrQuiet();
    TestCompressionFollowsTheLevel();
    TestALoudPassageSags();
    TestRecoversAfterALoudPassage();

    if (gFailures > 0)
    {
        std::cout << "\n" << gFailures << " check(s) failed\n";
        return 1;
    }

    std::cout << "\nAll checks passed\n";
    return 0;
}
