#pragma once
// Prototype from docs/nam-dsp-performance-review-2026-10-16.md: a fused, frame-vectorised forward
// pass for the WaveNet shapes NAM models actually ship in. It is not in the app; the review's
// recommendation 1 is to move it into core/src/dsp.
//
// Takes any WaveNet whose layers are non-gated, Tanh or LeakyReLU, with layer1x1 on and no head1x1,
// FiLM, grouping, condition DSP or post-stack head; any number of layer arrays; any channel count,
// kernel size and dilation; a head of any kernel size. That is the trainer's standard, lite, feather
// and nano presets, the xSTD and 32-channel variants in shared model libraries, and both A2 submodels.
// Anything else returns nullptr and stays on the NAM core.
//
// Layout: every activation buffer is channel-major, one contiguous time row per channel, so the SIMD
// lanes run across 8 frames and any channel count fills them. Blocks are padded to whole 8-frame
// lanes; the padded lanes compute on stale history and are never output, so every real frame takes
// the same arithmetic whatever the host block size (invariance.cpp checks it bit for bit).
//
// Switches, for experiments: FWN_OT / FWN_FT (register tile: outputs x 8-frame vectors, 4 x 2),
// FWN_CHUNK (internal chunk, 128), FWN_REAL_PREWARM (run PrewarmSamples() frames instead of the
// one-pass fill), FWN_NO_ACT (timing only: skip the activation), FWN_RCP_DIV (AVX2 reciprocal in
// place of the divide), FWN_SCALAR (no intrinsics). AVX2 and two-half SSE vectors are written; a NEON
// pair of float32x4_t is not yet.

#ifndef FWN_CHUNK
#define FWN_CHUNK 128
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include "json.hpp"

#if defined(__AVX2__)
#include <immintrin.h>
#endif

#if defined(_MSC_VER)
#define FWN_INLINE __forceinline
#else
#define FWN_INLINE inline __attribute__((always_inline))
#endif

namespace fusedwn
{
// ---------------------------------------------------------------- 8-lane float vector
#if defined(__AVX2__)
struct V8
{
    __m256 v;
};
FWN_INLINE V8 Load(const float* p) { return {_mm256_loadu_ps(p)}; }
FWN_INLINE void Store(float* p, V8 a) { _mm256_storeu_ps(p, a.v); }
FWN_INLINE V8 Set1(float x) { return {_mm256_set1_ps(x)}; }
FWN_INLINE V8 Bcast(const float* p) { return {_mm256_broadcast_ss(p)}; }
FWN_INLINE V8 Fma(V8 a, V8 b, V8 c) { return {_mm256_fmadd_ps(a.v, b.v, c.v)}; } // a*b+c
FWN_INLINE V8 Add(V8 a, V8 b) { return {_mm256_add_ps(a.v, b.v)}; }
FWN_INLINE V8 Mul(V8 a, V8 b) { return {_mm256_mul_ps(a.v, b.v)}; }
#if defined(FWN_RCP_DIV)
// Reciprocal estimate plus one Newton step: about 23 bits, and no divider.
FWN_INLINE V8 Div(V8 a, V8 b)
{
    const __m256 r = _mm256_rcp_ps(b.v);
    const __m256 r1 = _mm256_mul_ps(r, _mm256_fnmadd_ps(b.v, r, _mm256_set1_ps(2.0f)));
    return {_mm256_mul_ps(a.v, r1)};
}
#else
FWN_INLINE V8 Div(V8 a, V8 b) { return {_mm256_div_ps(a.v, b.v)}; }
#endif
FWN_INLINE V8 Abs(V8 a) { return {_mm256_andnot_ps(_mm256_set1_ps(-0.0f), a.v)}; }
FWN_INLINE V8 MaxV(V8 a, V8 b) { return {_mm256_max_ps(a.v, b.v)}; }
#elif (defined(_M_X64) || defined(__SSE2__)) && !defined(FWN_SCALAR)
// Two 128-bit halves: the shape a NEON build (float32x4_t pairs, with vfmaq) would take.
#define FWN_SSE 1
}
#include <emmintrin.h>
namespace fusedwn
{
struct V8
{
    __m128 lo, hi;
};
FWN_INLINE V8 Load(const float* p) { return {_mm_loadu_ps(p), _mm_loadu_ps(p + 4)}; }
FWN_INLINE void Store(float* p, V8 a) { _mm_storeu_ps(p, a.lo); _mm_storeu_ps(p + 4, a.hi); }
FWN_INLINE V8 Set1(float x) { const __m128 s = _mm_set1_ps(x); return {s, s}; }
FWN_INLINE V8 Bcast(const float* p) { const __m128 s = _mm_load1_ps(p); return {s, s}; }
FWN_INLINE V8 Fma(V8 a, V8 b, V8 c) { return {_mm_add_ps(_mm_mul_ps(a.lo, b.lo), c.lo), _mm_add_ps(_mm_mul_ps(a.hi, b.hi), c.hi)}; }
FWN_INLINE V8 Add(V8 a, V8 b) { return {_mm_add_ps(a.lo, b.lo), _mm_add_ps(a.hi, b.hi)}; }
FWN_INLINE V8 Mul(V8 a, V8 b) { return {_mm_mul_ps(a.lo, b.lo), _mm_mul_ps(a.hi, b.hi)}; }
FWN_INLINE V8 Div(V8 a, V8 b) { return {_mm_div_ps(a.lo, b.lo), _mm_div_ps(a.hi, b.hi)}; }
FWN_INLINE V8 Abs(V8 a) { const __m128 m = _mm_set1_ps(-0.0f); return {_mm_andnot_ps(m, a.lo), _mm_andnot_ps(m, a.hi)}; }
FWN_INLINE V8 MaxV(V8 a, V8 b) { return {_mm_max_ps(a.lo, b.lo), _mm_max_ps(a.hi, b.hi)}; }
#else
struct V8
{
    float v[8];
};
FWN_INLINE V8 Load(const float* p) { V8 r; for (int i = 0; i < 8; ++i) r.v[i] = p[i]; return r; }
FWN_INLINE void Store(float* p, V8 a) { for (int i = 0; i < 8; ++i) p[i] = a.v[i]; }
FWN_INLINE V8 Set1(float x) { V8 r; for (int i = 0; i < 8; ++i) r.v[i] = x; return r; }
FWN_INLINE V8 Bcast(const float* p) { return Set1(*p); }
FWN_INLINE V8 Fma(V8 a, V8 b, V8 c) { V8 r; for (int i = 0; i < 8; ++i) r.v[i] = a.v[i] * b.v[i] + c.v[i]; return r; }
FWN_INLINE V8 Add(V8 a, V8 b) { V8 r; for (int i = 0; i < 8; ++i) r.v[i] = a.v[i] + b.v[i]; return r; }
FWN_INLINE V8 Mul(V8 a, V8 b) { V8 r; for (int i = 0; i < 8; ++i) r.v[i] = a.v[i] * b.v[i]; return r; }
FWN_INLINE V8 Div(V8 a, V8 b) { V8 r; for (int i = 0; i < 8; ++i) r.v[i] = a.v[i] / b.v[i]; return r; }
FWN_INLINE V8 Abs(V8 a) { V8 r; for (int i = 0; i < 8; ++i) r.v[i] = std::fabs(a.v[i]); return r; }
FWN_INLINE V8 MaxV(V8 a, V8 b) { V8 r; for (int i = 0; i < 8; ++i) r.v[i] = a.v[i] > b.v[i] ? a.v[i] : b.v[i]; return r; }
#endif

// nam::activations::fast_tanh, lane-wise.
FWN_INLINE V8 FastTanh(V8 x)
#if defined(FWN_NO_ACT)
{ return x; } // timing only: what the activation costs
inline V8 FastTanhReal(V8 x)
#endif
{
    const V8 ax = Abs(x);
    const V8 x2 = Mul(x, x);
    const V8 a = Set1(2.45550750702956f);
    const V8 num = Mul(x, Fma(Fma(Set1(0.821226666969744f), ax, Set1(0.893229853513558f)), x2, Fma(a, ax, a)));
    const V8 d = Set1(2.44506634652299f);
    const V8 den = Fma(Add(d, x2), Abs(Fma(Mul(Set1(0.814642734961073f), x), ax, x)), d);
    return Div(num, den);
}

inline int RoundUp8(int n) { return (n + 7) & ~7; }
inline int NextPow2(int v) { int p = 1; while (p < v) p <<= 1; return p; }

// out[o][f] (+)= bias[o] + sum_i W[i][o] * in[i][f], for npad frames (a multiple of 8).
// W is input-major: the nout weights for input i are contiguous. Register tile: OT outputs x FT 8-frame vectors.
#ifndef FWN_OT
#define FWN_OT 4
#endif
#ifndef FWN_FT
#define FWN_FT 2
#endif
template <int OT, int FT>
FWN_INLINE void GemmTile(const float* W, int nout, const float* bias, const float* const* inRows, int nin,
                         float* out, int outStride, int o0, int f0, bool accumulate)
{
    V8 acc[OT][FT];
    for (int oo = 0; oo < OT; ++oo)
    {
        const V8 b = bias ? Set1(bias[o0 + oo]) : Set1(0.0f);
        for (int ft = 0; ft < FT; ++ft)
            acc[oo][ft] = accumulate ? Add(Load(out + (o0 + oo) * outStride + f0 + 8 * ft), b) : b;
    }
    for (int i = 0; i < nin; ++i)
    {
        const float* x = inRows[i] + f0;
        V8 xv[FT];
        for (int ft = 0; ft < FT; ++ft)
            xv[ft] = Load(x + 8 * ft);
        const float* w = W + i * nout + o0;
        for (int oo = 0; oo < OT; ++oo)
        {
            const V8 wv = Bcast(w + oo);
            for (int ft = 0; ft < FT; ++ft)
                acc[oo][ft] = Fma(wv, xv[ft], acc[oo][ft]);
        }
    }
    for (int oo = 0; oo < OT; ++oo)
        for (int ft = 0; ft < FT; ++ft)
            Store(out + (o0 + oo) * outStride + f0 + 8 * ft, acc[oo][ft]);
}

template <int FT>
FWN_INLINE void GemmFrames(const float* W, int nout, const float* bias, const float* const* inRows, int nin,
                           float* out, int outStride, int f0, bool accumulate)
{
    int o0 = 0;
    for (; o0 + FWN_OT <= nout; o0 += FWN_OT)
        GemmTile<FWN_OT, FT>(W, nout, bias, inRows, nin, out, outStride, o0, f0, accumulate);
    if (FWN_OT > 4)
        for (; o0 + 4 <= nout; o0 += 4)
            GemmTile<4, FT>(W, nout, bias, inRows, nin, out, outStride, o0, f0, accumulate);
    for (; o0 + 2 <= nout; o0 += 2)
        GemmTile<2, FT>(W, nout, bias, inRows, nin, out, outStride, o0, f0, accumulate);
    for (; o0 < nout; ++o0)
        GemmTile<1, FT>(W, nout, bias, inRows, nin, out, outStride, o0, f0, accumulate);
}

// General matrix x frames product over a list of input rows (a dilated conv passes K*C rows: one per tap and channel).
inline void Gemm(const float* W, int nout, const float* bias, const float* const* inRows, int nin, float* out,
                 int outStride, int npad, bool accumulate)
{
    int f0 = 0;
    for (; f0 + 8 * FWN_FT <= npad; f0 += 8 * FWN_FT)
        GemmFrames<FWN_FT>(W, nout, bias, inRows, nin, out, outStride, f0, accumulate);
    if (FWN_FT > 2)
        for (; f0 + 16 <= npad; f0 += 16)
            GemmFrames<2>(W, nout, bias, inRows, nin, out, outStride, f0, accumulate);
    for (; f0 < npad; f0 += 8)
        GemmFrames<1>(W, nout, bias, inRows, nin, out, outStride, f0, accumulate);
}

FWN_INLINE V8 LeakyRelu(V8 x, V8 slope)
{
    return MaxV(x, Mul(x, slope)); // slope in [0, 1)
}

enum class Act
{
    Tanh,
    LeakyRelu
};

// A channel-major pow2 ring with a mirror of its first npadMax columns, so any window is contiguous.
struct Ring
{
    std::vector<float> hist;
    int P = 0, mask = 0, rowStride = 0, wp = 0;

    void Allocate(int rows, int lookback, int npadMax)
    {
        P = NextPow2(lookback + npadMax);
        mask = P - 1;
        rowStride = P + npadMax;
        hist.assign(size_t(rows) * rowStride, 0.0f);
        wp = 0;
    }

    void Write(const float* src, int srcStride, int rows, int n, int npadMax)
    {
        for (int c = 0; c < rows; ++c)
        {
            float* row = hist.data() + size_t(c) * rowStride;
            const float* s = src + size_t(c) * srcStride;
            const int first = std::min(n, P - wp);
            std::memcpy(row + wp, s, sizeof(float) * first);
            if (first < n)
                std::memcpy(row, s + first, sizeof(float) * (n - first));
            const int lo = (first < n) ? 0 : wp;
            const int hi = (first < n) ? (n - first) : (wp + n);
            if (lo < npadMax)
                std::memcpy(row + P + lo, row + lo, sizeof(float) * (std::min(hi, npadMax) - lo));
        }
    }

    // Rows for a causal K-tap conv with this dilation: tap k reads (K-1-k)*d frames back.
    void TapRows(const float** rows, int channels, int K, int dilation) const
    {
        for (int k = 0; k < K; ++k)
        {
            const int start = (wp - (K - 1 - k) * dilation) & mask;
            for (int i = 0; i < channels; ++i)
                rows[k * channels + i] = hist.data() + size_t(i) * rowStride + start;
        }
    }

    void Advance(int n) { wp = (wp + n) & mask; }

    // Every column of each row set to that row's first source frame: the history of a constant input.
    void Fill(const float* src, int srcStride, int rows)
    {
        for (int c = 0; c < rows; ++c)
            std::fill_n(hist.data() + size_t(c) * rowStride, rowStride, src[size_t(c) * srcStride]);
    }
};

struct Layer
{
    int K = 0, baseDilation = 0, dilation = 0, lookback = 0;
    Act act = Act::Tanh;
    float slope = 0.01f;
    Ring ring;
    std::vector<float> convW;  // [k][i][o] input-major per tap, so taps and channels flatten into K*C rows
    std::vector<float> convB;  // [o]
    std::vector<float> mixW;   // [o]
    std::vector<float> l1W;    // [i][o]
    std::vector<float> l1B;    // [o]
};

struct Array
{
    int inSize = 0, C = 0, H = 0, headK = 1;
    bool headBias = false;
    std::vector<float> rechW; // [i][o], inSize -> C, no bias
    std::vector<Layer> layers;
    std::vector<float> headW; // [k][i][o], C -> H
    std::vector<float> headB; // [o]
    Ring headRing;            // only when headK > 1
    std::vector<float> cur, hs, z, headOut; // rows of npadMax
};

class FastWaveNet : public nam::DSP
{
  public:
    FastWaveNet(std::vector<Array> arrays, double sr) : nam::DSP(1, 1, sr), mArrays(std::move(arrays))
    {
        mPrewarm = 1;
        for (auto& a : mArrays)
        {
            for (auto& L : a.layers)
                mPrewarm += L.lookback;
            mPrewarm += a.headK - 1;
        }
    }

    float headScale = 1.0f;

    // Oversampled rendering: every temporal dilation, the heads' included, scales with the rate.
    void SetTimeScale(const int scale) override
    {
        mTimeScale = std::max(1, scale);
        mPrewarm = 1;
        for (auto& a : mArrays)
        {
            for (auto& L : a.layers)
            {
                L.dilation = L.baseDilation * mTimeScale;
                L.lookback = (L.K - 1) * L.dilation;
                mPrewarm += L.lookback;
            }
            mPrewarm += (a.headK - 1) * mTimeScale;
        }
        if (mMaxFrames > 0)
            SetMaxBufferSize(GetMaxBufferSize());
    }

    void process(NAM_SAMPLE** input, NAM_SAMPLE** output, const int num_frames) override
    {
        for (int done = 0; done < num_frames;)
        {
            const int n = std::min(num_frames - done, mMaxFrames);
            ProcessChunk(input[0] + done, output[0] + done, n);
            done += n;
        }
    }

  protected:
    int PrewarmSamples() override { return mPrewarm; }

#if !defined(FWN_REAL_PREWARM)
  public:
    // Silence in, for longer than the receptive field, leaves every layer's history holding one constant
    // column: the network has no feedback. So fill each history with it, layer by layer, in one 8-frame
    // pass, instead of running PrewarmSamples() frames. Every frame takes the same arithmetic, so the
    // state matches a real prewarm bit for bit.
    void prewarm() override
    {
        if (mMaxFrames == 0)
            SetMaxBufferSize(64);
        NAM_SAMPLE zeros[8] = {};
        NAM_SAMPLE sink[8];
        ProcessChunk(zeros, sink, std::min(8, mMaxFrames), true);
    }

  protected:
#endif

    void SetMaxBufferSize(const int maxBufferSize) override
    {
        nam::DSP::SetMaxBufferSize(maxBufferSize);
#if FWN_CHUNK > 0
        // Bigger host blocks run in chunks, so the working rows stay in L1.
        mMaxFrames = std::max(1, std::min(maxBufferSize, FWN_CHUNK));
#else
        mMaxFrames = std::max(1, maxBufferSize);
#endif
        mNpadMax = RoundUp8(mMaxFrames);
        mCond.assign(mNpadMax, 0.0f);
        size_t maxRows = 0;
        for (auto& a : mArrays)
        {
            a.cur.assign(size_t(a.C) * mNpadMax, 0.0f);
            a.hs.assign(size_t(a.C) * mNpadMax, 0.0f);
            a.z.assign(size_t(a.C) * mNpadMax, 0.0f);
            a.headOut.assign(size_t(a.H) * mNpadMax, 0.0f);
            for (auto& L : a.layers)
            {
                L.ring.Allocate(a.C, L.lookback, mNpadMax);
                maxRows = std::max(maxRows, size_t(L.K) * a.C);
            }
            if (a.headK > 1)
                a.headRing.Allocate(a.C, (a.headK - 1) * mTimeScale, mNpadMax);
            maxRows = std::max(maxRows, size_t(a.headK) * a.C);
            maxRows = std::max(maxRows, size_t(std::max(a.C, a.inSize)));
        }
        mRows.assign(maxRows, nullptr);
    }

  private:

    void ProcessChunk(const NAM_SAMPLE* in, NAM_SAMPLE* out, int n, bool prime = false)
    {
        const int npad = RoundUp8(n);
        for (int f = 0; f < n; ++f)
            mCond[f] = static_cast<float>(in[f]);
        for (int f = n; f < npad; ++f)
            mCond[f] = 0.0f;
        const float* condRow = mCond.data();

        const Array* prev = nullptr;
        for (auto& a : mArrays)
        {
            const int C = a.C;
            // Rechannel into cur.
            const float** rows = mRows.data();
            if (!prev)
                rows[0] = condRow;
            else
                for (int i = 0; i < a.inSize; ++i)
                    rows[i] = prev->cur.data() + size_t(i) * mNpadMax;
            Gemm(a.rechW.data(), C, nullptr, rows, a.inSize, a.cur.data(), mNpadMax, npad, false);

            // Head accumulator starts at zero, or at the previous array's head output.
            if (!prev)
                std::fill(a.hs.begin(), a.hs.begin() + size_t(C) * mNpadMax, 0.0f);
            else
                std::memcpy(a.hs.data(), prev->headOut.data(), sizeof(float) * size_t(C) * mNpadMax);

            for (auto& L : a.layers)
            {
                if (prime)
                    L.ring.Fill(a.cur.data(), mNpadMax, C);
                L.ring.Write(a.cur.data(), mNpadMax, C, n, mNpadMax);
                L.ring.TapRows(rows, C, L.K, L.dilation);
                float* z = a.z.data();
                Gemm(L.convW.data(), C, L.convB.data(), rows, L.K * C, z, mNpadMax, npad, false);
                // Mixin, activation, head sum.
                const V8 slope = Set1(L.slope);
                for (int o = 0; o < C; ++o)
                {
                    float* zr = z + size_t(o) * mNpadMax;
                    float* hr = a.hs.data() + size_t(o) * mNpadMax;
                    const V8 mw = Set1(L.mixW[o]);
                    if (L.act == Act::Tanh)
                        for (int f = 0; f < npad; f += 8)
                        {
                            const V8 t = FastTanh(Fma(mw, Load(condRow + f), Load(zr + f)));
                            Store(zr + f, t);
                            Store(hr + f, Add(Load(hr + f), t));
                        }
                    else
                        for (int f = 0; f < npad; f += 8)
                        {
                            const V8 t = LeakyRelu(Fma(mw, Load(condRow + f), Load(zr + f)), slope);
                            Store(zr + f, t);
                            Store(hr + f, Add(Load(hr + f), t));
                        }
                }
                // layer1x1 residual, in place on cur.
                for (int i = 0; i < C; ++i)
                    rows[i] = z + size_t(i) * mNpadMax;
                Gemm(L.l1W.data(), C, L.l1B.data(), rows, C, a.cur.data(), mNpadMax, npad, true);
                L.ring.Advance(n);
            }

            // Head rechannel C -> H, a causal conv when the head has more than one tap.
            if (a.headK > 1)
            {
                if (prime)
                    a.headRing.Fill(a.hs.data(), mNpadMax, C);
                a.headRing.Write(a.hs.data(), mNpadMax, C, n, mNpadMax);
                a.headRing.TapRows(rows, C, a.headK, mTimeScale);
            }
            else
                for (int i = 0; i < C; ++i)
                    rows[i] = a.hs.data() + size_t(i) * mNpadMax;
            Gemm(a.headW.data(), a.H, a.headBias ? a.headB.data() : nullptr, rows, a.headK * C, a.headOut.data(),
                 mNpadMax, npad, false);
            if (a.headK > 1)
                a.headRing.Advance(n);
            prev = &a;
        }

        const float* y = prev->headOut.data();
        for (int f = 0; f < n; ++f)
            out[f] = static_cast<NAM_SAMPLE>(headScale * y[f]);
    }

    std::vector<Array> mArrays;
    std::vector<float> mCond;
    std::vector<const float*> mRows;
    int mMaxFrames = 0, mNpadMax = 0, mPrewarm = 0, mTimeScale = 1;
};

// ---------------------------------------------------------------- shape detection and weights
// Tanh, or LeakyReLU with a slope in [0, 1). Anything else is left to the generic path.
inline bool ParseAct(const nlohmann::json& a, Act& act, float& slope)
{
    std::string type;
    if (a.is_string())
        type = a.get<std::string>();
    else if (a.is_object())
        type = a.value("type", std::string());
    else
        return false;
    if (type == "Tanh" && (a.is_string() || a.size() == 1))
    {
        act = Act::Tanh;
        return true;
    }
    if (type == "LeakyReLU")
    {
        act = Act::LeakyRelu;
        slope = a.is_object() ? a.value("negative_slope", 0.01f) : 0.01f;
        return slope >= 0.0f && slope < 1.0f && (a.is_string() || a.size() <= 2);
    }
    return false;
}

inline bool FilmOff(const nlohmann::json& l, const char* key)
{
    auto it = l.find(key);
    if (it == l.end() || it->is_null())
        return true;
    if (it->is_boolean())
        return !it->get<bool>();
    if (it->is_object())
        return !it->value("active", true);
    return false;
}

inline bool AllNone(const nlohmann::json& g)
{
    if (g.is_string())
        return g.get<std::string>() == "none";
    if (!g.is_array())
        return false;
    for (const auto& x : g)
        if (!x.is_string() || x.get<std::string>() != "none")
            return false;
    return true;
}

inline std::unique_ptr<nam::DSP> TryCreateFromConfig(const nlohmann::json& j, const std::vector<float>& weights,
                                                     double sr)
{
    if (j.value("architecture", std::string()) != "WaveNet")
        return nullptr;
    const auto& cfg = j["config"];
    if (cfg.contains("head") && !cfg["head"].is_null())
        return nullptr;
    if (cfg.contains("condition_dsp") && !cfg["condition_dsp"].is_null())
        return nullptr;
    if (cfg.value("in_channels", 1) != 1)
        return nullptr;

    std::vector<Array> arrays;
    int prevC = 1, prevH = -1;
    for (const auto& l : cfg["layers"])
    {
        if (l.contains("slimmable") && !l["slimmable"].is_null())
            return nullptr;
        Array a;
        a.inSize = l["input_size"].get<int>();
        a.C = l["channels"].get<int>();
        if (l.value("bottleneck", a.C) != a.C || l["condition_size"].get<int>() != 1 || a.inSize != prevC)
            return nullptr;
        if (prevH >= 0 && prevH != a.C)
            return nullptr;
        if (l.value("groups_input", 1) != 1 || l.value("groups_input_mixin", 1) != 1)
            return nullptr;
        if (l.contains("layer1x1") && (!l["layer1x1"].value("active", true) || l["layer1x1"].value("groups", 1) != 1))
            return nullptr;
        if (l.contains("head1x1") && l["head1x1"].value("active", false))
            return nullptr;
        if (l.contains("gating_mode"))
        {
            if (!AllNone(l["gating_mode"]))
                return nullptr;
        }
        else if (l.value("gated", false))
            return nullptr;
        for (const char* f : {"conv_pre_film", "conv_post_film", "input_mixin_pre_film", "input_mixin_post_film",
                              "activation_pre_film", "activation_post_film", "layer1x1_post_film", "head1x1_post_film"})
            if (!FilmOff(l, f))
                return nullptr;
        if (l.contains("head") && !l["head"].is_null())
        {
            const auto& h = l["head"];
            a.headK = h.at("kernel_size").get<int>();
            a.H = h.at("out_channels").get<int>();
            a.headBias = h.at("bias").get<bool>();
            if (a.headK < 1)
                return nullptr;
        }
        else
        {
            a.H = l.at("head_size").get<int>();
            a.headBias = l.at("head_bias").get<bool>();
        }
        const auto& dil = l["dilations"];
        std::vector<int> ks;
        if (l.contains("kernel_sizes"))
            ks = l["kernel_sizes"].get<std::vector<int>>();
        else
            ks.assign(dil.size(), l["kernel_size"].get<int>());
        if (ks.size() != dil.size())
            return nullptr;
        const auto& actJson = l["activation"];
        if (actJson.is_array() && actJson.size() != dil.size())
            return nullptr;
        for (size_t i = 0; i < dil.size(); ++i)
        {
            Layer L;
            L.K = ks[i];
            L.baseDilation = std::max(1, dil[i].get<int>());
            L.dilation = L.baseDilation;
            L.lookback = (L.K - 1) * L.dilation;
            if (L.K < 1 || !ParseAct(actJson.is_array() ? actJson[i] : actJson, L.act, L.slope))
                return nullptr;
            a.layers.push_back(std::move(L));
        }
        prevC = a.C;
        prevH = a.H;
        arrays.push_back(std::move(a));
    }
    if (arrays.empty() || arrays.back().H != 1)
        return nullptr;

    // Weights, in the generic loader's order.
    size_t p = 0;
    auto take = [&]() -> float {
        if (p >= weights.size())
            throw std::runtime_error("FusedWaveNet: weights exhausted");
        return weights[p++];
    };
    for (auto& a : arrays)
    {
        const int C = a.C;
        a.rechW.assign(size_t(a.inSize) * C, 0.0f);
        for (int o = 0; o < C; ++o)
            for (int i = 0; i < a.inSize; ++i)
                a.rechW[size_t(i) * C + o] = take();
        for (auto& L : a.layers)
        {
            L.convW.assign(size_t(L.K) * C * C, 0.0f);
            for (int o = 0; o < C; ++o)
                for (int i = 0; i < C; ++i)
                    for (int k = 0; k < L.K; ++k)
                        L.convW[(size_t(k) * C + i) * C + o] = take();
            L.convB.resize(C);
            for (int o = 0; o < C; ++o)
                L.convB[o] = take();
            L.mixW.resize(C);
            for (int o = 0; o < C; ++o)
                L.mixW[o] = take();
            L.l1W.assign(size_t(C) * C, 0.0f);
            for (int o = 0; o < C; ++o)
                for (int i = 0; i < C; ++i)
                    L.l1W[size_t(i) * C + o] = take();
            L.l1B.resize(C);
            for (int o = 0; o < C; ++o)
                L.l1B[o] = take();
        }
        a.headW.assign(size_t(a.headK) * C * a.H, 0.0f);
        for (int o = 0; o < a.H; ++o)
            for (int i = 0; i < C; ++i)
                for (int k = 0; k < a.headK; ++k)
                    a.headW[(size_t(k) * C + i) * a.H + o] = take();
        a.headB.assign(a.H, 0.0f);
        if (a.headBias)
            for (int o = 0; o < a.H; ++o)
                a.headB[o] = take();
    }
    const float headScale = take();
    if (p != weights.size())
        return nullptr;
    auto dsp = std::make_unique<FastWaveNet>(std::move(arrays), sr);
    dsp->headScale = headScale;
    return dsp;
}

// A plain WaveNet file, or the submodel a SlimmableContainer would pick at this size.
inline std::unique_ptr<nam::DSP> TryCreate(const std::string& path, double size = 1.0)
{
    std::ifstream f(path);
    nlohmann::json j = nlohmann::json::parse(f);
    if (j.value("architecture", std::string()) == "SlimmableContainer")
    {
        const auto& subs = j["config"]["submodels"];
        size_t pick = subs.size() - 1;
        for (size_t i = 0; i < subs.size(); ++i)
            if (size < subs[i]["max_value"].get<double>())
            {
                pick = i;
                break;
            }
        nlohmann::json m = subs[pick]["model"];
        j = std::move(m);
    }
    const double sr = j.contains("sample_rate") && j["sample_rate"].is_number() ? j["sample_rate"].get<double>() : -1.0;
    std::vector<float> w = j["weights"].get<std::vector<float>>();
    return TryCreateFromConfig(j, w, sr);
}
} // namespace fusedwn
