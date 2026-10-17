#include "dsp/nam/FusedWaveNet.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

#include "dsp/nam/FusedWaveNetSimd.h"

namespace guitarfx::fusedwavenet
{
namespace
{
using namespace simd;

int RoundUp8(int n)
{
    return (n + 7) & ~7;
}

int NextPow2(int v)
{
    int p = 1;

    while (p < v)
    {
        p <<= 1;
    }

    return p;
}

// out[o][f] (+)= bias[o] + sum_i W[i][o] * in[i][f], for one tile of OT outputs x FT 8-frame
// vectors. W is input-major: the nout weights for input i are contiguous. Each output sums its
// inputs in the same order whatever the tile, so every tile gives the same bits.
template <int OT, int FT>
GUITARFX_FWN_INLINE void GemmTile(const float* weights, int nout, const float* bias, const float* const* inRows,
                                  int nin, float* out, int outStride, int o0, int f0, bool accumulate)
{
    V8 acc[OT][FT];

    for (int oo = 0; oo < OT; ++oo)
    {
        const V8 b = bias != nullptr ? Set1(bias[o0 + oo]) : Set1(0.0f);

        for (int ft = 0; ft < FT; ++ft)
        {
            acc[oo][ft] = accumulate ? Add(Load(out + (o0 + oo) * outStride + f0 + 8 * ft), b) : b;
        }
    }

    for (int i = 0; i < nin; ++i)
    {
        const float* x = inRows[i] + f0;
        V8 xv[FT];

        for (int ft = 0; ft < FT; ++ft)
        {
            xv[ft] = Load(x + 8 * ft);
        }

        const float* w = weights + i * nout + o0;

        for (int oo = 0; oo < OT; ++oo)
        {
            const V8 wv = Broadcast(w + oo);

            for (int ft = 0; ft < FT; ++ft)
            {
                acc[oo][ft] = Fma(wv, xv[ft], acc[oo][ft]);
            }
        }
    }

    for (int oo = 0; oo < OT; ++oo)
    {
        for (int ft = 0; ft < FT; ++ft)
        {
            Store(out + (o0 + oo) * outStride + f0 + 8 * ft, acc[oo][ft]);
        }
    }
}

template <int FT>
GUITARFX_FWN_INLINE void GemmFrames(const float* weights, int nout, const float* bias, const float* const* inRows,
                                    int nin, float* out, int outStride, int f0, bool accumulate)
{
    int o0 = 0;

    for (; o0 + kTileOutputs <= nout; o0 += kTileOutputs)
    {
        GemmTile<kTileOutputs, FT>(weights, nout, bias, inRows, nin, out, outStride, o0, f0, accumulate);
    }

    for (; o0 + 2 <= nout; o0 += 2)
    {
        GemmTile<2, FT>(weights, nout, bias, inRows, nin, out, outStride, o0, f0, accumulate);
    }

    for (; o0 < nout; ++o0)
    {
        GemmTile<1, FT>(weights, nout, bias, inRows, nin, out, outStride, o0, f0, accumulate);
    }
}

// A matrix times a list of input rows, over padded frames (a multiple of 8). A dilated conv
// passes K * C rows, one per tap and channel, each a pointer into a ring.
void Gemm(const float* weights, int nout, const float* bias, const float* const* inRows, int nin, float* out,
          int outStride, int padded, bool accumulate)
{
    int f0 = 0;

    for (; f0 + 8 * kTileVectors <= padded; f0 += 8 * kTileVectors)
    {
        GemmFrames<kTileVectors>(weights, nout, bias, inRows, nin, out, outStride, f0, accumulate);
    }

    if constexpr (kTileVectors > 2)
    {
        for (; f0 + 16 <= padded; f0 += 16)
        {
            GemmFrames<2>(weights, nout, bias, inRows, nin, out, outStride, f0, accumulate);
        }
    }

    for (; f0 < padded; f0 += 8)
    {
        GemmFrames<1>(weights, nout, bias, inRows, nin, out, outStride, f0, accumulate);
    }
}
} // namespace

bool IsAvailable() noexcept
{
#if defined(GUITARFX_FWN_AVX2) || defined(GUITARFX_FWN_SSE)
    return true;
#else
    return false;
#endif
}

const char* KernelName() noexcept
{
#if defined(GUITARFX_FWN_AVX2)
    return "AVX2";
#elif defined(GUITARFX_FWN_SSE)
    return "SSE";
#else
    return "none";
#endif
}

// ---------------------------------------------------------------- rings
void FusedWaveNet::Ring::Allocate(int rows, int lookback, int paddedMax)
{
    size = NextPow2(lookback + paddedMax);
    mask = size - 1;
    rowStride = size + paddedMax;
    history.assign(static_cast<std::size_t>(rows) * static_cast<std::size_t>(rowStride), 0.0f);
    writePos = 0;
}

void FusedWaveNet::Ring::Write(const float* source, int sourceStride, int rows, int frames, int paddedMax)
{
    for (int c = 0; c < rows; ++c)
    {
        float* row = history.data() + static_cast<std::size_t>(c) * rowStride;
        const float* s = source + static_cast<std::size_t>(c) * sourceStride;
        const int first = std::min(frames, size - writePos);
        std::memcpy(row + writePos, s, sizeof(float) * static_cast<std::size_t>(first));

        if (first < frames)
        {
            std::memcpy(row, s + first, sizeof(float) * static_cast<std::size_t>(frames - first));
        }

        // Keep the mirror of the first paddedMax columns current.
        const int lo = first < frames ? 0 : writePos;
        const int hi = first < frames ? frames - first : writePos + frames;

        if (lo < paddedMax)
        {
            std::memcpy(row + size + lo, row + lo,
                        sizeof(float) * static_cast<std::size_t>(std::min(hi, paddedMax) - lo));
        }
    }
}

// Rows for a causal K-tap conv at this dilation: tap k reads (K - 1 - k) * dilation frames back.
void FusedWaveNet::Ring::TapRows(const float** rows, int channels, int kernelSize, int dilation) const
{
    for (int k = 0; k < kernelSize; ++k)
    {
        const int start = (writePos - (kernelSize - 1 - k) * dilation) & mask;

        for (int i = 0; i < channels; ++i)
        {
            rows[k * channels + i] = history.data() + static_cast<std::size_t>(i) * rowStride + start;
        }
    }
}

// Every column of each row set to that row's first source frame: the history of a constant input.
void FusedWaveNet::Ring::Fill(const float* source, int sourceStride, int rows)
{
    for (int c = 0; c < rows; ++c)
    {
        std::fill_n(history.data() + static_cast<std::size_t>(c) * rowStride, rowStride,
                    source[static_cast<std::size_t>(c) * sourceStride]);
    }
}

// ---------------------------------------------------------------- FusedWaveNet
FusedWaveNet::FusedWaveNet(std::shared_ptr<const Weights> weights, double expectedSampleRate)
    : ::nam::DSP(1, 1, expectedSampleRate), mWeights(std::move(weights))
{
    if (!mWeights || mWeights->arrays.empty())
    {
        throw std::invalid_argument("FusedWaveNet: no weights");
    }

    mArrays.resize(mWeights->arrays.size());

    for (std::size_t a = 0; a < mArrays.size(); ++a)
    {
        mArrays[a].layers.resize(mWeights->arrays[a].layers.size());
    }

    UpdateDilations();
}

void FusedWaveNet::UpdateDilations()
{
    // As the NAM core's WaveNet counts it: one sample, plus every layer's and head's lookback.
    mPrewarmSamples = 1;

    for (std::size_t a = 0; a < mArrays.size(); ++a)
    {
        const LayerArray& weights = mWeights->arrays[a];

        for (std::size_t l = 0; l < weights.layers.size(); ++l)
        {
            LayerState& state = mArrays[a].layers[l];
            state.dilation = weights.layers[l].dilation * mTimeScale;
            state.lookback = (weights.layers[l].kernelSize - 1) * state.dilation;
            mPrewarmSamples += state.lookback;
        }

        mPrewarmSamples += (weights.headKernelSize - 1) * mTimeScale;
    }
}

void FusedWaveNet::SetTimeScale(const int scale)
{
    mTimeScale = std::max(1, scale);
    UpdateDilations();

    if (mMaxFrames > 0)
    {
        SetMaxBufferSize(GetMaxBufferSize());
    }
}

void FusedWaveNet::SetMaxBufferSize(const int maxBufferSize)
{
    ::nam::DSP::SetMaxBufferSize(maxBufferSize);
    mMaxFrames = std::clamp(maxBufferSize, 1, kChunkFrames);
    mPaddedMax = RoundUp8(mMaxFrames);
    const auto stride = static_cast<std::size_t>(mPaddedMax);
    mCondition.assign(stride, 0.0f);
    std::size_t maxRows = 1;

    for (std::size_t a = 0; a < mArrays.size(); ++a)
    {
        const LayerArray& weights = mWeights->arrays[a];
        ArrayState& state = mArrays[a];
        const auto channels = static_cast<std::size_t>(weights.channels);
        state.current.assign(channels * stride, 0.0f);
        state.headSum.assign(channels * stride, 0.0f);
        state.conv.assign(channels * stride, 0.0f);
        state.headOut.assign(static_cast<std::size_t>(weights.headSize) * stride, 0.0f);

        for (std::size_t l = 0; l < weights.layers.size(); ++l)
        {
            state.layers[l].ring.Allocate(weights.channels, state.layers[l].lookback, mPaddedMax);
            maxRows = std::max(maxRows, static_cast<std::size_t>(weights.layers[l].kernelSize) * channels);
        }

        if (weights.headKernelSize > 1)
        {
            state.headRing.Allocate(weights.channels, (weights.headKernelSize - 1) * mTimeScale, mPaddedMax);
        }

        maxRows = std::max(maxRows, static_cast<std::size_t>(weights.headKernelSize) * channels);
        maxRows = std::max(maxRows, std::max(channels, static_cast<std::size_t>(weights.inputSize)));
    }

    mRows.assign(maxRows, nullptr);
}

void FusedWaveNet::process(NAM_SAMPLE** input, NAM_SAMPLE** output, const int numFrames)
{
    if (mMaxFrames <= 0)
    {
        // Never prepared. Build() prewarms every model it returns, so this is a caller's bug;
        // silence beats allocating here.
        std::fill_n(output[0], std::max(0, numFrames), static_cast<NAM_SAMPLE>(0));
        return;
    }

    for (int done = 0; done < numFrames;)
    {
        const int frames = std::min(numFrames - done, mMaxFrames);
        ProcessChunk(input[0] + done, output[0] + done, frames, false);
        done += frames;
    }
}

void FusedWaveNet::prewarm()
{
    if (mMaxFrames == 0)
    {
        SetMaxBufferSize(64);
    }

    NAM_SAMPLE zeros[8] = {};
    NAM_SAMPLE sink[8];
    ProcessChunk(zeros, sink, std::min(8, mMaxFrames), true);
}

void FusedWaveNet::PrewarmByRunning()
{
    SetMaxBufferSize(mMaxFrames == 0 ? 64 : GetMaxBufferSize());
    const int block = std::max(GetMaxBufferSize(), 1);
    std::vector<NAM_SAMPLE> zeros(static_cast<std::size_t>(block), static_cast<NAM_SAMPLE>(0));
    std::vector<NAM_SAMPLE> sink(static_cast<std::size_t>(block));
    NAM_SAMPLE* in[1] = {zeros.data()};
    NAM_SAMPLE* out[1] = {sink.data()};

    for (int done = 0; done < mPrewarmSamples; done += block)
    {
        process(in, out, block);
    }
}

std::unique_ptr<::nam::DSP> FusedWaveNet::CloneForPhase() const
{
    auto clone = std::make_unique<FusedWaveNet>(mWeights, mExpectedSampleRate);
    clone->SetTimeScale(mTimeScale);
    return clone;
}

void FusedWaveNet::ProcessChunk(const NAM_SAMPLE* input, NAM_SAMPLE* output, const int frames, const bool prime)
{
    const int padded = RoundUp8(frames);
    const int stride = mPaddedMax;
    const auto rowBytes = static_cast<std::size_t>(stride);

    for (int f = 0; f < frames; ++f)
    {
        mCondition[static_cast<std::size_t>(f)] = static_cast<float>(input[f]);
    }

    std::fill(mCondition.begin() + frames, mCondition.begin() + padded, 0.0f);
    const float* condition = mCondition.data();
    const float** rows = mRows.data();
    const ArrayState* previous = nullptr;

    for (std::size_t a = 0; a < mArrays.size(); ++a)
    {
        const LayerArray& weights = mWeights->arrays[a];
        ArrayState& state = mArrays[a];
        const int channels = weights.channels;

        // Rechannel the input, or the previous array's layer output, into the residual stream.
        if (previous == nullptr)
        {
            rows[0] = condition;
        }
        else
        {
            for (int i = 0; i < weights.inputSize; ++i)
            {
                rows[i] = previous->current.data() + static_cast<std::size_t>(i) * rowBytes;
            }
        }

        Gemm(weights.rechannel.data(), channels, nullptr, rows, weights.inputSize, state.current.data(), stride, padded,
             false);

        // The head accumulator starts at zero, or at the previous array's head output.
        if (previous == nullptr)
        {
            std::fill_n(state.headSum.begin(), static_cast<std::size_t>(channels) * rowBytes, 0.0f);
        }
        else
        {
            std::memcpy(state.headSum.data(), previous->headOut.data(),
                        sizeof(float) * static_cast<std::size_t>(channels) * rowBytes);
        }

        for (std::size_t l = 0; l < weights.layers.size(); ++l)
        {
            const Layer& layer = weights.layers[l];
            LayerState& layerState = state.layers[l];

            if (prime)
            {
                layerState.ring.Fill(state.current.data(), stride, channels);
            }

            layerState.ring.Write(state.current.data(), stride, channels, frames, stride);
            layerState.ring.TapRows(rows, channels, layer.kernelSize, layerState.dilation);
            float* conv = state.conv.data();
            Gemm(layer.conv.data(), channels, layer.convBias.data(), rows, layer.kernelSize * channels, conv, stride,
                 padded, false);

            // Input mixin, activation and head sum, in one pass.
            const V8 slope = Set1(layer.slope);

            for (int o = 0; o < channels; ++o)
            {
                float* z = conv + static_cast<std::size_t>(o) * rowBytes;
                float* h = state.headSum.data() + static_cast<std::size_t>(o) * rowBytes;
                const V8 mix = Set1(layer.mixin[static_cast<std::size_t>(o)]);

                if (layer.activation == Activation::Tanh)
                {
                    for (int f = 0; f < padded; f += 8)
                    {
                        const V8 t = FastTanh(Fma(mix, Load(condition + f), Load(z + f)));
                        Store(z + f, t);
                        Store(h + f, Add(Load(h + f), t));
                    }
                }
                else
                {
                    for (int f = 0; f < padded; f += 8)
                    {
                        const V8 t = LeakyRelu(Fma(mix, Load(condition + f), Load(z + f)), slope);
                        Store(z + f, t);
                        Store(h + f, Add(Load(h + f), t));
                    }
                }
            }

            // layer1x1 residual, in place on the residual stream.
            for (int i = 0; i < channels; ++i)
            {
                rows[i] = conv + static_cast<std::size_t>(i) * rowBytes;
            }

            Gemm(layer.layer1x1.data(), channels, layer.layer1x1Bias.data(), rows, channels, state.current.data(),
                 stride, padded, true);
            layerState.ring.Advance(frames);
        }

        // Head rechannel, channels -> head size: a causal conv when the head has more than one tap.
        if (weights.headKernelSize > 1)
        {
            if (prime)
            {
                state.headRing.Fill(state.headSum.data(), stride, channels);
            }

            state.headRing.Write(state.headSum.data(), stride, channels, frames, stride);
            state.headRing.TapRows(rows, channels, weights.headKernelSize, mTimeScale);
        }
        else
        {
            for (int i = 0; i < channels; ++i)
            {
                rows[i] = state.headSum.data() + static_cast<std::size_t>(i) * rowBytes;
            }
        }

        Gemm(weights.head.data(), weights.headSize, weights.hasHeadBias ? weights.headBias.data() : nullptr, rows,
             weights.headKernelSize * channels, state.headOut.data(), stride, padded, false);

        if (weights.headKernelSize > 1)
        {
            state.headRing.Advance(frames);
        }

        previous = &state;
    }

    const float* y = previous->headOut.data();
    const float headScale = mWeights->headScale;

    for (int f = 0; f < frames; ++f)
    {
        output[f] = static_cast<NAM_SAMPLE>(headScale * y[f]);
    }
}

// ---------------------------------------------------------------- FusedContainer
FusedContainer::FusedContainer(std::vector<Submodel> submodels, const double expectedSampleRate)
    : ::nam::DSP(1, 1, expectedSampleRate), mSubmodels(std::move(submodels))
{
    if (mSubmodels.empty())
    {
        throw std::invalid_argument("FusedContainer: no submodels");
    }

    // Full size, the last submodel, until SetSlimmableSize says otherwise.
    mActiveIndex.store(mSubmodels.size() - 1, std::memory_order_release);
}

void FusedContainer::process(NAM_SAMPLE** input, NAM_SAMPLE** output, const int numFrames)
{
    mSubmodels[mActiveIndex.load(std::memory_order_acquire)].model->process(input, output, numFrames);
}

void FusedContainer::prewarm()
{
    mSubmodels[mActiveIndex.load(std::memory_order_acquire)].model->prewarm();
}

void FusedContainer::SetMaxBufferSize(const int maxBufferSize)
{
    ::nam::DSP::SetMaxBufferSize(maxBufferSize);
    mSubmodels[mActiveIndex.load(std::memory_order_acquire)].model->SetMaxBufferSize(maxBufferSize);
}

void FusedContainer::Reset(const double sampleRate, const int maxBufferSize)
{
    std::lock_guard<std::mutex> lock(mMutex);
    // As the NAM core's container: not through DSP::Reset(), which would prewarm the active
    // submodel before the submodel has these settings.
    mExternalSampleRate = sampleRate;
    mHaveExternalSampleRate = true;
    ::nam::DSP::SetMaxBufferSize(maxBufferSize);
    mSubmodels[mActiveIndex.load(std::memory_order_acquire)].model->Reset(sampleRate, maxBufferSize);
}

void FusedContainer::SetTimeScale(const int scale)
{
    std::lock_guard<std::mutex> lock(mMutex);
    mTimeScale = std::max(1, scale);

    for (auto& submodel : mSubmodels)
    {
        submodel.model->SetTimeScale(mTimeScale);
    }
}

void FusedContainer::SetSlimmableSize(const double value)
{
    std::size_t index = mSubmodels.size() - 1;

    for (std::size_t i = 0; i < mSubmodels.size(); ++i)
    {
        if (value < mSubmodels[i].maxValue)
        {
            index = i;
            break;
        }
    }

    if (index == mActiveIndex.load(std::memory_order_acquire))
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mMutex);

    if (index == mActiveIndex.load(std::memory_order_acquire))
    {
        return;
    }

    // Ready the submodel before it goes on the audio path.
    const double sampleRate = mHaveExternalSampleRate ? mExternalSampleRate : mExpectedSampleRate;
    mSubmodels[index].model->SetTimeScale(mTimeScale);
    mSubmodels[index].model->Reset(sampleRate, GetMaxBufferSize());
    mActiveIndex.store(index, std::memory_order_release);
}

std::unique_ptr<::nam::DSP> FusedContainer::CloneForPhase() const
{
    return mSubmodels[mActiveIndex.load(std::memory_order_acquire)].model->CloneForPhase();
}
} // namespace guitarfx::fusedwavenet
