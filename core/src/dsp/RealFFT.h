#pragma once

#include "SimdFFT.h"

#include <complex>
#include <cstddef>
#include <vector>

namespace guitarfx
{
/**
 * FFT of N real samples, run as an N/2-point complex FFT (SimdFFT) plus one split pass: about half
 * the work of an N-point complex transform of the same samples, and half the spectrum to store and
 * multiply. The real samples x[2n], x[2n + 1] go in as complex point n.
 *
 * The spectrum is "packed": N/2 complex bins. Bin k, 0 < k < N/2, is X[k]; the bins above N/2 are
 * their conjugates and are not stored. X[0] and X[N/2] are both real, so bin 0 holds X[0] in its
 * real part and X[N/2] in its imaginary part, and a product of two packed spectra takes
 * MultiplyAccumulate(), which treats bin 0 as two real products.
 *
 * The input comes in two halves, x[0, N/2) from `lo` and x[N/2, N) from `hi`: overlap-save's
 * [previous block | current block] frame, without building it. The inverse leaves N real samples
 * in GetResult(), unscaled (N times x) like SimdFFT's.
 *
 * Stepped transform, one slice at a time, so its cost can be spread over several callbacks
 * (RealtimeConvolverSpread.cpp). Ranges start and end on multiples of 4, or at the range's end,
 * and then the result is the whole transform's exactly.
 *  - Forward: LoadInput over [0, N/4), which also runs the first butterfly stage, then
 *    RunButterflies for stages 2 to GetNumStages() in order, each over [0, N/4), then
 *    StoreSpectrum over [0, N/4).
 *  - Inverse: LoadSpectrum over [0, N/4), then RunButterflies with `inverse` for stages 1 to
 *    GetNumStages(), each over [0, N/4); then read GetResult().
 * Forward() and Inverse() are those steps run whole. The two directions share one work buffer, so
 * one transform at a time.
 */
class RealFFT
{
  public:
    /// `size` is N, a power of two of at least 4.
    explicit RealFFT(size_t size) : mSize(size), mComplex(size / 2)
    {
        // W^k = e^(-2 pi i k / N) for the split pass's pairs k < N/4, in double so that every
        // twiddle is rounded once. Real and imaginary parts apart, as the SIMD passes load them.
        mTwiddleRe.resize(mSize / 4);
        mTwiddleIm.resize(mSize / 4);

        for (size_t k = 0; k < mTwiddleRe.size(); ++k)
        {
            const double angle = -2.0 * M_PI * static_cast<double>(k) / static_cast<double>(mSize);
            mTwiddleRe[k] = static_cast<float>(std::cos(angle));
            mTwiddleIm[k] = static_cast<float>(std::sin(angle));
        }
    }

    [[nodiscard]] size_t GetSize() const noexcept
    {
        return mSize;
    }

    /// Complex bins in a packed spectrum: N/2.
    [[nodiscard]] size_t GetNumBins() const noexcept
    {
        return mSize / 2;
    }

    /// Butterfly stages of the N/2-point complex transform; each has N/4 butterflies.
    [[nodiscard]] size_t GetNumStages() const noexcept
    {
        return mComplex.GetNumStages();
    }

    void Forward(std::complex<float>* spectrum, const float* lo, const float* hi) const
    {
        const size_t quarter = mSize / 4;
        LoadInput(lo, hi, 0, quarter);

        for (size_t stage = 2; stage <= GetNumStages(); ++stage)
        {
            RunButterflies(stage, 0, quarter, false);
        }

        StoreSpectrum(spectrum, 0, quarter);
    }

    /// Leaves the N real samples, unscaled, in GetResult().
    void Inverse(const std::complex<float>* spectrum) const
    {
        const size_t quarter = mSize / 4;
        LoadSpectrum(spectrum, 0, quarter);

        for (size_t stage = 1; stage <= GetNumStages(); ++stage)
        {
            RunButterflies(stage, 0, quarter, true);
        }
    }

    // Loads the input into the work buffer in bit-reversed order and runs the first stage on it.
    // Butterfly b of that stage joins points 2b and 2b + 1, which bit-reverse to points r and
    // r + N/4 for r = bitrev(2b): the same place in `lo` and in `hi`.
    void LoadInput(const float* lo, const float* hi, size_t begin, size_t end) const
    {
        float* work = mComplex.mWorkBuffer;
        const size_t* bitReversed = mComplex.mBitReversed.data();

        for (size_t b = begin; b < end; ++b)
        {
            const size_t r = bitReversed[2 * b] * 2;
            const float ur = lo[r];
            const float ui = lo[r + 1];
            const float tr = hi[r];
            const float ti = hi[r + 1];
            float* p = work + b * 4;
            p[0] = ur + tr;
            p[1] = ui + ti;
            p[2] = ur - tr;
            p[3] = ui - ti;
        }
    }

    void RunButterflies(size_t stage, size_t begin, size_t end, bool inverse) const
    {
        mComplex.ButterflyRange(mComplex.mWorkBuffer, stage, begin, end, inverse);
    }

    // The split pass, pairs [begin, end): pair k writes bins k and N/2 - k from the complex
    // transform Z of the packed input. With E = (Z[k] + conj Z[N/2-k]) / 2, the even samples'
    // spectrum, and O = (Z[k] - conj Z[N/2-k]) / 2i, the odd ones', X[k] = E + W^k O and
    // X[N/2-k] = conj(E - W^k O). Pair 0 writes bin 0 (packed) and bin N/4, which pairs with itself.
    void StoreSpectrum(std::complex<float>* spectrum, size_t begin, size_t end) const
    {
        const float* z = mComplex.mWorkBuffer;
        float* out = reinterpret_cast<float*>(spectrum);
        const size_t half = mSize / 2;
        size_t k = begin;

        if (k == 0 && k < end)
        {
            out[0] = z[0] + z[1];
            out[1] = z[0] - z[1];
            out[half] = z[half];
            out[half + 1] = -z[half + 1];
            ++k;
        }

        // Four pairs at a time from a multiple of 4, so a pair takes the same path however the
        // transform is sliced.
        for (; k < end && (k & 3) != 0; ++k)
        {
            StorePair(z, out, k);
        }

#if defined(GUITARFX_ARCH_ARM64)

        const float32x4_t halfOf = vdupq_n_f32(0.5f);

        for (; k + 4 <= end; k += 4)
        {
            const size_t mirror = (half - k - 3) * 2; // points N/2-k-3 .. N/2-k, the mirror reversed
            const float32x4x2_t a = vld2q_f32(z + k * 2);
            const float32x4x2_t m = vld2q_f32(z + mirror);
            const float32x4_t br = Reverse(m.val[0]);
            const float32x4_t bi = vnegq_f32(Reverse(m.val[1]));

            const float32x4_t er = vmulq_f32(halfOf, vaddq_f32(a.val[0], br));
            const float32x4_t ei = vmulq_f32(halfOf, vaddq_f32(a.val[1], bi));
            const float32x4_t or_ = vmulq_f32(halfOf, vsubq_f32(a.val[1], bi));
            const float32x4_t oi = vmulq_f32(halfOf, vsubq_f32(br, a.val[0]));

            const float32x4_t wr = vld1q_f32(mTwiddleRe.data() + k);
            const float32x4_t wi = vld1q_f32(mTwiddleIm.data() + k);
            const float32x4_t wor = vfmsq_f32(vmulq_f32(or_, wr), oi, wi);
            const float32x4_t woi = vfmaq_f32(vmulq_f32(or_, wi), oi, wr);

            float32x4x2_t x;
            x.val[0] = vaddq_f32(er, wor);
            x.val[1] = vaddq_f32(ei, woi);
            vst2q_f32(out + k * 2, x);
            x.val[0] = Reverse(vsubq_f32(er, wor));
            x.val[1] = Reverse(vsubq_f32(woi, ei));
            vst2q_f32(out + mirror, x);
        }

#elif defined(GUITARFX_ARCH_X86)

        const __m128 halfOf = _mm_set1_ps(0.5f);
        const __m128 zero = _mm_setzero_ps();

        for (; k + 4 <= end; k += 4)
        {
            const size_t mirror = (half - k - 3) * 2;
            const __m128 a0 = _mm_loadu_ps(z + k * 2);
            const __m128 a1 = _mm_loadu_ps(z + k * 2 + 4);
            const __m128 m0 = _mm_loadu_ps(z + mirror);
            const __m128 m1 = _mm_loadu_ps(z + mirror + 4);
            const __m128 ar = _mm_shuffle_ps(a0, a1, _MM_SHUFFLE(2, 0, 2, 0));
            const __m128 ai = _mm_shuffle_ps(a0, a1, _MM_SHUFFLE(3, 1, 3, 1));
            const __m128 br = _mm_shuffle_ps(m1, m0, _MM_SHUFFLE(0, 2, 0, 2));                   // reversed
            const __m128 bi = _mm_sub_ps(zero, _mm_shuffle_ps(m1, m0, _MM_SHUFFLE(1, 3, 1, 3))); // reversed

            const __m128 er = _mm_mul_ps(halfOf, _mm_add_ps(ar, br));
            const __m128 ei = _mm_mul_ps(halfOf, _mm_add_ps(ai, bi));
            const __m128 or_ = _mm_mul_ps(halfOf, _mm_sub_ps(ai, bi));
            const __m128 oi = _mm_mul_ps(halfOf, _mm_sub_ps(br, ar));

            const __m128 wr = _mm_loadu_ps(mTwiddleRe.data() + k);
            const __m128 wi = _mm_loadu_ps(mTwiddleIm.data() + k);
            const __m128 wor = _mm_sub_ps(_mm_mul_ps(or_, wr), _mm_mul_ps(oi, wi));
            const __m128 woi = _mm_add_ps(_mm_mul_ps(or_, wi), _mm_mul_ps(oi, wr));

            const __m128 xr = _mm_add_ps(er, wor);
            const __m128 xi = _mm_add_ps(ei, woi);
            _mm_storeu_ps(out + k * 2, _mm_unpacklo_ps(xr, xi));
            _mm_storeu_ps(out + k * 2 + 4, _mm_unpackhi_ps(xr, xi));

            const __m128 yr = _mm_sub_ps(er, wor);
            const __m128 yi = _mm_sub_ps(woi, ei);
            const __m128 yrRev = _mm_shuffle_ps(yr, yr, _MM_SHUFFLE(0, 1, 2, 3));
            const __m128 yiRev = _mm_shuffle_ps(yi, yi, _MM_SHUFFLE(0, 1, 2, 3));
            _mm_storeu_ps(out + mirror, _mm_unpacklo_ps(yrRev, yiRev));
            _mm_storeu_ps(out + mirror + 4, _mm_unpackhi_ps(yrRev, yiRev));
        }

#endif

        for (; k < end; ++k)
        {
            StorePair(z, out, k);
        }
    }

    // The split pass undone, pairs [begin, end), written straight to the work buffer in
    // bit-reversed order. It builds 2Z, so the inverse complex transform's N/2 scaling becomes
    // the N an N-point transform would have: E2 = X[k] + conj X[N/2-k] and
    // D2 = (X[k] - conj X[N/2-k]) conj(W^k) are 2E and 2O, and 2Z[k] = E2 + i D2,
    // 2Z[N/2-k] = conj(E2) + i conj(D2).
    void LoadSpectrum(const std::complex<float>* spectrum, size_t begin, size_t end) const
    {
        const float* x = reinterpret_cast<const float*>(spectrum);
        float* work = mComplex.mWorkBuffer;
        const size_t* bitReversed = mComplex.mBitReversed.data();
        const size_t half = mSize / 2;
        size_t k = begin;

        if (k == 0 && k < end)
        {
            // Bin 0 is X[0] and X[N/2]: E = (X[0] + X[N/2]) / 2, O = (X[0] - X[N/2]) / 2.
            work[0] = x[0] + x[1];
            work[1] = x[0] - x[1];
            const size_t mid = bitReversed[half / 2] * 2;
            work[mid] = 2.0f * x[half];
            work[mid + 1] = -2.0f * x[half + 1];
            ++k;
        }

        for (; k < end && (k & 3) != 0; ++k)
        {
            LoadPair(x, work, k);
        }

#if defined(GUITARFX_ARCH_ARM64)

        for (; k + 4 <= end; k += 4)
        {
            const size_t mirror = (half - k - 3) * 2;
            const float32x4x2_t p = vld2q_f32(x + k * 2);
            const float32x4x2_t m = vld2q_f32(x + mirror);
            const float32x4_t qr = Reverse(m.val[0]);
            const float32x4_t qi = vnegq_f32(Reverse(m.val[1]));

            const float32x4_t er = vaddq_f32(p.val[0], qr);
            const float32x4_t ei = vaddq_f32(p.val[1], qi);
            const float32x4_t sr = vsubq_f32(p.val[0], qr);
            const float32x4_t si = vsubq_f32(p.val[1], qi);

            const float32x4_t wr = vld1q_f32(mTwiddleRe.data() + k);
            const float32x4_t wi = vld1q_f32(mTwiddleIm.data() + k);
            const float32x4_t dr = vfmaq_f32(vmulq_f32(sr, wr), si, wi);
            const float32x4_t di = vfmsq_f32(vmulq_f32(si, wr), sr, wi);

            // Lane j is pair k + j: 2Z[k + j], and 2Z[N/2 - k - j], each to its bit-reversed point.
            const float32x4_t zkr = vsubq_f32(er, di);
            const float32x4_t zki = vaddq_f32(ei, dr);
            const float32x4_t zmr = vaddq_f32(er, di);
            const float32x4_t zmi = vsubq_f32(dr, ei);
            const float32x4_t k01 = vzip1q_f32(zkr, zki);
            const float32x4_t k23 = vzip2q_f32(zkr, zki);
            const float32x4_t m01 = vzip1q_f32(zmr, zmi);
            const float32x4_t m23 = vzip2q_f32(zmr, zmi);
            vst1_f32(work + bitReversed[k] * 2, vget_low_f32(k01));
            vst1_f32(work + bitReversed[k + 1] * 2, vget_high_f32(k01));
            vst1_f32(work + bitReversed[k + 2] * 2, vget_low_f32(k23));
            vst1_f32(work + bitReversed[k + 3] * 2, vget_high_f32(k23));
            vst1_f32(work + bitReversed[half - k] * 2, vget_low_f32(m01));
            vst1_f32(work + bitReversed[half - k - 1] * 2, vget_high_f32(m01));
            vst1_f32(work + bitReversed[half - k - 2] * 2, vget_low_f32(m23));
            vst1_f32(work + bitReversed[half - k - 3] * 2, vget_high_f32(m23));
        }

#elif defined(GUITARFX_ARCH_X86)

        const __m128 zero = _mm_setzero_ps();

        for (; k + 4 <= end; k += 4)
        {
            const size_t mirror = (half - k - 3) * 2;
            const __m128 p0 = _mm_loadu_ps(x + k * 2);
            const __m128 p1 = _mm_loadu_ps(x + k * 2 + 4);
            const __m128 m0 = _mm_loadu_ps(x + mirror);
            const __m128 m1 = _mm_loadu_ps(x + mirror + 4);
            const __m128 pr = _mm_shuffle_ps(p0, p1, _MM_SHUFFLE(2, 0, 2, 0));
            const __m128 pi = _mm_shuffle_ps(p0, p1, _MM_SHUFFLE(3, 1, 3, 1));
            const __m128 qr = _mm_shuffle_ps(m1, m0, _MM_SHUFFLE(0, 2, 0, 2));
            const __m128 qi = _mm_sub_ps(zero, _mm_shuffle_ps(m1, m0, _MM_SHUFFLE(1, 3, 1, 3)));

            const __m128 er = _mm_add_ps(pr, qr);
            const __m128 ei = _mm_add_ps(pi, qi);
            const __m128 sr = _mm_sub_ps(pr, qr);
            const __m128 si = _mm_sub_ps(pi, qi);

            const __m128 wr = _mm_loadu_ps(mTwiddleRe.data() + k);
            const __m128 wi = _mm_loadu_ps(mTwiddleIm.data() + k);
            const __m128 dr = _mm_add_ps(_mm_mul_ps(sr, wr), _mm_mul_ps(si, wi));
            const __m128 di = _mm_sub_ps(_mm_mul_ps(si, wr), _mm_mul_ps(sr, wi));

            const __m128 zkr = _mm_sub_ps(er, di);
            const __m128 zki = _mm_add_ps(ei, dr);
            const __m128 zmr = _mm_add_ps(er, di);
            const __m128 zmi = _mm_sub_ps(dr, ei);
            const __m128 k01 = _mm_unpacklo_ps(zkr, zki);
            const __m128 k23 = _mm_unpackhi_ps(zkr, zki);
            const __m128 m01 = _mm_unpacklo_ps(zmr, zmi);
            const __m128 m23 = _mm_unpackhi_ps(zmr, zmi);
            _mm_storel_pi(reinterpret_cast<__m64*>(work + bitReversed[k] * 2), k01);
            _mm_storeh_pi(reinterpret_cast<__m64*>(work + bitReversed[k + 1] * 2), k01);
            _mm_storel_pi(reinterpret_cast<__m64*>(work + bitReversed[k + 2] * 2), k23);
            _mm_storeh_pi(reinterpret_cast<__m64*>(work + bitReversed[k + 3] * 2), k23);
            _mm_storel_pi(reinterpret_cast<__m64*>(work + bitReversed[half - k] * 2), m01);
            _mm_storeh_pi(reinterpret_cast<__m64*>(work + bitReversed[half - k - 1] * 2), m01);
            _mm_storel_pi(reinterpret_cast<__m64*>(work + bitReversed[half - k - 2] * 2), m23);
            _mm_storeh_pi(reinterpret_cast<__m64*>(work + bitReversed[half - k - 3] * 2), m23);
        }

#endif

        for (; k < end; ++k)
        {
            LoadPair(x, work, k);
        }
    }

    /// The inverse transform's N real samples, valid once its last stage has run.
    [[nodiscard]] const float* GetResult() const noexcept
    {
        return mComplex.mWorkBuffer;
    }

    /// acc[k] += a[k] * b[k] over `count` bins of packed spectra, bin 0 of each at index 0.
    static void MultiplyAccumulate(std::complex<float>* acc, const std::complex<float>* a, const std::complex<float>* b,
                                   size_t count)
    {
        const std::complex<float> first(acc[0].real() + a[0].real() * b[0].real(),
                                        acc[0].imag() + a[0].imag() * b[0].imag());
        SimdFFT::ComplexMultiplyAccumulate(acc, a, b, count);
        acc[0] = first;
    }

  private:
#if defined(GUITARFX_ARCH_ARM64)
    static float32x4_t Reverse(float32x4_t v)
    {
        const float32x4_t pairsSwapped = vrev64q_f32(v);
        return vextq_f32(pairsSwapped, pairsSwapped, 2);
    }
#endif

    // StoreSpectrum's pair k (0 < k < N/4), one at a time.
    void StorePair(const float* z, float* out, size_t k) const
    {
        const size_t lo = k * 2;
        const size_t hi = (mSize / 2 - k) * 2;
        const float ar = z[lo];
        const float ai = z[lo + 1];
        const float br = z[hi];
        const float bi = -z[hi + 1];

        const float er = 0.5f * (ar + br);
        const float ei = 0.5f * (ai + bi);
        const float or_ = 0.5f * (ai - bi);
        const float oi = 0.5f * (br - ar);

        const float wr = mTwiddleRe[k];
        const float wi = mTwiddleIm[k];
        const float wor = or_ * wr - oi * wi;
        const float woi = or_ * wi + oi * wr;

        out[lo] = er + wor;
        out[lo + 1] = ei + woi;
        out[hi] = er - wor;
        out[hi + 1] = woi - ei;
    }

    // LoadSpectrum's pair k (0 < k < N/4), one at a time.
    void LoadPair(const float* x, float* work, size_t k) const
    {
        const size_t* bitReversed = mComplex.mBitReversed.data();
        const size_t half = mSize / 2;
        const size_t lo = k * 2;
        const size_t hi = (half - k) * 2;
        const float pr = x[lo];
        const float pi = x[lo + 1];
        const float qr = x[hi];
        const float qi = -x[hi + 1];

        const float er = pr + qr;
        const float ei = pi + qi;
        const float sr = pr - qr;
        const float si = pi - qi;

        // s * conj(w)
        const float wr = mTwiddleRe[k];
        const float wi = mTwiddleIm[k];
        const float dr = sr * wr + si * wi;
        const float di = si * wr - sr * wi;

        float* zk = work + bitReversed[k] * 2;
        float* zm = work + bitReversed[half - k] * 2;
        zk[0] = er - di;
        zk[1] = ei + dr;
        zm[0] = er + di;
        zm[1] = dr - ei;
    }

    size_t mSize;
    SimdFFT mComplex;
    std::vector<float> mTwiddleRe;
    std::vector<float> mTwiddleIm;
};
} // namespace guitarfx
