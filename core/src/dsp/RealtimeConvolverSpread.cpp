// The spread engine of RealtimeConvolver: a uniform stage that computes each block's output over
// the samples of the block after it, a slice per callback, instead of all at its boundary. See
// BuildNonUniform in RealtimeConvolver.cpp for where the layout uses it.
#include "RealtimeConvolver.h"
#include "FiniteCheck.h"
#include "RealFFT.h"

#include <algorithm>

namespace guitarfx
{
namespace
{
// The spread engine's work for one block, in order: forward FFT the frame into the delay line
// (load it with the first butterfly stage, the other stages, the split pass that stores the
// spectrum), multiply-accumulate every partition, inverse FFT (the split pass undone, every
// stage), write the output and clear the accumulator for the next block. Each phase is a run of
// items, done a grain at a time; an item's cost is its rough time relative to the others, so the
// work lands evenly in time. RealFFT.h has the transform's steps.
enum SpreadPhase : size_t
{
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

// Per-item costs relative to a butterfly's 10, measured 2026-10-19 at real FFT sizes 512-8192 on
// x64 (AVX2 Release) and a Galaxy S21's A78 (NEON, clang -O3 -ffast-math), which agree to within
// a few units: a butterfly is 0.46 ns on x64 and 1.45 ns on the A78, a multiply-accumulate bin
// 0.6 of one, a split-pass pair 1.5 (store) and 2 (load, which scatters). Wrong weights put a
// job's cheap-looking phases in too few callbacks: with them all 1-2 the first callback after a
// 4096-sample boundary did four times its share.
constexpr size_t kSpreadPhaseCost[kSpreadDone] = {12, 10, 15, 6, 19, 10, 3, 1};

// Items per slice. Every phase's length is a multiple of it, and it keeps the slices on the
// even butterflies and 4-aligned multiplies the SIMD paths take whole.
constexpr size_t kSpreadGrain = 64;

// Items in a phase, for a transform of `numBins` complex points (half the real FFT size, and
// the partition size) with `numStages` butterfly stages of numBins / 2 each.
size_t SpreadPhaseItems(size_t phase, size_t numBins, size_t numStages, size_t numPartitions)
{
    switch (phase)
    {
    case kSpreadButterflies:
        return (numStages - 1) * (numBins / 2); // the first stage ran with the load
    case kSpreadInverseButterflies:
        return numStages * (numBins / 2);
    case kSpreadMultiply:
        return numPartitions * numBins;
    case kSpreadLoad:
    case kSpreadStore:
    case kSpreadInverseLoad:
        return numBins / 2; // butterflies, or the split pass's pairs
    default:
        return numBins; // the output's samples, the accumulator's bins
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
    // The shortest phases are half a partition long.
    if (mPartitionSize % (2 * kSpreadGrain) != 0)
    {
        return false;
    }

    mSpreadCurrentBlock.assign(mPartitionSize, 0.0f);
    mSpreadOutput.assign(mPartitionSize, 0.0f);
    mJobCost = 0;

    for (size_t phase = 0; phase < kSpreadDone; ++phase)
    {
        mJobCost += kSpreadPhaseCost[phase] * SpreadPhaseItems(phase, mNumBins, mFFT->GetNumStages(), mNumPartitions);
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
    mJobPhase = kSpreadLoad;
    mJobPos = 0;
    mJobDone = 0;

    mOutputBufferReadPos = 0;
}

void RealtimeConvolver::RunSpreadJobTo(size_t target)
{
    while (mJobDone < target && mJobPhase < kSpreadDone)
    {
        const size_t cost = kSpreadPhaseCost[mJobPhase];
        const size_t items = SpreadPhaseItems(mJobPhase, mNumBins, mFFT->GetNumStages(), mNumPartitions);
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
    const size_t stageItems = mNumBins / 2;

    switch (phase)
    {
    case kSpreadLoad:
        // [previous block | current block], the overlap-save frame ProcessBlock() transforms.
        mFFT->LoadInput(mPreviousInputBlock.data(), mSpreadCurrentBlock.data(), begin, end);
        break;

    case kSpreadInverseLoad:
        mFFT->LoadSpectrum(mAccumulator.data(), begin, end);
        break;

    case kSpreadButterflies:
    case kSpreadInverseButterflies: {
        // Items run stage by stage, stageItems butterflies each, from stage 2 forward (LoadInput
        // ran the first) and stage 1 inverse.
        const bool inverse = phase == kSpreadInverseButterflies;
        const size_t firstStage = inverse ? 1 : 2;

        for (size_t pos = begin; pos < end;)
        {
            const size_t stage = pos / stageItems;
            const size_t first = pos - stage * stageItems;
            const size_t last = std::min(stageItems, first + (end - pos));
            mFFT->RunButterflies(stage + firstStage, first, last, inverse);
            pos += last - first;
        }

        break;
    }

    case kSpreadStore:
        mFFT->StoreSpectrum(mInputFFTDelayLine[mJobSlot].data(), begin, end);
        break;

    case kSpreadMultiply:
        // Partition p pairs with the frame p blocks back from this one, which is in mJobSlot.
        for (size_t pos = begin; pos < end;)
        {
            const size_t p = pos / mNumBins;
            const size_t first = pos - p * mNumBins;
            const size_t count = std::min(mNumBins - first, end - pos);
            const size_t slot = (mJobSlot + mNumPartitions - p) % mNumPartitions;
            std::complex<float>* acc = mAccumulator.data() + first;
            const std::complex<float>* frame = mInputFFTDelayLine[slot].data() + first;
            const std::complex<float>* ir = mIRPartitionsFFT[p].data() + first;

            // Bin 0 is packed (two real bins), so a slice that starts there takes the packed multiply.
            if (first == 0)
            {
                RealFFT::MultiplyAccumulate(acc, frame, ir, count);
            }
            else
            {
                SimdFFT::ComplexMultiplyAccumulate(acc, frame, ir, count);
            }

            pos += count;
        }

        break;

    case kSpreadOutput: {
        // The second half of the inverse transform is the valid linear convolution.
        const float scale = 1.0f / static_cast<float>(mFFTSize);
        const float* result = mFFT->GetResult() + mPartitionSize;

        for (size_t i = begin; i < end; ++i)
        {
            mSpreadOutput[i] = SanitizeSpreadOutput(result[i] * scale);
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
