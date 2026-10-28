// Cost of each step of one generic WaveNet layer (nam::wavenet::detail::Layer::Process), per block.
#define NOMINMAX
#include <windows.h>
#include <xmmintrin.h>
#include <pmmintrin.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <vector>

#include <Eigen/Dense>

#include "NAM/activations.h"
#include "NAM/conv1d.h"
#include "NAM/dsp.h"

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

static double TimeIt(const std::function<void()>& fn, int iters = 20000, int rounds = 7)
{
    double best = 1e30;

    for (int r = 0; r < rounds; ++r)
    {
        const double t0 = Now();

        for (int i = 0; i < iters; ++i)
        {
            fn();
        }

        best = std::min(best, (Now() - t0) / iters);
    }

    return best * 1e9; // ns per call
}

static void FillWeights(std::vector<float>& w, size_t n, std::mt19937& rng)
{
    std::uniform_real_distribution<float> d(-0.3f, 0.3f);
    w.resize(n);

    for (auto& v : w)
    {
        v = d(rng);
    }
}

static void Layer(int C, int K, int dil, int N)
{
    std::mt19937 rng(1);
    std::vector<float> w;

    nam::Conv1D conv(C, C, K, 1, dil, 1);
    FillWeights(w, size_t(C) * C * K + C, rng);
    auto it = w.begin();
    conv.set_weights_(it);
    conv.SetMaxBufferSize(N);

    nam::Conv1x1 mixin(1, C, false, 1);
    FillWeights(w, size_t(C), rng);
    it = w.begin();
    mixin.set_weights_(it);
    mixin.SetMaxBufferSize(N);

    nam::Conv1x1 l1x1(C, C, true, 1);
    FillWeights(w, size_t(C) * C + C, rng);
    it = w.begin();
    l1x1.set_weights_(it);
    l1x1.SetMaxBufferSize(N);

    auto act = nam::activations::Activation::get_activation(std::string("Tanh"));

    Eigen::MatrixXf input = Eigen::MatrixXf::Random(C, N) * 0.5f;
    Eigen::MatrixXf cond = Eigen::MatrixXf::Random(1, N) * 0.5f;
    Eigen::MatrixXf z(C, N), head(C, N), next(C, N);
    head.setZero();

    const double tConv = TimeIt([&] { conv.Process(input, N); });
    const double tMixin = TimeIt([&] { mixin.process_(cond, N); });
    const double tSum =
        TimeIt([&] { z.leftCols(N).noalias() = conv.GetOutput().leftCols(N) + mixin.GetOutput().leftCols(N); });
    Eigen::MatrixXf zsave = z;
    const double tAct = TimeIt([&] {
                            z = zsave;
                            act->apply(z.leftCols(N));
                        }) -
                        TimeIt([&] { z = zsave; });
    const double tL1 = TimeIt([&] { l1x1.process_(z, N); });
    const double tHead = TimeIt([&] { head.leftCols(N).noalias() += z.leftCols(N); });
    const double tRes = TimeIt([&] { next.leftCols(N).noalias() = input.leftCols(N) + l1x1.GetOutput().leftCols(N); });
    const double total = tConv + tMixin + tSum + tAct + tL1 + tHead + tRes;
    const double macs = double(C) * C * K + C + double(C) * C;
    std::printf(
        "C=%2d K=%2d N=%3d | conv %6.0f  mixin %5.0f  sum %4.0f  tanh %5.0f  l1x1 %5.0f  head+= %4.0f  resid %4.0f | "
        "total %6.0f ns/block = %5.1f ns/frame, %4.1f GMAC/s (conv alone %4.1f)\n",
        C, K, N, tConv, tMixin, tSum, tAct, tL1, tHead, tRes, total, total / N, macs * N / total,
        double(C) * C * K * N / tConv);
}

int main()
{
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << 4);
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
    nam::activations::Activation::enable_fast_tanh();

    for (int N : {64, 256})
    {
        Layer(16, 3, 8, N);
        Layer(8, 3, 8, N);
        Layer(12, 3, 8, N);
        Layer(6, 3, 8, N);
        Layer(4, 3, 8, N);
        Layer(8, 6, 9, N);
        Layer(32, 3, 8, N);
    }

    return 0;
}
