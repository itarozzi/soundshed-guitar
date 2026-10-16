// Every model in a list: does the fused path take it, and does it agree with the generic model?
// Usage: coverage.exe list.txt   (one .nam path per line, UTF-8)
#define NOMINMAX
#include <windows.h>
#include <xmmintrin.h>
#include <pmmintrin.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "NAM/activations.h"
#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include "NAM/slimmable.h"
#include "FusedWaveNet.h"

namespace nam { namespace factory { void ForceFactoryRegistration(); } }

static std::wstring Widen(const std::string& s)
{
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
    return w;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
    nam::factory::ForceFactoryRegistration();
    nam::activations::Activation::enable_fast_tanh();

    std::vector<double> input(24000);
    for (size_t i = 0; i < input.size(); ++i)
    {
        const double t = i / 48000.0;
        input[i] = 0.3 * std::exp(-std::fmod(t, 0.25) * 6.0) * std::sin(2 * 3.14159265 * (110.0 + 40.0 * t) * t);
    }

    std::ifstream list(argv[1]);
    std::string line;
    int total = 0, fused = 0, generic = 0, failed = 0, bad = 0;
    double worst = 1e9;
    std::string worstName;
    std::map<std::string, int> rejected;
    while (std::getline(list, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        ++total;
        try
        {
            const std::filesystem::path p(Widen(line));
            std::ifstream f(p);
            nlohmann::json j = nlohmann::json::parse(f);
            const std::string arch = j.value("architecture", std::string("?"));
            const bool container = arch == "SlimmableContainer";
            for (double size : container ? std::vector<double>{0.0, 1.0} : std::vector<double>{1.0})
            {
                nlohmann::json m = j;
                if (container)
                {
                    const auto& subs = j["config"]["submodels"];
                    size_t pick = subs.size() - 1;
                    for (size_t i = 0; i < subs.size(); ++i)
                        if (size < subs[i]["max_value"].get<double>())
                        {
                            pick = i;
                            break;
                        }
                    nlohmann::json sub = subs[pick]["model"];
                    m = std::move(sub);
                }
                std::unique_ptr<nam::DSP> fm;
                if (m.contains("weights"))
                    fm = fusedwn::TryCreateFromConfig(m, m["weights"].get<std::vector<float>>(), 48000.0);
                if (!fm)
                {
                    ++generic;
                    std::string key = m.value("architecture", std::string("?"));
                    if (key == "WaveNet")
                    {
                        const auto& l0 = m["config"]["layers"][0];
                        key += " act=" + l0["activation"].dump().substr(0, 40) + " gated=" +
                               (l0.contains("gating_mode") ? l0["gating_mode"].dump().substr(0, 20)
                                                           : std::to_string(l0.value("gated", false)));
                    }
                    rejected[key]++;
                    continue;
                }
                auto gm = nam::get_dsp(p);
                if (auto* sl = dynamic_cast<nam::SlimmableModel*>(gm.get()))
                    sl->SetSlimmableSize(size);
                fm->ResetAndPrewarm(48000.0, 64);
                gm->ResetAndPrewarm(48000.0, 64);
                std::vector<double> a(input.size()), b(input.size()), in = input;
                for (size_t s = 0; s < in.size(); s += 64)
                {
                    NAM_SAMPLE* ip[1] = {in.data() + s};
                    NAM_SAMPLE* ao[1] = {a.data() + s};
                    NAM_SAMPLE* bo[1] = {b.data() + s};
                    fm->process(ip, ao, 64);
                    gm->process(ip, bo, 64);
                }
                double e = 0, sig = 0;
                for (size_t s = 0; s < a.size(); ++s)
                {
                    e += (a[s] - b[s]) * (a[s] - b[s]);
                    sig += b[s] * b[s];
                }
                const double snr = e == 0 ? 999.0 : 10 * std::log10(sig / e);
                ++fused;
                if (snr < worst)
                {
                    worst = snr;
                    worstName = line + (container ? (size < 0.5 ? " [nano]" : " [full]") : "");
                }
                if (snr < 90.0)
                {
                    ++bad;
                    double peak = 0, maxErr = 0;
                    for (size_t s = 0; s < a.size(); ++s)
                    {
                        peak = std::max(peak, std::abs(b[s]));
                        maxErr = std::max(maxErr, std::abs(a[s] - b[s]));
                    }
                    std::printf("LOW SNR %.1f dB (output rms %.2e, peak %.2e, max error %.2e): %s\n", snr,
                                std::sqrt(sig / a.size()), peak, maxErr, line.c_str());
                }
            }
        }
        catch (const std::exception& ex)
        {
            ++failed;
            std::printf("error (%s): %s\n", ex.what(), line.c_str());
        }
    }
    std::printf("\nfiles %d | models fused %d, left to generic %d, unreadable %d | below 90 dB: %d | worst %.1f dB (%s)\n",
                total, fused, generic, failed, bad, worst, worstName.c_str());
    for (auto& [k, n] : rejected)
        std::printf("  generic: %4d  %s\n", n, k.c_str());
    return 0;
}
