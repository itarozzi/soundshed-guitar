// The spread engine of RealtimeConvolver: a uniform stage that computes each block's output over
// the samples of the block after it, a slice per callback, instead of all at its boundary. See
// BuildNonUniform in RealtimeConvolver.cpp for where the layout uses it.
#include "RealtimeConvolver.h"
#include "FiniteCheck.h"
#include "SimdFFT.h"

#include <algorithm>

namespace guitarfx
{
namespace
{
// The spread engine's work for one block, in order: build the frame, forward FFT it into the
// delay line, multiply-accumulate every partition, inverse FFT, write the output and clear the
// accumulator for the next block. Each phase is a run of items, done a grain at a time; an
// item's cost is its rough time relative to the others, so the work lands evenly in time.
enum SpreadPhase : size_t
{
    kSpreadFrame,
    kSpreadLoad,
    kSpreadButterflies,
    kSpreadStore,
    kSpreadMultiply,
    kSpreadInverseLoad,
    kSpreadInverseButterflies,
    kSpreadOutput,
    kSpreadClear,
    kSpreadDone
};

// Per-item costs, measured on x64 (AVX2 Release, FFT sizes 512-8192) in units of ~0.07 ns: the
// bit-reversed loads gather, and a butterfly is about twice a multiply-accumulate item. Wrong
// weights put a job's cheap-looking phases in too few callbacks: with them all 1-2 the first
// callback after a 4096-sample boundary did four times its share.
constexpr size_t kSpreadPhaseCost[kSpreadDone] = {6, 12, 9, 2, 4, 12, 9, 4, 1};

// Items per slice. Every phase's length is a multiple of it, and it keeps the slices on the
// even butterflies and 4-aligned multiplies the SIMD paths take whole.
constexpr size_t kSpreadGrain = 64;

size_t SpreadPhaseItems(size_t phase, size_t fftSize, size_t numStages, size_t numPartitions)
{
    switch (phase)
    {
    case kSpreadButterflies:
    case kSpreadInverseButterflies:
        return numStages * (fftSize / 2);
    case kSpreadMultiply:
        return numPartitions * fftSize;
    case kSpreadOutput:
        return fftSize / 2;
    default:
        return fftSize;
    }
}

// The guard every engine puts on its output samples (SanitizeOutput in RealtimeConvolver.cpp).
float SanitizeSpreadOutput(float sample)
{
    return IsFinite(sample) ? std::clamp(sample, -100.0f, 100.0f) : 0.0f;
}
} // namespace

bool RealtimeConvolver::InitSpread()
{
    if (mPartitionSize % kSpreadGrain != 0)
    {
        return false;
    }

    mSpreadCurrentBlock.assign(mPartitionSize, 0.0f);
    mSpreadOutput.assign(mPartitionSize, 0.0f);
    mJobCost = 0;

    for (size_t phase = 0; phase < kSpreadDone; ++phase)
    {
        mJobCost += kSpreadPhaseCost[phase] * SpreadPhaseItems(phase, mFFTSize, mFFT->GetNumStages(), mNumPartitions);
    }

    ResetSpread();
    mSpread = true;
    return true;
}

void RealtimeConvolver::ResetSpread()
{
    // Nothing in flight, so the first boundary plays silence.
    std::fill(mSpreadCurrentBlock.begin(), mSpreadCurrentBlock.end(), 0.0f);
    std::fill(mSpreadOutput.begin(), mSpreadOutput.end(), 0.0f);
    mJobSlot = 0;
    mJobPhase = kSpreadDone;
    mJobPos = 0;
    mJobDone = mJobCost;
}

// The spread engine runs a block's whole convolution -- forward FFT, every partition's
// multiply, inverse FFT -- over the samples of the block after it, a slice per callback in
// proportion to how far that block has filled, and plays the result from the boundary after.
// That makes it a partition later than the boundary engine (ProcessBlock), which BuildNonUniform
// lays out for, and no callback pays for a whole FFT.
void RealtimeConvolver::ProcessSpreadBoundary()
{
    // The block in flight is due now. Normally it is already done; the remainder is whatever
    // the last callback's share fell short by.
    RunSpreadJobTo(mJobCost);
    std::swap(mOutputBuffer, mSpreadOutput);

    // The block that just filled is the new frame's second half, the one before it its first,
    // and the oldest buffer is free to fill. Swapping vectors moves no samples.
    std::swap(mPreviousInputBlock, mSpreadCurrentBlock);
    std::swap(mSpreadCurrentBlock, mInputBuffer);

    // The frame goes to the oldest slot, which the new block is the first not to read.
    mJobSlot = mDelayLineIndex;
    mDelayLineIndex = (mDelayLineIndex + 1) % mNumPartitions;
    mJobPhase = kSpreadFrame;
    mJobPos = 0;
    mJobDone = 0;

    mOutputBufferReadPos = 0;
}

void RealtimeConvolver::RunSpreadJobTo(size_t target)
{
    while (mJobDone < target && mJobPhase < kSpreadDone)
    {
        const size_t cost = kSpreadPhaseCost[mJobPhase];
        const size_t items = SpreadPhaseItems(mJobPhase, mFFTSize, mFFT->GetNumStages(), mNumPartitions);
        const size_t wanted = (target - mJobDone + cost - 1) / cost;
        const size_t slice = (wanted + kSpreadGrain - 1) / kSpreadGrain * kSpreadGrain;
        const size_t end = std::min(items, mJobPos + slice);

        RunSpreadPhase(mJobPhase, mJobPos, end);
        mJobDone += (end - mJobPos) * cost;
        mJobPos = end;

        if (mJobPos == items)
        {
            ++mJobPhase;
            mJobPos = 0;
        }
    }
}

void RealtimeConvolver::RunSpreadPhase(size_t phase, size_t begin, size_t end)
{
    const size_t halfFft = mFFTSize / 2;

    switch (phase)
    {
    case kSpreadFrame:
        // [previous block | current block], the overlap-save frame ProcessBlock() builds.
        for (size_t i = begin; i < end; ++i)
        {
            const float sample = i < mPartitionSize ? mPreviousInputBlock[i] : mSpreadCurrentBlock[i - mPartitionSize];
            mFFTInputBuffer[i] = std::complex<float>(sample, 0.0f);
        }

        break;

    case kSpreadLoad:
        mFFT->LoadBitReversed(mFFTInputBuffer.data(), begin, end);
        break;

    case kSpreadInverseLoad:
        mFFT->LoadBitReversed(mAccumulator.data(), begin, end);
        break;

    case kSpreadButterflies:
    case kSpreadInverseButterflies:
        // Items run stage by stage, halfFft butterflies each.
        for (size_t pos = begin; pos < end;)
        {
            const size_t stage = pos / halfFft;
            const size_t first = pos - stage * halfFft;
            const size_t last = std::min(halfFft, first + (end - pos));
            mFFT->RunButterflies(stage + 1, first, last, phase == kSpreadInverseButterflies);
            pos += last - first;
        }

        break;

    case kSpreadStore:
        std::copy(mFFT->GetResult() + begin, mFFT->GetResult() + end, mInputFFTDelayLine[mJobSlot].begin() + begin);
        break;

    case kSpreadMultiply:
        // Partition p pairs with the frame p blocks back from this one, which is in mJobSlot.
        for (size_t pos = begin; pos < end;)
        {
            const size_t p = pos / mFFTSize;
            const size_t first = pos - p * mFFTSize;
            const size_t count = std::min(mFFTSize - first, end - pos);
            const size_t slot = (mJobSlot + mNumPartitions - p) % mNumPartitions;
            SimdFFT::ComplexMultiplyAccumulate(mAccumulator.data() + first, mInputFFTDelayLine[slot].data() + first,
                                               mIRPartitionsFFT[p].data() + first, count);
            pos += count;
        }

        break;

    case kSpreadOutput: {
        // The second half of the inverse transform is the valid linear convolution.
        const float scale = 1.0f / static_cast<float>(mFFTSize);
        const std::complex<float>* result = mFFT->GetResult() + mPartitionSize;

        for (size_t i = begin; i < end; ++i)
        {
            mSpreadOutput[i] = SanitizeSpreadOutput(result[i].real() * scale);
        }

        break;
    }

    case kSpreadClear:
        SimdFFT::ClearBuffer(mAccumulator.data() + begin, end - begin);
        break;

    default:
        break;
    }
}
} // namespace guitarfx
