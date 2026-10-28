// What a model load costs: file parse, construction from cached data (what NamModelCache does on a hit,
// including get_dsp's own prewarm), and the Reset + prewarm our Prepare adds at 48 kHz / 64.
#define NOMINMAX
#include <windows.h>
#include <xmmintrin.h>
#include <pmmintrin.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>

#include "NAM/activations.h"
#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include "NAM/slimmable.h"
#include "FusedWaveNet.h"

namespace nam
{
namespace factory
{
void ForceFactoryRegistration();
}
} // namespace nam

static double Now()
{
    static LARGE_INTEGER f = [] {
        LARGE_INTEGER x;
        QueryPerformanceFrequency(&x);
        return x;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(f.QuadPart);
}

int main(int argc, char** argv)
{
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << 4);
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
    nam::factory::ForceFactoryRegistration();
    nam::activations::Activation::enable_fast_tanh();

    for (int a = 1; a < argc; ++a)
    {
        std::string path = argv[a];
        double size = 1.0;

        if (auto c = path.rfind(':'); c != std::string::npos && c > 1)
        {
            size = std::atof(path.c_str() + c + 1);
            path = path.substr(0, c);
        }

        double tParse = 1e9, tCached = 1e9, tPrepare = 1e9, tFastPrepare = 1e9, tFastBuild = 1e9;
        nlohmann::json pj;
        {
            std::ifstream pf(path);
            pj = nlohmann::json::parse(pf);

            if (pj.value("architecture", std::string()) == "SlimmableContainer")
            {
                nlohmann::json m = pj["config"]["submodels"][size < 0.5 ? 0 : 1]["model"];
                pj = std::move(m);
            }
        }
        const std::vector<float> pw = pj["weights"].get<std::vector<float>>();

        for (int r = 0; r < 5; ++r)
        {
            nam::dspData data;
            double t0 = Now();
            auto m = nam::get_dsp(std::filesystem::path(path), data);
            tParse = std::min(tParse, Now() - t0);
            nam::dspData copy = data;
            t0 = Now();
            auto m2 = nam::get_dsp(copy);
            tCached = std::min(tCached, Now() - t0);

            if (auto* sl = dynamic_cast<nam::SlimmableModel*>(m2.get()))
            {
                sl->SetSlimmableSize(size);
            }

            t0 = Now();
            m2->Reset(48000.0, 65);
            tPrepare = std::min(tPrepare, Now() - t0);
            t0 = Now();
            auto fb = fusedwn::TryCreateFromConfig(pj, pw, 48000.0);
            tFastBuild = std::min(tFastBuild, Now() - t0);
            auto f = fusedwn::TryCreate(path, size);

            if (f)
            {
                t0 = Now();
                f->Reset(48000.0, 65);
                tFastPrepare = std::min(tFastPrepare, Now() - t0);
            }
        }

        std::printf(
            "%-14s parse+build %6.1f ms | build from cache (incl. its prewarm) %6.1f ms | Reset(48k,65) prewarm: "
            "generic %5.1f ms, fused %5.1f ms | fused build from parsed %5.2f ms\n",
            path.substr(path.find_last_of("/\\") + 1).c_str(), tParse * 1e3, tCached * 1e3, tPrepare * 1e3,
            tFastPrepare * 1e3, tFastBuild * 1e3);
    }

    return 0;
}
