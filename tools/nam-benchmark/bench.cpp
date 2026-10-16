// NAM core benchmark: ns per sample for each model at 48 kHz, fixed block size, models interleaved, best round kept.
//
// Usage: bench.exe [--block N] [--rounds R] [--seconds S] [--slow-tanh] [fused:]model.nam[:size] ...
//   model.nam            the model as the NAM core builds it (nam::get_dsp), the way the app runs it today
//   fused:model.nam      the same file through FusedWaveNet.h; compared with the generic one when both are listed
//   :size                slimmable size for an A2 container (0.0 = nano, 1.0 = full, the app's default)
#define NOMINMAX
#include <windows.h>
#include <xmmintrin.h>
#include <pmmintrin.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "NAM/activations.h"
#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include "NAM/slimmable.h"

#include "FusedWaveNet.h"

namespace nam { namespace factory { void ForceFactoryRegistration(); } }

static double Now()
{
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(f.QuadPart);
}

// Guitar-like test input: decaying plucked partials plus a little noise, about -18 dBFS RMS.
static std::vector<double> MakeInput(int frames, double sr)
{
    std::vector<double> x(frames);
    unsigned s = 12345;
    double notes[] = {82.4, 110.0, 146.8, 196.0, 246.9, 329.6};
    for (int i = 0; i < frames; ++i)
    {
        double t = i / sr;
        int n = int(t * 4.0) % 6;
        double tn = std::fmod(t, 0.25);
        double env = std::exp(-tn * 6.0);
        double v = 0.0;
        for (int h = 1; h <= 6; ++h)
            v += std::sin(2.0 * 3.14159265358979 * notes[n] * h * t) / h;
        s = s * 1664525u + 1013904223u;
        double noise = (double(s >> 8) / double(1 << 24) - 0.5) * 0.002;
        x[i] = 0.18 * env * v + noise;
    }
    return x;
}

struct Item
{
    std::string label;
    std::unique_ptr<nam::DSP> dsp;
    std::vector<double> out;
    double best = 1e30;
    double worstBlock = 0.0;
};

int main(int argc, char** argv)
{
    int block = 64;
    int rounds = 7;
    double seconds = 1.0;
    bool fastTanh = true;
    std::vector<std::string> specs;
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--block") block = std::atoi(argv[++i]);
        else if (a == "--rounds") rounds = std::atoi(argv[++i]);
        else if (a == "--seconds") seconds = std::atof(argv[++i]);
        else if (a == "--slow-tanh") fastTanh = false;
        else specs.push_back(a);
    }

    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << 4);
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);

    nam::factory::ForceFactoryRegistration();
    if (fastTanh)
        nam::activations::Activation::enable_fast_tanh();

    const double sr = 48000.0;
    const int frames = int(seconds * sr) / block * block;
    std::vector<double> input = MakeInput(frames, sr);

    std::vector<Item> items;
    for (const auto& spec : specs)
    {
        std::string path = spec;
        double size = -1.0;
        bool fused = false;
        if (path.rfind("fused:", 0) == 0) { fused = true; path = path.substr(6); }
        auto colon = path.rfind(':');
        if (colon != std::string::npos && colon > 1)
        {
            size = std::atof(path.c_str() + colon + 1);
            path = path.substr(0, colon);
        }
        Item it;
        it.label = spec;
        if (fused)
            it.dsp = fusedwn::TryCreate(path, size >= 0.0 ? size : 1.0);
        else
            it.dsp = nam::get_dsp(std::filesystem::path(path));
        if (!it.dsp) { std::printf("failed: %s\n", spec.c_str()); return 1; }
        if (size >= 0.0)
            if (auto* sl = dynamic_cast<nam::SlimmableModel*>(it.dsp.get()))
                sl->SetSlimmableSize(size);
        it.dsp->ResetAndPrewarm(sr, block);
        it.out.assign(frames, 0.0);
        items.push_back(std::move(it));
    }

    for (int r = 0; r < rounds; ++r)
    {
        for (auto& it : items)
        {
            double worst = 0.0;
            const double t0 = Now();
            for (int b = 0; b < frames; b += block)
            {
                NAM_SAMPLE* in[1] = {input.data() + b};
                NAM_SAMPLE* out[1] = {it.out.data() + b};
                const double tb = Now();
                it.dsp->process(in, out, block);
                worst = std::max(worst, Now() - tb);
            }
            const double t = Now() - t0;
            it.best = std::min(it.best, t / frames * 1e9);
            if (r > 0) it.worstBlock = std::max(it.worstBlock, worst * 1e6);
        }
    }

    for (auto& it : items)
    {
        double sum = 0.0, peak = 0.0;
        for (double v : it.out) { sum += v * v; peak = std::max(peak, std::abs(v)); }
        std::string name = it.label;
        if (auto slash = name.find_last_of("/\\"); slash != std::string::npos)
            name = (name.rfind("fused:", 0) == 0 ? "fused:" : "") + name.substr(slash + 1);
        std::printf("%-22s %8.1f ns/sample  %6.2f us/block(%d)  worst block %7.1f us  out rms %.4f peak %.3f",
                    name.c_str(), it.best, it.best * block / 1000.0, block, it.worstBlock,
                    std::sqrt(sum / it.out.size()), peak);
        // Against the generic model of the same file, when it ran too.
        if (it.label.rfind("fused:", 0) == 0)
        {
            const std::string base = it.label.substr(6);
            for (auto& ref : items)
                if (ref.label == base)
                {
                    double err = 0.0, sig = 0.0, maxErr = 0.0;
                    for (size_t i = 0; i < it.out.size(); ++i)
                    {
                        const double e = it.out[i] - ref.out[i];
                        err += e * e;
                        sig += ref.out[i] * ref.out[i];
                        maxErr = std::max(maxErr, std::abs(e));
                    }
                    std::printf("  | vs generic: %.1fx faster, SNR %.1f dB, max err %.2e", ref.best / it.best,
                                10.0 * std::log10(sig / std::max(err, 1e-300)), maxErr);
                }
        }
        std::printf("\n");
    }
    return 0;
}
