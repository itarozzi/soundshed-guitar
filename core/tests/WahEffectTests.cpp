/**
 * @file WahEffectTests.cpp
 * @brief Tests for the pedal-controlled wah.
 *
 * These measure what the pedal claims to do, rather than only that it makes sound:
 *   - registration, the parameter table and clamping
 *   - the resonant peak sits at Heel Freq, Toe Freq, and their geometric mean mid-travel
 *   - the measured Q follows the Q knob and Toe Q Scale
 *   - Taper bends mid-travel the documented way
 *   - the 2 sqrt(Q) peak gain law, Toe Gain, Level and Mix
 *   - pink noise stays at a steady level across the sweep and the Q knob
 *   - Low End and Treble shape the response where they claim to
 *   - pedal smoothing glides at the Response time constant and then settles exactly
 *   - Auto-Engage switches off after resting at the heel and back on when the pedal moves
 *   - Saturation compresses a hot peak and leaves a quiet one alone
 *   - every factory preset is well formed, sensibly loud, and stable under a fast sweep
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/effects/WahEffect.h"

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 256;
constexpr double kPi = 3.14159265358979323846;

using Params = std::vector<std::pair<std::string, double>>;

int gFailures = 0;
int gChecks = 0;

void Check(bool condition, const std::string& what, const std::string& detail = "")
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

std::string Num(double v, int precision = 3)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", precision, v);
    return std::string(buffer);
}

std::string Db(double gain)
{
    return Num(20.0 * std::log10(gain), 2) + " dB";
}

/// A fixed sweep with the tone shaping off, so the wah is a plain linear bandpass: heel 450 Hz
/// at Q 8, toe 2200 Hz at Q 2, even taper, no low-end bleed, flat treble, no saturation. Tests
/// set it explicitly so they do not move when the default voicing is retuned.
Params Linear(Params extra)
{
    Params params = {{"heelFreq", 450.0}, {"toeFreq", 2200.0}, {"taper", 0.0},  {"q", 8.0},         {"toeQScale", 0.25},
                     {"toeGain", 0.0},    {"lowEnd", 0.0},     {"treble", 0.0}, {"saturation", 0.0}};
    params.insert(params.end(), extra.begin(), extra.end());
    return params;
}

std::unique_ptr<guitarfx::EffectProcessor> MakeWah(const Params& params, double sampleRate = kSampleRate)
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
std::vector<float> Render(guitarfx::EffectProcessor& effect, const std::vector<float>& input)
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

std::vector<float> Silence(double seconds)
{
    return std::vector<float>(static_cast<std::size_t>(seconds * kSampleRate), 0.0f);
}

std::vector<float> Sine(double hz, double amplitude, double seconds)
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

std::vector<float> WhiteNoise(double seconds, double sampleRate = kSampleRate)
{
    Noise noise;
    std::vector<float> out(static_cast<std::size_t>(seconds * sampleRate));

    for (auto& sample : out)
    {
        sample = noise.White();
    }

    return out;
}

std::vector<float> PinkNoise(double seconds)
{
    Noise noise;
    std::vector<float> out(static_cast<std::size_t>(seconds * kSampleRate));

    for (auto& sample : out)
    {
        sample = noise.Pink();
    }

    return out;
}

double Rms(const std::vector<float>& signal, std::size_t start = 0)
{
    double sum = 0.0;

    for (std::size_t i = start; i < signal.size(); ++i)
    {
        sum += static_cast<double>(signal[i]) * signal[i];
    }

    return std::sqrt(sum / static_cast<double>(std::max<std::size_t>(1, signal.size() - start)));
}

double MaxDifference(const std::vector<float>& a, const std::vector<float>& b)
{
    double largest = 0.0;

    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i)
    {
        largest = std::max(largest, static_cast<double>(std::abs(a[i] - b[i])));
    }

    return largest;
}

std::vector<float> ImpulseResponse(guitarfx::EffectProcessor& effect)
{
    std::vector<float> impulse(16384, 0.0f);
    impulse[0] = 1.0f;
    return Render(effect, impulse);
}

double MagnitudeAt(const std::vector<float>& ir, double hz)
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
Peak FindPeak(const std::vector<float>& ir)
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
double MeasureQ(const std::vector<float>& ir, const Peak& peak)
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

bool Near(double actual, double expected, double relativeTolerance)
{
    return std::abs(actual - expected) <= std::abs(expected) * relativeTolerance;
}

void TestRegistration()
{
    std::cout << "\nRegistration and parameters\n";
    auto& registry = guitarfx::EffectRegistry::Instance();
    const auto info = registry.GetTypeInfo(guitarfx::EffectGuids::kWah);

    Check(info.has_value(), "wah is registered");
    Check(registry.Resolve("wah") == guitarfx::EffectGuids::kWah, "the \"wah\" alias resolves to the UUID");

    if (!info)
    {
        return;
    }

    Check(info->parameters.size() == guitarfx::wah::kParamCount, "every parameter in the table is registered");

    const auto* position = registry.FindParameter(guitarfx::EffectGuids::kWah, "position");
    Check(position != nullptr && position->minValue == 0.0 && position->maxValue == 1.0 && position->step == 0.0,
          "Pedal Position is a continuous 0..1 parameter, so a mapped pedal is never snapped");

    auto wah = MakeWah({});
    wah->SetParam("position", 2.0);
    Check(wah->GetParam("position") == 1.0, "position clamps to its range");
    wah->SetParam("heelFreq", 10.0);
    Check(wah->GetParam("heelFreq") == 150.0, "Heel Freq clamps to its range");
    wah->SetParam("q", 0.3);
    wah->SetParam("q", std::numeric_limits<double>::infinity());
    Check(wah->GetParam("q") == 0.5, "a non-finite value is ignored", "q=" + Num(wah->GetParam("q")));
    Check(wah->GetParam("noSuchParam") == 0.0, "an unknown key reads as 0");
}

void TestSweepEndpoints()
{
    std::cout << "\nThe resonant peak follows the pedal\n";
    const std::vector<std::pair<double, double>> cases = {
        {0.0, 450.0}, {0.5, std::sqrt(450.0 * 2200.0)}, {1.0, 2200.0}};

    for (const auto& [position, expectedHz] : cases)
    {
        auto wah = MakeWah(Linear({{"position", position}}));
        const auto peak = FindPeak(ImpulseResponse(*wah));
        Check(Near(peak.hz, expectedHz, 0.02),
              "position " + Num(position, 1) + " peaks at " + Num(expectedHz, 0) + " Hz",
              "measured " + Num(peak.hz, 1) + " Hz");
        Check(Near(wah->GetParam("currentFrequency"), expectedHz, 1.0e-9), "currentFrequency reports it");
    }
}

void TestQ()
{
    std::cout << "\nQ follows the Q knob and Toe Q Scale\n";

    const auto measure = [](const Params& params) {
        auto wah = MakeWah(Linear(params));
        const auto ir = ImpulseResponse(*wah);
        return MeasureQ(ir, FindPeak(ir));
    };

    const double heelQ = measure({{"position", 0.0}});
    Check(Near(heelQ, 8.0, 0.08), "Q at the heel is the Q knob", "measured " + Num(heelQ, 2));

    const double toeQ = measure({{"position", 1.0}});
    Check(Near(toeQ, 2.0, 0.08), "Q at the toe is Q x Toe Q Scale", "measured " + Num(toeQ, 2));

    const double flatToeQ = measure({{"position", 1.0}, {"toeQScale", 1.0}});
    Check(Near(flatToeQ, 8.0, 0.10), "Toe Q Scale 1 holds Q across the sweep", "measured " + Num(flatToeQ, 2));
}

void TestTaper()
{
    std::cout << "\nTaper bends pedal travel\n";

    const auto midTravelHz = [](double taper) {
        auto wah = MakeWah(Linear({{"position", 0.5}, {"taper", taper}}));
        return wah->GetParam("currentFrequency");
    };

    const auto expected = [](double taper) {
        const double warped = std::pow(0.5, std::pow(3.0, -taper));
        return 450.0 * std::pow(2200.0 / 450.0, warped);
    };

    const double towardToe = midTravelHz(-1.0);
    const double even = midTravelHz(0.0);
    const double towardHeel = midTravelHz(1.0);

    Check(towardToe < even && even < towardHeel, "negative taper holds mid-travel low, positive pushes it high",
          Num(towardToe, 0) + " < " + Num(even, 0) + " < " + Num(towardHeel, 0) + " Hz");
    Check(Near(towardToe, expected(-1.0), 1.0e-9) && Near(towardHeel, expected(1.0), 1.0e-9),
          "taper is the documented power curve");
}

void TestGainLawLevelAndMix()
{
    std::cout << "\nPeak gain, Toe Gain, Level and Mix\n";

    const auto peakGain = [](const Params& params) {
        auto wah = MakeWah(Linear(params));
        return FindPeak(ImpulseResponse(*wah)).gain;
    };

    const auto law = [](double q) { return guitarfx::wah::kPeakGainScale * std::sqrt(q); };
    const double heel = peakGain({{"position", 0.0}});
    const double mid = peakGain({{"position", 0.5}});
    const double toe = peakGain({{"position", 1.0}});

    Check(Near(heel, law(8.0), 0.02), "heel peak gain is 2 sqrt(Q)", Db(heel));
    Check(Near(mid, law(4.0), 0.02), "mid-travel peak gain follows the same law", Db(mid));
    Check(Near(toe, law(2.0), 0.02), "toe peak gain follows the same law", Db(toe));

    const double tiltedToe = peakGain({{"position", 1.0}, {"toeGain", 6.0}});
    const double tiltedHeel = peakGain({{"position", 0.0}, {"toeGain", 6.0}});
    Check(Near(tiltedToe / toe, std::pow(10.0, 6.0 / 20.0), 0.005), "Toe Gain +6 dB lifts the toe peak 6 dB",
          Db(tiltedToe / toe));
    Check(Near(tiltedHeel / heel, 1.0, 0.005), "and leaves the heel alone", Db(tiltedHeel / heel));

    const double boosted = peakGain({{"position", 0.5}, {"level", 6.0}});
    Check(Near(boosted / mid, std::pow(10.0, 6.0 / 20.0), 0.005), "Level +6 dB doubles the wet signal",
          Db(boosted / mid));

    const auto input = WhiteNoise(0.2);
    auto dry = MakeWah({{"mix", 0.0}, {"saturation", 1.0}});
    const double difference = MaxDifference(Render(*dry, input), input);
    Check(difference <= 1.0e-6, "Mix 0 passes the dry signal", "max difference " + std::to_string(difference));
}

void TestPinkLevel()
{
    std::cout << "\nPink noise holds its level\n";
    const auto input = PinkNoise(3.0);
    const std::size_t start = input.size() / 8;
    const double inputRms = Rms(input, start);

    const auto levelDb = [&](const Params& params) {
        auto wah = MakeWah(Linear(params));
        return 20.0 * std::log10(Rms(Render(*wah, input), start) / inputRms);
    };

    const double heel = levelDb({{"position", 0.0}});
    const double mid = levelDb({{"position", 0.5}});
    const double toe = levelDb({{"position", 1.0}});
    const double spread = std::max({heel, mid, toe}) - std::min({heel, mid, toe});
    Check(spread < 1.5, "across the sweep", Num(heel, 2) + " / " + Num(mid, 2) + " / " + Num(toe, 2) + " dB");

    const double narrow = levelDb({{"position", 0.5}, {"q", 16.0}});
    const double broad = levelDb({{"position", 0.5}, {"q", 2.0}});
    Check(std::abs(narrow - broad) < 1.5, "across the Q knob",
          "Q 16: " + Num(narrow, 2) + " dB, Q 2: " + Num(broad, 2) + " dB");
}

void TestLowEndAndTreble()
{
    std::cout << "\nLow End and Treble\n";

    const auto magnitude = [](const Params& params, double hz) {
        auto wah = MakeWah(Linear(params));
        return MagnitudeAt(ImpulseResponse(*wah), hz);
    };

    const double withBass = magnitude({{"position", 0.0}, {"lowEnd", 1.0}}, 60.0);
    const double withoutBass = magnitude({{"position", 0.0}, {"lowEnd", 0.0}}, 60.0);
    Check(withBass > 0.85, "Low End 1 keeps 60 Hz near unity", Num(withBass));
    Check(withoutBass < 0.15, "Low End 0 is a bandpass that drops 60 Hz", Num(withoutBass));

    const auto atEightKhz = [&](double trebleDb) {
        return magnitude({{"position", 1.0}, {"treble", trebleDb}}, 8000.0);
    };
    const double flat = atEightKhz(0.0);
    const double lifted = atEightKhz(12.0) / flat;
    const double cut = atEightKhz(-12.0) / flat;
    Check(lifted > 2.5, "Treble +12 dB lifts 8 kHz", Db(lifted));
    Check(cut < 0.45, "Treble -12 dB cuts 8 kHz", Db(cut));
}

void TestPedalGlide()
{
    std::cout << "\nPedal smoothing\n";
    auto wah = MakeWah(Linear({{"position", 0.0}, {"response", 20.0}}));
    wah->SetParam("position", 1.0);

    const auto progress = [&]() {
        return std::log2(wah->GetParam("currentFrequency") / 450.0) / std::log2(2200.0 / 450.0);
    };
    const auto expected = [](int samples) { return 1.0 - std::exp(-samples / (0.020 * kSampleRate)); };

    Render(*wah, Silence(static_cast<double>(kBlockSize) / kSampleRate));
    const double afterOneBlock = progress();
    Check(std::abs(afterOneBlock - expected(kBlockSize)) < 0.01, "a pedal jump glides instead of stepping",
          "after 1 block " + Num(afterOneBlock) + ", expected " + Num(expected(kBlockSize)));

    Render(*wah, Silence(3.0 * kBlockSize / kSampleRate));
    const double afterFourBlocks = progress();
    Check(std::abs(afterFourBlocks - expected(4 * kBlockSize)) < 0.01, "it glides at the Response time constant",
          "after 4 blocks " + Num(afterFourBlocks) + ", expected " + Num(expected(4 * kBlockSize)));

    Render(*wah, Silence(1.0));
    Check(Near(wah->GetParam("currentFrequency"), 2200.0, 1.0e-9), "and settles on the target",
          Num(wah->GetParam("currentFrequency"), 6) + " Hz");
}

void TestAutoEngage()
{
    std::cout << "\nAuto-Engage\n";

    auto parked = MakeWah({{"autoEngage", 1.0}, {"position", 0.0}});
    Check(parked->GetParam("engaged") == 0.0, "a wah loaded with its pedal at the heel starts off");

    auto wah = MakeWah({{"autoEngage", 1.0}, {"position", 0.5}});
    Check(wah->GetParam("engaged") == 1.0, "a wah loaded with its pedal forward starts on");

    wah->SetParam("position", 0.0);
    Render(*wah, WhiteNoise(0.3));
    Check(wah->GetParam("engaged") == 1.0, "a brief rest at the heel keeps it on");

    Render(*wah, WhiteNoise(0.7));
    const auto input = WhiteNoise(0.1);
    const double difference = MaxDifference(Render(*wah, input), input);
    Check(wah->GetParam("engaged") == 0.0 && difference <= 1.0e-6,
          "resting at the heel past the hold switches it off, to the dry signal",
          "max difference " + std::to_string(difference));

    wah->SetParam("position", 0.3);
    Render(*wah, WhiteNoise(0.2));
    Check(wah->GetParam("engaged") > 0.999, "moving the pedal switches it back on",
          "engaged " + Num(wah->GetParam("engaged"), 4));
}

void TestSaturation()
{
    std::cout << "\nSaturation\n";

    const auto gainDb = [](double saturation, double amplitude) {
        auto wah = MakeWah(Linear({{"position", 0.0}, {"saturation", saturation}}));
        const auto input = Sine(450.0, amplitude, 0.5);
        const std::size_t start = input.size() / 2;
        return 20.0 * std::log10(Rms(Render(*wah, input), start) / Rms(input, start));
    };

    const double hotCompression = gainDb(0.0, 0.5) - gainDb(1.0, 0.5);
    const double quietCompression = gainDb(0.0, 0.001) - gainDb(1.0, 0.001);
    Check(hotCompression > 3.0, "a hot signal at resonance is compressed", Num(hotCompression, 2) + " dB");
    Check(std::abs(quietCompression) < 0.1, "a quiet one is left alone", Num(quietCompression, 3) + " dB");
}

void TestFactoryPresets()
{
    std::cout << "\nFactory presets\n";
    const auto info = guitarfx::EffectRegistry::Instance().GetTypeInfo(guitarfx::EffectGuids::kWah);

    if (!info)
    {
        Check(false, "wah is registered");
        return;
    }

    Check(info->presets.size() == guitarfx::wah::kFactoryVoicings.size(), "every voicing is a registered preset",
          std::to_string(info->presets.size()) + " presets");

    std::set<std::string> ids;
    bool wellFormed = true;
    std::string problem;

    for (const auto& preset : info->presets)
    {
        if (!ids.insert(preset.id).second || preset.displayName.empty() || !preset.isFactory)
        {
            wellFormed = false;
            problem = preset.id + " is a duplicate, unnamed or not factory";
        }

        for (const auto& spec : guitarfx::wah::kParams)
        {
            const std::string key = spec.id;
            const bool performance = (key == "position" || key == "autoEngage");
            const auto found = preset.parameters.find(key);

            if (performance != (found == preset.parameters.end()))
            {
                wellFormed = false;
                problem = preset.id + (performance ? " sets " : " omits ") + key;
            }
            else if (found != preset.parameters.end() &&
                     (found->second < spec.minValue || found->second > spec.maxValue))
            {
                wellFormed = false;
                problem = preset.id + " puts " + key + " out of range";
            }
        }

        if (preset.parameters.size() != guitarfx::wah::kParamCount - 2)
        {
            wellFormed = false;
            problem = preset.id + " sets a key the wah does not declare";
        }
    }

    Check(wellFormed, "each preset is unique, in range, and sets every voicing parameter but not the pedal", problem);

    const auto isDefault = [](const guitarfx::EffectPresetDefinition& preset) { return preset.isDefault; };
    const auto defaults = std::count_if(info->presets.begin(), info->presets.end(), isDefault);
    Check(defaults == 1, "exactly one preset starts new wah nodes");

    const auto defaultPreset = std::find_if(info->presets.begin(), info->presets.end(), isDefault);
    bool matchesParamDefaults = defaultPreset != info->presets.end();

    for (const auto& [key, value] : matchesParamDefaults ? defaultPreset->parameters : std::map<std::string, double>{})
    {
        const std::size_t index = guitarfx::wah::FindParam(key);
        matchesParamDefaults = matchesParamDefaults && index != guitarfx::wah::kParamCount &&
                               guitarfx::wah::kParams[index].defaultValue == value;
    }

    Check(matchesParamDefaults, "the default preset is the parameter defaults, so a new node and a bare wah agree");

    // Loudness against pink noise, which is close to a guitar's long-term spectrum. The bounds
    // only catch a gross calibration mistake; the table is printed for voicing by ear.
    std::cout << "  pink-noise level against dry, dB (heel / mid / toe):\n";
    const auto pink = PinkNoise(1.0);
    const std::size_t start = pink.size() / 4;
    const double pinkRms = Rms(pink, start);
    bool sensible = true;

    for (const auto& preset : info->presets)
    {
        std::cout << "    " << preset.displayName << ":";

        for (const double position : {0.0, 0.5, 1.0})
        {
            Params params(preset.parameters.begin(), preset.parameters.end());
            params.emplace_back("position", position);
            auto wah = MakeWah(params);
            const double db = 20.0 * std::log10(Rms(Render(*wah, pink), start) / pinkRms);
            sensible = sensible && db > -24.0 && db < 12.0;
            std::cout << " " << Num(db, 1);
        }

        std::cout << "\n";
    }

    Check(sensible, "every preset sits within -24..+12 dB of the dry level across the sweep");
}

void TestPresetStability()
{
    std::cout << "\nStability under a fast sweep\n";
    const auto info = guitarfx::EffectRegistry::Instance().GetTypeInfo(guitarfx::EffectGuids::kWah);

    if (!info)
    {
        return;
    }

    constexpr int kStep = 32;
    bool stable = true;
    std::string problem;

    for (const double sampleRate : {44100.0, 96000.0})
    {
        for (const auto& preset : info->presets)
        {
            Params params(preset.parameters.begin(), preset.parameters.end());
            params.emplace_back("response", 1.0);
            auto wah = MakeWah(params, sampleRate);
            auto input = WhiteNoise(0.5, sampleRate);
            std::vector<float> outL(kStep), outR(kStep);

            for (std::size_t start = 0; start + kStep <= input.size() && stable; start += kStep)
            {
                // A 6 Hz heel-to-toe rock, far faster than a foot, with the smoothing at its minimum.
                const double t = static_cast<double>(start) / sampleRate;
                wah->SetParam("position", 0.5 + 0.5 * std::sin(2.0 * kPi * 6.0 * t));
                float* inputs[2] = {input.data() + start, input.data() + start};
                float* outputs[2] = {outL.data(), outR.data()};
                wah->Process(inputs, outputs, kStep);

                for (const float y : outL)
                {
                    if (!guitarfx::IsFinite(y) || std::abs(y) > 1000.0f)
                    {
                        stable = false;
                        problem = preset.id + " at " + Num(sampleRate, 0) + " Hz produced " + std::to_string(y);
                        break;
                    }
                }
            }
        }
    }

    Check(stable, "every preset stays finite and bounded on full-scale noise while sweeping", problem);
}
} // namespace

int main()
{
    std::cout << "=== WahEffectTests ===" << std::endl;
    guitarfx::RegisterWahEffect();

    TestRegistration();
    TestSweepEndpoints();
    TestQ();
    TestTaper();
    TestGainLawLevelAndMix();
    TestPinkLevel();
    TestLowEndAndTreble();
    TestPedalGlide();
    TestAutoEngage();
    TestSaturation();
    TestFactoryPresets();
    TestPresetStability();

    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}
