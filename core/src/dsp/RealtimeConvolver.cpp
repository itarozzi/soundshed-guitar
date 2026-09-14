#include "RealtimeConvolver.h"
#include "FiniteCheck.h"
#include "SimdFFT.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace guitarfx
{
namespace
{
constexpr size_t NextPowerOf2(size_t n)
{
    size_t power = 1;

    while (power < n)
    {
        power *= 2;
    }

    return power;
}

// Tail partition size for the non-uniform engine. That engine's latency is pinned to
// its base block, so the tail is free to use a partition far larger than the host
// block -- and it has to be, because a partitioned convolution's per-sample cost is
// proportional to the partition COUNT. A 4.7 s plate reverb split at 256 samples is
// ~880 partitions and streams 28 KB of spectra per output sample, which is DRAM-bound;
// at 4096 it is ~55 partitions and 1.8 KB. Size the tail so the count stays near the
// target rather than tying it to the host block, which has nothing to do with the IR.
constexpr size_t kTargetTailPartitions = 64;

size_t TailPartitionForIR(size_t irLength)
{
    return std::clamp(NextPowerOf2(irLength / kTargetTailPartitions), size_t{256}, size_t{4096});
}
} // namespace

RealtimeConvolver::RealtimeConvolver() = default;

RealtimeConvolver::~RealtimeConvolver() = default;

RealtimeConvolver::RealtimeConvolver(RealtimeConvolver&&) noexcept = default;

RealtimeConvolver& RealtimeConvolver::operator=(RealtimeConvolver&&) noexcept = default;

bool RealtimeConvolver::SetImpulse(const std::vector<float>& irSamples, int blockSize)
{
    mInitialized = false;
    mUseDirectConvolution = false;
    mIsNonUniform = false;
    mNonUniformStages.clear();

    if (irSamples.empty() || blockSize <= 0)
    {
        mIRPartitionsFFT.clear();
        mInputFFTDelayLine.clear();
        mFFT.reset();
        mDirectIR.clear();
        mDirectHistory.clear();
        return false;
    }

    // For very short IRs, use direct convolution (zero latency)
    if (irSamples.size() <= kDirectConvolutionThreshold)
    {
        mDirectIR = irSamples;
        mDirectHistory.assign(irSamples.size(), 0.0f);
        mDirectHistoryPos = 0;
        mUseDirectConvolution = true;
        mInitialized = true;
        return true;
    }

    // Partition size the uniform engine would use. It doubles as that engine's
    // latency, which is why it stays tied to the host block.
    const size_t uniformPartition =
        std::clamp(NextPowerOf2(static_cast<size_t>(std::max(blockSize, 256))), size_t{256}, size_t{2048});

    if (mLowLatencyMode)
    {
        // Base block = latency target for the non-uniform engine. Capped so it never
        // exceeds the uniform partition (otherwise non-uniform gives no benefit).
        const size_t base =
            std::clamp(NextPowerOf2(static_cast<size_t>(std::max(blockSize, 64))), size_t{64}, size_t{256});

        // Non-uniform pays off whenever its tail partition beats the base block: for
        // latency when the base is below the uniform partition, and -- since a long IR's
        // tail partition now outgrows the uniform one -- for CPU as well.
        if (base < TailPartitionForIR(irSamples.size()))
        {
            return BuildNonUniform(irSamples, blockSize);
        }

        // No benefit available (short IR and a host block already >= uniform partition):
        // fall through to the uniform engine.
    }

    return BuildUniform(irSamples, uniformPartition);
}

bool RealtimeConvolver::BuildUniform(const std::vector<float>& irSamples, size_t partitionSize)
{
    mInitialized = false;
    mUseDirectConvolution = false;
    mIsNonUniform = false;

    if (irSamples.empty() || partitionSize == 0)
    {
        return false;
    }

    mPartitionSize = partitionSize;

    // FFT size is 2x partition for linear convolution
    mFFTSize = mPartitionSize * 2;

    // Calculate partitions needed
    mNumPartitions = (irSamples.size() + mPartitionSize - 1) / mPartitionSize;

    // Create FFT plan
    try
    {
        mFFT = std::make_unique<SimdFFT>(mFFTSize);
    }
    catch (...)
    {
        return false;
    }

    // Allocate all working buffers ONCE
    mFFTInputBuffer.resize(mFFTSize);
    mFFTOutputBuffer.resize(mFFTSize);
    mAccumulator.resize(mFFTSize);

    // Pre-compute FFT of each IR partition
    mIRPartitionsFFT.resize(mNumPartitions);

    for (size_t p = 0; p < mNumPartitions; ++p)
    {
        // Clear input buffer
        std::fill(mFFTInputBuffer.begin(), mFFTInputBuffer.end(), std::complex<float>(0.0f, 0.0f));

        // Copy IR partition into FIRST half
        const size_t irStart = p * mPartitionSize;
        const size_t irEnd = std::min(irStart + mPartitionSize, irSamples.size());

        for (size_t i = irStart; i < irEnd; ++i)
        {
            mFFTInputBuffer[i - irStart] = std::complex<float>(irSamples[i], 0.0f);
        }

        // Compute and store FFT
        mIRPartitionsFFT[p].resize(mFFTSize);
        mFFT->Forward(mIRPartitionsFFT[p].data(), mFFTInputBuffer.data());
    }

    // Allocate delay line for input FFTs
    mInputFFTDelayLine.resize(mNumPartitions);

    for (auto& fft : mInputFFTDelayLine)
    {
        fft.assign(mFFTSize, std::complex<float>(0.0f, 0.0f));
    }

    mDelayLineIndex = 0;

    // Allocate I/O buffers
    mInputBuffer.assign(mPartitionSize, 0.0f);
    mOutputBuffer.assign(mPartitionSize, 0.0f);
    mInputBufferPos = 0;
    mOutputBufferReadPos = mPartitionSize; // Start empty

    // Previous input block for overlap-save (needed for proper convolution)
    mPreviousInputBlock.assign(mPartitionSize, 0.0f);

    // The accumulator is carried across the block now, so it starts cleared with
    // nothing scheduled: the first ProcessBlock() has no older frames to fold in.
    std::fill(mAccumulator.begin(), mAccumulator.end(), std::complex<float>(0.0f, 0.0f));
    mPreAccumAnchor = 0;
    mPreAccumPartition = mNumPartitions;

    mInitialized = true;
    return true;
}

// Non-uniform (Gardner-style) partitioned convolution.
//
// The IR is split into contiguous segments whose partition size grows
// geometrically from a small "base" block (low latency) up to the uniform
// partition (efficient). Each segment is an independent single-block uniform
// convolver, except the long tail which is one multi-partition uniform engine.
//
// For a segment covering IR samples starting at offset `off` with partition P,
// its single-block convolver produces (h_seg * x)[n - P]. To contribute the
// correctly time-aligned (h_seg * x)[n - off] to an output emitted with system
// latency L, the stage output is delayed by d = L + off - P. With L = base and
// the chosen layout (P == off for every stage after the head, and the tail
// starting at exactly its own partition size), d == base for ALL stages except
// the head (off=0, P=base, d=0). So a single shared delay line of `base`
// samples aligns every non-head stage. Summing all stages reconstructs the
// full convolution delayed by `base` samples.
bool RealtimeConvolver::BuildNonUniform(const std::vector<float>& irSamples, int blockSize)
{
    mInitialized = false;
    mUseDirectConvolution = false;
    mIsNonUniform = false;
    mNonUniformStages.clear();

    const size_t n = irSamples.size();
    const size_t base = std::clamp(NextPowerOf2(static_cast<size_t>(std::max(blockSize, 64))), size_t{64}, size_t{256});
    // Never below `base`: the layout below relies on the tail starting at exactly its own
    // partition size, which the geometric growth from `base` can only reach from underneath.
    const size_t maxPartition = std::max(TailPartitionForIR(n), base);

    // Build a single-block uniform stage covering irSamples[off, off+len).
    auto addStage = [&](size_t off, size_t len, size_t partition) -> bool {
        auto stage = std::make_unique<RealtimeConvolver>();
        stage->mLowLatencyMode = false;
        std::vector<float> slice(irSamples.begin() + static_cast<std::ptrdiff_t>(off),
                                 irSamples.begin() + static_cast<std::ptrdiff_t>(std::min(off + len, n)));

        if (!stage->BuildUniform(slice, partition))
        {
            return false;
        }

        mNonUniformStages.push_back(std::move(stage));
        return true;
    };

    // Head segment: low-latency base partition, no extra delay.
    if (!addStage(0, base, base))
    {
        return false;
    }

    // Geometrically growing head segments until the partition reaches maxPartition.
    size_t off = base;

    while (off < n && off < maxPartition)
    {
        const size_t partition = off; // P == off keeps every stage delay-aligned to `base`

        if (!addStage(off, partition, partition))
        {
            return false;
        }

        off += partition;
    }

    // Efficient tail: one multi-partition uniform engine for the remainder.
    if (off < n)
    {
        // off == maxPartition here, so the tail engine partition is maxPartition and
        // its required delay also reduces to `base`.
        std::vector<float> tail(irSamples.begin() + static_cast<std::ptrdiff_t>(off), irSamples.end());
        auto stage = std::make_unique<RealtimeConvolver>();
        stage->mLowLatencyMode = false;

        if (!stage->BuildUniform(tail, maxPartition))
        {
            return false;
        }

        mNonUniformStages.push_back(std::move(stage));
    }

    // Aggregate reporting + shared delay/scratch buffers.
    mBaseBlock = base;
    mPartitionSize = base; // GetLatency() reports the base block as the engine latency
    mNumPartitions = 0;

    for (const auto& stage : mNonUniformStages)
    {
        mNumPartitions += stage->GetNumPartitions();
    }

    mNuScratchSize = std::max<size_t>(static_cast<size_t>(std::max(blockSize, 1)), maxPartition);
    mNuHeadScratch.assign(mNuScratchSize, 0.0f);
    mNuRestScratch.assign(mNuScratchSize, 0.0f);
    mNuStageScratch.assign(mNuScratchSize, 0.0f);
    mNuDelayLine.assign(base, 0.0f);
    mNuDelayPos = 0;

    mIsNonUniform = true;
    mInitialized = true;
    return true;
}

void RealtimeConvolver::ProcessDirect(const float* input, float* output, int numSamples)
{
    const size_t irLen = mDirectIR.size();

    for (int i = 0; i < numSamples; ++i)
    {
        // Store new sample in history
        mDirectHistory[mDirectHistoryPos] = input[i];

        // Direct FIR convolution: output[n] = sum(input[n-k] * ir[k])
        float sum = 0.0f;

        for (size_t k = 0; k < irLen; ++k)
        {
            // Calculate index into circular history buffer
            size_t histIdx = (mDirectHistoryPos + irLen - k) % irLen;
            sum += mDirectHistory[histIdx] * mDirectIR[k];
        }

        output[i] = sum;

        // Advance history position
        mDirectHistoryPos = (mDirectHistoryPos + 1) % irLen;
    }
}

// Fold IR partitions into the accumulator up to (but not including) `target`.
//
// Partition p pairs the IR's p-th slice with the input frame from p blocks back, so
// every partition except p == 0 uses a frame that has been sitting in the delay line
// since before the current block started. Only p == 0 has to wait for the boundary.
// Process() therefore walks this forward a slice at a time as the block fills, which
// turns "one 880-multiply burst every partition" into the same work spread evenly over
// the callbacks that produce it. With a 64-sample host block against a multi-second IR
// the burst was several times the callback deadline; the average was never the problem.
void RealtimeConvolver::AccumulatePartitionsUpTo(size_t target)
{
    const size_t limit = std::min(target, mNumPartitions);

    while (mPreAccumPartition < limit)
    {
        const size_t p = mPreAccumPartition;

        // Measured from the newest frame as of this block's start, partition p's frame
        // is (p - 1) slots older.
        const size_t delayIdx = (mPreAccumAnchor + mNumPartitions - (p - 1)) % mNumPartitions;
        SimdFFT::ComplexMultiplyAccumulate(mAccumulator.data(), mInputFFTDelayLine[delayIdx].data(),
                                           mIRPartitionsFFT[p].data(), mFFTSize);
        ++mPreAccumPartition;
    }
}

void RealtimeConvolver::ProcessBlock()
{
    // Everything but partition 0 should already be in the accumulator. Finish whatever
    // the schedule has not reached: the very first block, or a host feeding us fewer
    // samples than it takes to walk the whole partition list.
    AccumulatePartitionsUpTo(mNumPartitions);

    // Prepare FFT input: [previous samples | current samples]
    // This is the correct overlap-save arrangement for linear convolution
    for (size_t i = 0; i < mPartitionSize; ++i)
    {
        mFFTInputBuffer[i] = std::complex<float>(static_cast<float>(mPreviousInputBlock[i]), 0.0f);
        mFFTInputBuffer[mPartitionSize + i] = std::complex<float>(static_cast<float>(mInputBuffer[i]), 0.0f);
    }

    // Save current input for next block
    std::copy(mInputBuffer.begin(), mInputBuffer.end(), mPreviousInputBlock.begin());

    // Forward FFT of input block
    mFFT->Forward(mFFTOutputBuffer.data(), mFFTInputBuffer.data());

    // Store in delay line. This is the slot the accumulation pass never reads, so the
    // frame it replaces (the oldest) is already spent.
    auto& currentSlot = mInputFFTDelayLine[mDelayLineIndex];
    std::copy(mFFTOutputBuffer.begin(), mFFTOutputBuffer.end(), currentSlot.begin());

    // Partition 0 is the only one that needs the frame that just closed.
    SimdFFT::ComplexMultiplyAccumulate(mAccumulator.data(), currentSlot.data(), mIRPartitionsFFT[0].data(), mFFTSize);

    // Inverse FFT
    mFFT->Inverse(mFFTInputBuffer.data(), mAccumulator.data());

    // Overlap-Save output extraction:
    // The first N samples contain circular convolution artifacts - DISCARD them
    // The last N samples are the valid linear convolution result - KEEP them
    const float scale = 1.0f / static_cast<float>(mFFTSize);

    for (size_t i = 0; i < mPartitionSize; ++i)
    {
        // Keep only the SECOND half of the IFFT output (valid linear convolution)
        float sample = mFFTInputBuffer[mPartitionSize + i].real() * scale;

        // Safety: clamp to reasonable audio range to prevent numeric instability
        if (!IsFinite(sample))
        {
            sample = 0.0f;
        }
        else
        {
            sample = std::clamp(sample, -100.0f, 100.0f);
        }

        mOutputBuffer[i] = sample;
    }

    // Open the next block's accumulation against the frame just stored, and hand the
    // write cursor on to the slot that frame will displace.
    SimdFFT::ClearBuffer(mAccumulator.data(), mFFTSize);
    mPreAccumAnchor = mDelayLineIndex;
    mPreAccumPartition = 1;
    mDelayLineIndex = (mDelayLineIndex + 1) % mNumPartitions;

    mOutputBufferReadPos = 0;
}

void RealtimeConvolver::Process(const float* input, float* output, int numSamples)
{
    if (!mInitialized || !input || !output || numSamples <= 0)
    {
        if (output && numSamples > 0)
        {
            std::memset(output, 0, static_cast<size_t>(numSamples) * sizeof(float));
        }

        return;
    }

    // Use direct convolution for short IRs (zero latency)
    if (mUseDirectConvolution)
    {
        ProcessDirect(input, output, numSamples);
        return;
    }

    // Non-uniform (Gardner-style) engine.
    if (mIsNonUniform)
    {
        ProcessNonUniform(input, output, numSamples);
        return;
    }

    int i = 0;

    while (i < numSamples)
    {
        // If output buffer exhausted, process next block
        if (mOutputBufferReadPos >= mPartitionSize)
        {
            ProcessBlock();
        }

        // Output available samples
        // Key insight: We SYNCHRONIZE input/output buffer positions
        // When we read sample N from output, we write sample N to input for next block
        while (mOutputBufferReadPos < mPartitionSize && i < numSamples)
        {
            const size_t bufferPos = mOutputBufferReadPos;
            output[i] = mOutputBuffer[bufferPos];
            mInputBuffer[bufferPos] = input[i];
            ++mOutputBufferReadPos;
            ++i;
        }

        // Keep the accumulation as far through the partition list as the block is
        // through its samples, so the cost lands evenly instead of all at the boundary.
        if (mNumPartitions > 1)
        {
            AccumulatePartitionsUpTo(1 + ((mNumPartitions - 1) * mOutputBufferReadPos) / mPartitionSize);
        }
    }
}

void RealtimeConvolver::ProcessNonUniform(const float* input, float* output, int numSamples)
{
    const size_t numStages = mNonUniformStages.size();
    int processed = 0;

    while (processed < numSamples)
    {
        const int chunk =
            static_cast<int>(std::min<size_t>(static_cast<size_t>(numSamples - processed), mNuScratchSize));
        const float* in = input + processed;
        float* out = output + processed;

        // Stage 0 (head): contributes with no extra delay.
        mNonUniformStages[0]->Process(in, mNuHeadScratch.data(), chunk);

        // Remaining stages accumulate into mNuRestScratch.
        std::fill_n(mNuRestScratch.data(), static_cast<size_t>(chunk), 0.0f);

        for (size_t s = 1; s < numStages; ++s)
        {
            mNonUniformStages[s]->Process(in, mNuStageScratch.data(), chunk);

            for (int k = 0; k < chunk; ++k)
            {
                mNuRestScratch[static_cast<size_t>(k)] += mNuStageScratch[static_cast<size_t>(k)];
            }
        }

        // Delay the accumulated tail by exactly mBaseBlock samples and add the head.
        if (mBaseBlock == 0)
        {
            for (int k = 0; k < chunk; ++k)
            {
                out[k] = mNuHeadScratch[static_cast<size_t>(k)] + mNuRestScratch[static_cast<size_t>(k)];
            }
        }
        else
        {
            for (int k = 0; k < chunk; ++k)
            {
                const float delayed = mNuDelayLine[mNuDelayPos];
                mNuDelayLine[mNuDelayPos] = mNuRestScratch[static_cast<size_t>(k)];
                mNuDelayPos = (mNuDelayPos + 1) % mBaseBlock;
                out[k] = mNuHeadScratch[static_cast<size_t>(k)] + delayed;
            }
        }

        processed += chunk;
    }
}

void RealtimeConvolver::Reset()
{
    if (!mInitialized)
    {
        return;
    }

    // Reset direct convolution state
    if (mUseDirectConvolution)
    {
        std::fill(mDirectHistory.begin(), mDirectHistory.end(), 0.0f);
        mDirectHistoryPos = 0;
        return;
    }

    // Reset non-uniform engine state
    if (mIsNonUniform)
    {
        for (auto& stage : mNonUniformStages)
        {
            if (stage)
            {
                stage->Reset();
            }
        }

        std::fill(mNuDelayLine.begin(), mNuDelayLine.end(), 0.0f);
        mNuDelayPos = 0;
        std::fill(mNuHeadScratch.begin(), mNuHeadScratch.end(), 0.0f);
        std::fill(mNuRestScratch.begin(), mNuRestScratch.end(), 0.0f);
        std::fill(mNuStageScratch.begin(), mNuStageScratch.end(), 0.0f);
        return;
    }

    // Clear delay line
    for (auto& fft : mInputFFTDelayLine)
    {
        std::fill(fft.begin(), fft.end(), std::complex<float>(0.0f, 0.0f));
    }

    mDelayLineIndex = 0;

    // Drop the part-built accumulation along with the frames it was built from.
    std::fill(mAccumulator.begin(), mAccumulator.end(), std::complex<float>(0.0f, 0.0f));
    mPreAccumAnchor = 0;
    mPreAccumPartition = mNumPartitions;

    // Clear buffers
    std::fill(mInputBuffer.begin(), mInputBuffer.end(), 0.0f);
    std::fill(mOutputBuffer.begin(), mOutputBuffer.end(), 0.0f);
    std::fill(mPreviousInputBlock.begin(), mPreviousInputBlock.end(), 0.0f);
    mInputBufferPos = 0;
    mOutputBufferReadPos = mPartitionSize;
}
} // namespace guitarfx
