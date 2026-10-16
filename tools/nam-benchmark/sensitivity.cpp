// Does a model amplify float rounding? Render the generic model on x and on x*(1+eps), and compare with the
// fused model on x. If a 1e-7 nudge moves the generic output as far as the fused one sits from it, the gap
// is the model's own sensitivity, not a fused-path bug.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "NAM/activations.h"
#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include "FusedWaveNet.h"
#include <xmmintrin.h>
#include <pmmintrin.h>

namespace nam { namespace factory { void ForceFactoryRegistration(); } }

static std::vector<double> Run(nam::DSP& d, std::vector<double> in)
{
    d.ResetAndPrewarm(48000.0, 64);
    std::vector<double> out(in.size());
    for (size_t s = 0; s < in.size(); s += 64)
    {
        NAM_SAMPLE* ip[1] = {in.data() + s};
        NAM_SAMPLE* op[1] = {out.data() + s};
        d.process(ip, op, 64);
    }
    return out;
}

static double Snr(const std::vector<double>& r, const std::vector<double>& x, size_t from = 0)
{
    double e = 0, s = 0;
    for (size_t i = from; i < r.size(); ++i)
    {
        e += (x[i] - r[i]) * (x[i] - r[i]);
        s += r[i] * r[i];
    }
    return e == 0 ? 999 : 10 * std::log10(s / e);
}

int main(int argc, char** argv)
{
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
    nam::factory::ForceFactoryRegistration();
    nam::activations::Activation::enable_fast_tanh();
    std::vector<double> x(48000);
    for (size_t i = 0; i < x.size(); ++i)
    {
        const double t = i / 48000.0;
        x[i] = 0.3 * std::exp(-std::fmod(t, 0.25) * 6.0) * std::sin(2 * 3.14159265 * (110.0 + 40.0 * t) * t);
    }
    for (int a = 1; a < argc; ++a)
    {
        auto g = nam::get_dsp(std::filesystem::path(argv[a]));
        auto f = fusedwn::TryCreate(argv[a]);
        const auto ref = Run(*g, x);
        const auto fused = Run(*f, x);
        std::printf("%s\n  fused vs generic: %.1f dB\n", argv[a], Snr(ref, fused));
        for (double eps : {1e-7, 1e-6})
        {
            std::vector<double> xp = x;
            for (auto& v : xp)
                v *= 1.0 + eps;
            const auto gp = Run(*g, xp);
            std::printf("  generic vs generic with input x(1+%g): %.1f dB\n", eps, Snr(ref, gp));
        }
        // Per 0.1 s window, to see whether the gap is everywhere or in one passage.
        std::printf("  per 100 ms, fused vs generic:");
        for (size_t w = 0; w + 4800 <= x.size(); w += 4800)
        {
            std::vector<double> r(ref.begin() + w, ref.begin() + w + 4800), q(fused.begin() + w, fused.begin() + w + 4800);
            std::printf(" %.0f", Snr(r, q));
        }
        std::printf("\n");
    }
    return 0;
}
