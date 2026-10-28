// Block-size invariance and accuracy of the fused WaveNet against the generic one.
// The same 2 s input is rendered with several host block patterns; the fused model's output must be
// bit-identical across patterns, and close to the generic model's.
#define NOMINMAX
#include <windows.h>
#include <xmmintrin.h>
#include <pmmintrin.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

static std::vector<double> MakeInput(int frames, double sr)
{
    std::vector<double> x(frames);
    unsigned s = 999;

    for (int i = 0; i < frames; ++i)
    {
        const double t = i / sr;
        const double env = std::exp(-std::fmod(t, 0.5) * 5.0);
        s = s * 1664525u + 1013904223u;
        x[i] = 0.3 * env * std::sin(2 * 3.14159265 * 110.0 * t) + 0.25 * env * std::sin(2 * 3.14159265 * 331.0 * t) +
               (double(s >> 8) / double(1 << 24) - 0.5) * 0.01;
    }

    return x;
}

static std::vector<double> Render(nam::DSP& dsp, const std::vector<double>& in, const std::vector<int>& pattern,
                                  int maxBuf)
{
    dsp.ResetAndPrewarm(48000.0, maxBuf);
    std::vector<double> out(in.size(), 0.0);
    size_t pos = 0, pi = 0;
    std::vector<double> inCopy = in;

    while (pos < in.size())
    {
        const int n = int(std::min<size_t>(pattern[pi++ % pattern.size()], in.size() - pos));
        NAM_SAMPLE* ip[1] = {inCopy.data() + pos};
        NAM_SAMPLE* op[1] = {out.data() + pos};
        dsp.process(ip, op, n);
        pos += n;
    }

    return out;
}

static double Snr(const std::vector<double>& ref, const std::vector<double>& x)
{
    double e = 0, s = 0;

    for (size_t i = 0; i < ref.size(); ++i)
    {
        e += (x[i] - ref[i]) * (x[i] - ref[i]);
        s += ref[i] * ref[i];
    }

    return e == 0 ? 999.0 : 10 * std::log10(s / e);
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
    nam::factory::ForceFactoryRegistration();
    nam::activations::Activation::enable_fast_tanh();
    // --sweep: coverage.cpp's input (a low swept tone), where some models amplify float rounding most.
    bool sweep = false;

    for (int a = 1; a < argc; ++a)
    {
        sweep = sweep || std::string(argv[a]) == "--sweep";
    }

    auto input = MakeInput(96000, 48000.0);

    if (sweep)
    {
        for (size_t i = 0; i < input.size(); ++i)
        {
            const double t = i / 48000.0;
            input[i] = 0.3 * std::exp(-std::fmod(t, 0.25) * 6.0) * std::sin(2 * 3.14159265 * (110.0 + 40.0 * t) * t);
        }
    }

    const std::vector<std::vector<int>> patterns = {{64}, {47}, {1}, {17, 256, 3, 100}, {512}, {13, 7}};
    const char* names[] = {"64", "47", "1", "17/256/3/100", "512 (over max, chunks)", "13/7"};
    int failures = 0;
    int scale = 1;

    for (int a = 1; a < argc; ++a)
    {
        if (std::string(argv[a]) == "--scale")
        {
            scale = std::atoi(argv[++a]);
            continue;
        }

        if (std::string(argv[a]) == "--sweep")
        {
            continue;
        }

        std::string path = argv[a];
        double size = 1.0;

        if (auto c = path.rfind(':'); c != std::string::npos && c > 1)
        {
            size = std::atof(path.c_str() + c + 1);
            path = path.substr(0, c);
        }

        auto fast = fusedwn::TryCreate(path, size);
        auto generic = nam::get_dsp(std::filesystem::path(path));

        if (auto* sl = dynamic_cast<nam::SlimmableModel*>(generic.get()))
        {
            sl->SetSlimmableSize(size);
        }

        if (fast && scale > 1)
        {
            fast->SetTimeScale(scale);
            generic->SetTimeScale(scale);
            std::printf("time scale %d\n", scale);
        }

        if (!fast)
        {
            std::printf("%s: not a fused shape\n", argv[a]);
            continue;
        }

        const auto ref = Render(*fast, input, patterns[0], 256);
        const auto gref = Render(*generic, input, patterns[0], 256);
        unsigned long long h = 1469598103934665603ull;

        for (double v : ref)
        {
            unsigned long long bits;
            std::memcpy(&bits, &v, 8);
            h = (h ^ bits) * 1099511628211ull;
        }

        // The first samples come straight out of the prewarmed state, so compare them with the generic model's too.
        double early = 0, earlySig = 0;

        for (int i = 0; i < 64; ++i)
        {
            early += (ref[i] - gref[i]) * (ref[i] - gref[i]);
            earlySig += gref[i] * gref[i];
        }

        std::printf(
            "%s\n  fused vs generic @64: SNR %.1f dB (first block %.1f dB, first sample %.3e vs %.3e)  fused hash %016llx\n",
            argv[a], Snr(gref, ref), early == 0 ? 999.0 : 10 * std::log10(earlySig / early), ref[0], gref[0], h);

        for (size_t p = 1; p < patterns.size(); ++p)
        {
            std::printf("  [pattern %s]\n", names[p]);
            const auto out = Render(*fast, input, patterns[p], 256);
            // The generic WaveNet overruns its buffers past the Reset() size (asserts are off in Release).
            const bool overMax = patterns[p][0] > 256;
            const auto gout = overMax ? gref : Render(*generic, input, patterns[p], 256);
            const bool same = std::memcmp(out.data(), ref.data(), out.size() * sizeof(double)) == 0;
            const bool gsame = std::memcmp(gout.data(), gref.data(), gout.size() * sizeof(double)) == 0;
            failures += same ? 0 : 1;
            std::printf("  blocks %-24s fused %s (SNR vs 64: %.1f)   generic %s (SNR vs 64: %.1f)\n", names[p],
                        same ? "bit-identical" : "DIFFERS", Snr(ref, out), gsame ? "bit-identical" : "differs",
                        Snr(gref, gout));
        }
    }

    std::printf(failures ? "FAIL: %d fused renders depend on the block size\n" : "OK\n", failures);
    return failures ? 1 : 0;
}
