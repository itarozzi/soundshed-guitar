/**
 * @file VerbalTests.cpp
 * @brief Verbal, the multi-machine reverb (core/src/dsp/reverb/).
 *
 *   - every machine runs finite, stays silent on silence and dies away; the tank machines' two
 *     sides are uncorrelated from a mono input
 *   - Decay is the measured RT60: Hall and Room within 12% of the knob from 1 to 6 s
 *   - the Mix law keeps the dry at unity and adds the wet, with a fully wet top
 *   - Infinite holds the tail and keeps adding to it; Freeze holds it and takes nothing new;
 *     releasing either lets it decay
 *   - a machine change lets the old tail ring out, without a click
 *   - a non-finite input sample is scrubbed rather than latched
 *   - every machine-only control says which machines show it
 */

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/reverb/VerbalEffect.h"

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 128;

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

std::string Num(double v)
{
    return std::to_string(std::round(v * 1000.0) / 1000.0).substr(0, 7);
}

/// A rendered stereo output, with a callback per block for automation.
struct Render
{
    std::vector<float> l;
    std::vector<float> r;

    [[nodiscard]] double Energy(double fromS, double toS) const
    {
        const auto a = static_cast<std::size_t>(fromS * kSampleRate);
        const auto b = std::min(l.size(), static_cast<std::size_t>(toS * kSampleRate));
        double e = 0.0;

        for (std::size_t i = a; i < b; ++i)
        {
            e += 0.5 * (static_cast<double>(l[i]) * l[i] + static_cast<double>(r[i]) * r[i]);
        }

        return e / std::max<std::size_t>(1, b - a);
    }

    [[nodiscard]] bool Finite() const
    {
        return std::all_of(l.begin(), l.end(), [](float v) { return guitarfx::IsFinite(v); }) &&
               std::all_of(r.begin(), r.end(), [](float v) { return guitarfx::IsFinite(v); });
    }
};

template <typename Input, typename Automation>
Render Run(guitarfx::VerbalEffect& fx, double seconds, Input input, Automation automate)
{
    const auto total = static_cast<std::size_t>(seconds * kSampleRate);
    std::vector<float> inL(kBlockSize), inR(kBlockSize), outL(kBlockSize), outR(kBlockSize);
    float* ins[2] = {inL.data(), inR.data()};
    float* outs[2] = {outL.data(), outR.data()};
    Render out;
    out.l.reserve(total);
    out.r.reserve(total);

    for (std::size_t start = 0; start < total; start += kBlockSize)
    {
        automate(static_cast<double>(start) / kSampleRate);

        for (int i = 0; i < kBlockSize; ++i)
        {
            const float v = input(start + static_cast<std::size_t>(i));
            inL[i] = v;
            inR[i] = v;
        }

        fx.Process(ins, outs, kBlockSize);
        out.l.insert(out.l.end(), outL.begin(), outL.end());
        out.r.insert(out.r.end(), outR.begin(), outR.end());
    }

    return out;
}

/// A 50 ms noise burst at -12 dBFS rms from `at` seconds, on every call the same.
auto Burst(double at = 0.0, double length = 0.05)
{
    return [at, length](std::size_t n) {
        const auto start = static_cast<std::size_t>(at * kSampleRate);
        const auto end = start + static_cast<std::size_t>(length * kSampleRate);

        if (n < start || n >= end)
        {
            return 0.0f;
        }

        std::uint32_t seed = static_cast<std::uint32_t>(n) * 2654435761u + 12345u;
        seed ^= seed >> 13;
        seed *= 1274126177u;
        return 0.25f * 1.7320508f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
    };
}

void NoAutomation(double)
{
}

std::unique_ptr<guitarfx::VerbalEffect> Make(const std::map<std::string, double>& params)
{
    auto fx = std::make_unique<guitarfx::VerbalEffect>();
    fx->SetParam("mix", 1.0);

    for (const auto& [key, value] : params)
    {
        fx->SetParam(key, value);
    }

    fx->Prepare(kSampleRate, kBlockSize);
    return fx;
}

/// Schroeder T30 from an impulse response, both channels.
double MeasureT30(const Render& out)
{
    std::vector<double> edc(out.l.size());
    double acc = 0.0;

    for (std::size_t i = out.l.size(); i-- > 0;)
    {
        acc += static_cast<double>(out.l[i]) * out.l[i] + static_cast<double>(out.r[i]) * out.r[i];
        edc[i] = acc;
    }

    const double total = edc[0];
    const auto at = [&](double db) {
        const double target = total * std::pow(10.0, db / 10.0);

        for (std::size_t i = 0; i < edc.size(); ++i)
        {
            if (edc[i] <= target)
            {
                return static_cast<double>(i) / kSampleRate;
            }
        }

        return static_cast<double>(edc.size()) / kSampleRate;
    };

    return 2.0 * (at(-35.0) - at(-5.0));
}

void TestEveryMachine()
{
    std::cout << "\nEvery machine: finite, silent on silence, dies away" << std::endl;

    for (int machine = 0; machine < static_cast<int>(guitarfx::verbal::kMachineCount); ++machine)
    {
        const std::string name = guitarfx::verbal::kMachineLabels[machine];
        auto fx = Make({{"machine", machine}, {"decay", 1.0}, {"echoFeedback", 0.3}});
        const auto silent = Run(*fx, 0.3, [](std::size_t) { return 0.0f; }, NoAutomation);
        Check(silent.Energy(0.0, 0.3) == 0.0, name + ": silence in, silence out");

        fx->Reset();
        const auto out = Run(*fx, 3.0, Burst(), NoAutomation);
        const double early = out.Energy(0.05, 0.4);
        const double late = out.Energy(2.5, 3.0);
        Check(out.Finite() && early > 1e-7, name + ": answers a burst", "early " + Num(10 * std::log10(early)) + " dB");
        Check(late < early * 1e-3, name + ": dies away", Num(10 * std::log10(late / early)) + " dB");

        // The tank machines' two sides: uncorrelated even though both inputs are the same.
        if (machine != static_cast<int>(guitarfx::verbal::Machine::Reflections))
        {
            double lr = 0.0, ll = 0.0, rr = 0.0;

            for (std::size_t i = static_cast<std::size_t>(0.15 * kSampleRate); i < out.l.size(); ++i)
            {
                lr += static_cast<double>(out.l[i]) * out.r[i];
                ll += static_cast<double>(out.l[i]) * out.l[i];
                rr += static_cast<double>(out.r[i]) * out.r[i];
            }

            const double corr = lr / std::sqrt(ll * rr + 1e-30);
            Check(std::abs(corr) < 0.5, name + ": the sides are uncorrelated", "corr " + Num(corr));
        }
    }
}

void TestDecayIsRt60()
{
    std::cout << "\nDecay reads as the measured RT60" << std::endl;

    for (const auto& [machine, label] : {std::pair{1, "Hall"}, std::pair{0, "Room"}})
    {
        for (const double decay : {1.0, 3.0, 6.0})
        {
            auto fx = Make({{"machine", machine}, {"decay", decay}, {"preDelay", 1.0}, {"tone", 0.6}, {"lowEnd", 0.5}});
            const auto out =
                Run(*fx, decay * 1.6 + 1.0, [](std::size_t n) { return n == 0 ? 0.5f : 0.0f; }, NoAutomation);
            const double t30 = MeasureT30(out);
            Check(std::abs(t30 / decay - 1.0) < 0.12, std::string(label) + " Decay " + Num(decay) + " s",
                  "T30 " + Num(t30) + " s");
        }
    }
}

void TestMixLaw()
{
    std::cout << "\nMix adds the reverb to an untouched dry" << std::endl;
    using guitarfx::VerbalEffect;
    Check(VerbalEffect::WetGain(0.0f) == 0.0f && VerbalEffect::DryGain(0.0f) == 1.0f, "Mix 0 is dry only");
    Check(std::abs(20.0f * std::log10(VerbalEffect::WetGain(0.5f)) + 10.0f) < 0.1f &&
              VerbalEffect::DryGain(0.5f) == 1.0f,
          "half way: the dry at unity, the wet 10 dB down");
    Check(VerbalEffect::WetGain(0.9f) == 1.0f && VerbalEffect::DryGain(0.9f) == 1.0f, "nine tenths: both at unity");
    Check(VerbalEffect::WetGain(1.0f) == 1.0f && VerbalEffect::DryGain(1.0f) == 0.0f, "the top is fully wet");

    bool rising = true;

    for (float m = 0.0f; m < 1.0f; m += 0.01f)
    {
        rising = rising && VerbalEffect::WetGain(m + 0.01f) >= VerbalEffect::WetGain(m);
    }

    Check(rising, "the wet never falls as Mix rises");
}

void TestHold()
{
    std::cout << "\nHold" << std::endl;

    // Infinite: engaged at 0.3 s, a second burst at 1.5 s joins the tail, released at 3.5 s.
    {
        auto fx = Make({{"machine", 1}, {"decay", 1.0}, {"holdMode", 0}});
        auto input = [](std::size_t n) { return Burst(0.0)(n) + Burst(1.5)(n); };
        auto automate = [&fx](double t) { fx->SetParam("hold", t >= 0.3 && t < 3.5 ? 1.0 : 0.0); };
        const auto out = Run(*fx, 6.0, input, automate);
        const double held = out.Energy(1.0, 1.4);
        const double later = out.Energy(3.0, 3.4);
        const double grown = out.Energy(1.8, 2.2);
        Check(out.Finite() && later > held * 0.3, "Infinite holds the tail",
              Num(10 * std::log10(later / held)) + " dB over two seconds");
        Check(grown > held * 1.5, "Infinite adds what is played into it", Num(10 * std::log10(grown / held)) + " dB");
        Check(out.Energy(5.5, 6.0) < later * 1e-2, "released, it decays again");
    }

    // Freeze: engaged at 0.3 s; a louder burst at 1.5 s must not join the tail.
    {
        auto fx = Make({{"machine", 1}, {"decay", 1.0}, {"holdMode", 1}});
        auto input = [](std::size_t n) { return Burst(0.0)(n) + 2.0f * Burst(1.5)(n); };
        auto automate = [&fx](double t) { fx->SetParam("hold", t >= 0.3 ? 1.0 : 0.0); };
        const auto out = Run(*fx, 3.0, input, automate);
        const double before = out.Energy(1.0, 1.4);
        const double after = out.Energy(2.0, 2.4);
        Check(before > 1e-8 && std::abs(10 * std::log10(after / before)) < 2.0, "Freeze holds and takes nothing new",
              Num(10 * std::log10(after / before)) + " dB across the burst");
    }
}

void TestMachineChangeSpillsOver()
{
    std::cout << "\nChanging machine" << std::endl;
    auto fx = Make({{"machine", 1}, {"decay", 4.0}});
    auto automate = [&fx](double t) { fx->SetParam("machine", t >= 0.8 ? 0.0 : 1.0); };
    const auto out = Run(*fx, 1.6, Burst(), automate);
    const auto change = static_cast<std::size_t>(0.8 * kSampleRate);
    double worstStep = 0.0, typicalStep = 0.0;

    for (std::size_t i = change - 4800; i < change + 4800; ++i)
    {
        const double step = std::abs(static_cast<double>(out.l[i]) - out.l[i - 1]);
        (i < change ? typicalStep : worstStep) = std::max(i < change ? typicalStep : worstStep, step);
    }

    const double before = out.Energy(0.7, 0.8);
    const double after = out.Energy(0.8, 0.9);
    Check(after > before * 0.3, "the old tail rings on", Num(10 * std::log10(after / before)) + " dB");
    Check(worstStep < typicalStep * 2.5, "without a click", "worst step x" + Num(worstStep / (typicalStep + 1e-12)));
}

void TestNonFiniteInput()
{
    std::cout << "\nA non-finite input sample" << std::endl;
    auto fx = Make({{"machine", 6}, {"decay", 2.0}});
    const auto out = Run(
        *fx, 1.0,
        [](std::size_t n) {
            // From its bits: under fast math a NaN constant can be folded away.
            return n == 1000 ? std::bit_cast<float>(0x7fc00000u) : Burst(0.1)(n);
        },
        NoAutomation);
    Check(out.Finite() && out.Energy(0.3, 0.6) > 1e-8,
          "is scrubbed: the output stays finite and the reverb keeps working");
}

void TestVisibilityMetadata()
{
    std::cout << "\nControls each machine shows" << std::endl;
    guitarfx::RegisterVerbalEffect();
    const auto info = guitarfx::EffectRegistry::Instance().GetTypeInfo(guitarfx::EffectGuids::kReverbVerbal);
    Check(info.has_value(), "registered");

    if (!info)
    {
        return;
    }

    bool ok = true;
    std::string problem;

    for (const auto& def : info->parameters)
    {
        if (def.group == "Machine")
        {
            const bool valid = def.visibleWhenKey == "machine" && !def.visibleWhenValues.empty() &&
                               std::all_of(def.visibleWhenValues.begin(), def.visibleWhenValues.end(),
                                           [](int m) { return m >= 0 && m < 12; });
            ok = ok && valid;
            problem = valid ? problem : def.id;
        }
    }

    Check(ok, "every machine-only control names its machines", problem);
    const auto& machine = info->parameters[guitarfx::verbal::kMachine];
    Check(machine.control == "picker" && machine.labels.size() == 12 && machine.labelInfo.size() == 12,
          "Machine is a picker of twelve, each with a line of detail");
}
} // namespace

int main()
{
    std::cout << "=== VerbalTests ===" << std::endl;
    TestEveryMachine();
    TestDecayIsRt60();
    TestMixLaw();
    TestHold();
    TestMachineChangeSpillsOver();
    TestNonFiniteInput();
    TestVisibilityMetadata();
    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}
