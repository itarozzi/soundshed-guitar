/**
 * @file CompressorEffectTests.cpp
 * @brief The VCA and Opto compressors (core/src/dsp/effects/CompressorEffect.h) do what they say.
 *
 *   - the static curve: the Opto's RMS detector lands on the ideal hard-knee curve; the VCA's
 *     peak detector, smoothed by its attack, comes in a little under the ideal peak curve
 *   - with Stereo Link on (the default) both channels share one gain, so a stereo image holds
 *     still; off, each is compressed on its own level
 *   - a NaN or an infinity in the input never stops either compressing, nor reaches the output
 *   - Makeup and Mix glide to a new setting instead of stepping the level
 *
 * The non-finite checks use FiniteCheck.h, so they mean the same in the fast-math builds.
 */

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "dsp/FiniteCheck.h"
#include "dsp/effects/CompressorEffect.h"

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 64;
constexpr double kPi = 3.14159265358979323846;

int gFailures = 0;
int gChecks = 0;

void Check(bool condition, const std::string& what, const std::string& detail = "")
{
    ++gChecks;
    gFailures += condition ? 0 : 1;
    std::cout << (condition ? "  [PASS] " : "  [FAIL] ") << what;

    if (!detail.empty())
    {
        std::cout << "  (" << detail << ")";
    }

    std::cout << std::endl;
}

std::string Db(double value)
{
    return std::to_string(std::round(value * 100.0) / 100.0).substr(0, 6) + " dB";
}

using Params = std::map<std::string, double>;

const Params kVca = {{"threshold", -20.0}, {"ratio", 4.0},  {"attack", 10.0}, {"release", 100.0},
                     {"knee", 0.0},        {"makeup", 0.0}, {"mix", 1.0},     {"softClip", 0.0}};
const Params kOpto = {{"threshold", -20.0}, {"ratio", 4.0}, {"attack", 20.0}, {"release", 300.0},
                      {"makeup", 0.0},      {"mix", 1.0},   {"softClip", 0.0}};

/// A 1 kHz sine through `effect`, `seconds` long, the right channel `rightOffsetDb` from the left.
/// `each(block)` runs before each block: to change a parameter, or to corrupt the input.
template <typename Each>
void Render(guitarfx::EffectProcessor& effect, double amplitudeDb, double seconds, std::vector<float>& outL,
            std::vector<float>& outR, double rightOffsetDb, Each each)
{
    const auto total = static_cast<std::size_t>(seconds * kSampleRate);
    const double amplitude = std::pow(10.0, amplitudeDb / 20.0);
    const double right = std::pow(10.0, rightOffsetDb / 20.0);
    std::vector<float> inL(kBlock), inR(kBlock), oL(kBlock), oR(kBlock);
    float* inputs[2] = {inL.data(), inR.data()};
    float* outputs[2] = {oL.data(), oR.data()};
    outL.assign(total, 0.0f);
    outR.assign(total, 0.0f);

    for (std::size_t start = 0, block = 0; start < total; start += kBlock, ++block)
    {
        for (int i = 0; i < kBlock; ++i)
        {
            const double x = amplitude * std::sin(2.0 * kPi * 1000.0 * static_cast<double>(start + i) / kSampleRate);
            inL[i] = static_cast<float>(x);
            inR[i] = static_cast<float>(x * right);
        }

        each(block, inL, inR);
        effect.Process(inputs, outputs, kBlock);

        for (int i = 0; i < kBlock && start + i < total; ++i)
        {
            outL[start + i] = oL[i];
            outR[start + i] = oR[i];
        }
    }
}

template <typename Compressor> std::unique_ptr<Compressor> Make(const Params& params)
{
    auto compressor = std::make_unique<Compressor>();
    compressor->Prepare(kSampleRate, kBlock);

    for (const auto& [key, value] : params)
    {
        compressor->SetParam(key, value);
    }

    compressor->Reset();
    return compressor;
}

/// Peak over the last `seconds` of `x`, in dBFS.
double TailPeakDb(const std::vector<float>& x, double seconds)
{
    const auto from = x.size() - static_cast<std::size_t>(seconds * kSampleRate);
    float peak = 0.0f;

    for (auto i = from; i < x.size(); ++i)
    {
        peak = std::max(peak, std::abs(x[i]));
    }

    return 20.0 * std::log10(std::max(static_cast<double>(peak), 1e-12));
}

void NoChange(std::size_t, std::vector<float>&, std::vector<float>&)
{
}

void TestStaticCurve()
{
    std::cout << "\nThe static curve, a 1 kHz sine 14 dB over a -20 dB threshold at 4:1" << std::endl;
    std::vector<float> l, r;

    auto opto = Make<guitarfx::OptoCompressorEffect>(kOpto);
    Render(*opto, -6.0, 2.0, l, r, 0.0, NoChange);
    // The detector reads RMS, 3 dB under the peak: 11 dB over, three quarters of it taken off.
    const double optoGr = -6.0 - TailPeakDb(l, 0.1);
    Check(std::abs(optoGr - 11.0103 * 0.75) < 0.3, "the Opto lands on the ideal RMS curve", Db(optoGr));

    auto vca = Make<guitarfx::CompressorEffect>(kVca);
    Render(*vca, -6.0, 2.0, l, r, 0.0, NoChange);
    // A peak detector smoothed by a 10 ms attack sees a little under the crest of each cycle.
    const double vcaGr = -6.0 - TailPeakDb(l, 0.1);
    Check(vcaGr > 10.5 - 1.5 && vcaGr < 10.5 + 0.05, "the VCA comes in just under the ideal peak curve", Db(vcaGr));

    auto under = Make<guitarfx::CompressorEffect>(kVca);
    Render(*under, -24.0, 1.0, l, r, 0.0, NoChange);
    Check(std::abs(TailPeakDb(l, 0.1) + 24.0) < 0.01, "below the threshold the VCA leaves the level alone");
}

template <typename Compressor> void ExpectLinked(const char* name, const Params& params)
{
    std::vector<float> l, r;
    auto compressor = Make<Compressor>(params);
    Render(*compressor, -6.0, 2.0, l, r, -20.0, NoChange);
    const double left = -6.0 - TailPeakDb(l, 0.1);
    const double right = -26.0 - TailPeakDb(r, 0.1);
    Check(left > 5.0 && std::abs(left - right) < 0.01,
          std::string(name) + " gives the quiet channel the loud one's gain reduction",
          "left " + Db(left) + ", right " + Db(right));

    auto independent = params;
    independent["stereoLink"] = 0.0;
    auto unlinked = Make<Compressor>(independent);
    Render(*unlinked, -6.0, 2.0, l, r, -20.0, NoChange);
    const double unlinkedLeft = -6.0 - TailPeakDb(l, 0.1);
    const double unlinkedRight = -26.0 - TailPeakDb(r, 0.1);
    Check(std::abs(unlinkedLeft - left) < 0.01 && std::abs(unlinkedRight) < 0.01,
          std::string(name) + " with Stereo Link off compresses each channel on its own level",
          "left " + Db(unlinkedLeft) + ", right " + Db(unlinkedRight));
}

void TestStereoLinked()
{
    std::cout << "\nStereo: the left channel 20 dB hotter than the right, linked (the default) and not" << std::endl;
    ExpectLinked<guitarfx::CompressorEffect>("the VCA", kVca);
    ExpectLinked<guitarfx::OptoCompressorEffect>("the Opto", kOpto);
}

template <typename Compressor> void ExpectRecovers(const char* name, const Params& params, float bad, const char* what)
{
    std::vector<float> l, r;
    auto clean = Make<Compressor>(params);
    Render(*clean, -6.0, 2.0, l, r, 0.0, NoChange);
    const double expected = TailPeakDb(l, 0.1);

    auto compressor = Make<Compressor>(params);
    Render(*compressor, -6.0, 2.0, l, r, 0.0,
           [bad](std::size_t block, std::vector<float>& inL, std::vector<float>& inR) {
               if (block == 300)
               {
                   inL[10] = bad;
                   inR[20] = bad;
               }
           });

    const bool finite = std::all_of(l.begin(), l.end(), [](float v) { return guitarfx::IsFinite(v); }) &&
                        std::all_of(r.begin(), r.end(), [](float v) { return guitarfx::IsFinite(v); });
    const double after = TailPeakDb(l, 0.1);
    Check(finite && std::abs(after - expected) < 0.05,
          std::string(name) + " after " + what + ": no non-finite output, and compressing as before",
          std::string(finite ? "finite" : "non-finite output") + ", tail peak " + Db(after) + " against " +
              Db(expected));
}

void TestRecoversFromNonFiniteInput()
{
    std::cout << "\nA NaN and an infinity in the input" << std::endl;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    ExpectRecovers<guitarfx::CompressorEffect>("the VCA", kVca, nan, "a NaN");
    ExpectRecovers<guitarfx::CompressorEffect>("the VCA", kVca, inf, "an infinity");
    ExpectRecovers<guitarfx::OptoCompressorEffect>("the Opto", kOpto, nan, "a NaN");
    ExpectRecovers<guitarfx::OptoCompressorEffect>("the Opto", kOpto, inf, "an infinity");
}

/// A sine under the threshold, so only the control being moved changes the level: the gain the
/// output shows each sample, against the input's, must never jump.
template <typename Compressor>
void ExpectGlides(const char* name, const Params& params, const char* key, double from, double to, double expectDb)
{
    auto base = params;
    base[key] = from;
    std::vector<float> l, r;
    auto compressor = Make<Compressor>(base);
    const std::size_t changeBlock = static_cast<std::size_t>(0.5 * kSampleRate) / kBlock;
    Render(*compressor, -40.0, 1.0, l, r, 0.0, [&](std::size_t block, std::vector<float>&, std::vector<float>&) {
        if (block == changeBlock)
        {
            compressor->SetParam(key, to);
        }
    });

    // The largest change in level between neighbouring cycles of the 1 kHz sine: a step shows
    // as the whole move landing between two cycles.
    const int period = static_cast<int>(kSampleRate / 1000.0);
    double largestStepDb = 0.0;
    double previous = 0.0;

    for (std::size_t start = static_cast<std::size_t>(0.4 * kSampleRate); start + period < l.size(); start += period)
    {
        float peak = 0.0f;

        for (int i = 0; i < period; ++i)
        {
            peak = std::max(peak, std::abs(l[start + i]));
        }

        const double db = 20.0 * std::log10(std::max(static_cast<double>(peak), 1e-12));

        if (previous != 0.0)
        {
            largestStepDb = std::max(largestStepDb, std::abs(db - previous));
        }

        previous = db;
    }

    const double settled = TailPeakDb(l, 0.05) + 40.0;
    Check(largestStepDb < 1.5 && std::abs(settled - expectDb) < 0.05,
          std::string(name) + " " + key + " glides to its new setting",
          "largest step between cycles " + Db(largestStepDb) + ", settles at " + Db(settled));
}

void TestLevelsGlide()
{
    std::cout << "\nMakeup and Mix, changed mid-note" << std::endl;
    ExpectGlides<guitarfx::CompressorEffect>("the VCA", kVca, "makeup", 0.0, 12.0, 12.0);
    ExpectGlides<guitarfx::OptoCompressorEffect>("the Opto", kOpto, "makeup", 0.0, 12.0, 12.0);

    // Under the threshold the wet path is the dry one plus makeup, so Mix moves the level only
    // when there is makeup to blend in.
    auto withMakeup = kVca;
    withMakeup["makeup"] = 12.0;
    ExpectGlides<guitarfx::CompressorEffect>("the VCA", withMakeup, "mix", 1.0, 0.0, 0.0);
}
} // namespace

int main()
{
    std::cout << "=== CompressorEffectTests ===" << std::endl;
    TestStaticCurve();
    TestStereoLinked();
    TestRecoversFromNonFiniteInput();
    TestLevelsGlide();
    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}
