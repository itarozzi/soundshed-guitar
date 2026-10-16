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

// Tail partition size for the non-uniform engine. That engine has no latency to keep
// down, so the tail is free to use a partition far larger than the host
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

// FIR head length of the zero-latency engine: 256 taps, or 512 once the IR is long enough for a
// 512-sample tail partition. Measured on 480-40,000-sample IRs (docs/plans/zero-latency-ir-cab.md),
// a vectorised FIR of this length costs less than the 64- and 128-sample FFT partitions it
// replaces; of the heads tried (64 to 512), 256 was cheapest up to ~8,000 samples and 512 from
// ~24,000. The switch falls at 16,448 samples, where the tail partition reaches 512.
constexpr size_t kMaxFirHead = 512;

size_t ZeroLatencyHeadForIR(size_t irLength)
{
    return std::min(TailPartitionForIR(irLength), kMaxFirHead);
}

// The same guard ProcessBlock() puts on every FFT output sample, so a bad sample cannot reach
// the recursive effects downstream whichever engine runs.
float SanitizeOutput(float sample)
{
    return IsFinite(sample) ? std::clamp(sample, -100.0f, 100.0f) : 0.0f;
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
        mFirReversed.clear();
        mFirHistory.clear();
        return false;
    }

    // Low latency is zero latency, and owes nothing to the host block: an IR no longer than the
    // FIR head runs on the FIR alone, a longer one on the FIR plus partitions that need no delay.
    const size_t directLimit = mLowLatencyMode ? ZeroLatencyHeadForIR(irSamples.size()) : kDirectConvolutionThreshold;

    if (irSamples.size() <= directLimit)
    {
        BuildFir(irSamples.data(), irSamples.size());
        mUseDirectConvolution = true;
        mInitialized = true;
        return true;
    }

    if (mLowLatencyMode)
    {
        return BuildNonUniform(irSamples);
    }

    // Partition size the uniform engine uses, which doubles as its latency. Low Latency
    // off asks for this trade: fewer, larger partitions cost less on a long IR.
    return BuildUniform(irSamples, static_cast<size_t>(GetMaxLatency(blockSize)));
}

int RealtimeConvolver::GetMaxLatency(int blockSize) noexcept
{
    return static_cast<int>(
        std::clamp(NextPowerOf2(static_cast<size_t>(std::max(blockSize, 256))), size_t{256}, size_t{2048}));
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

// Zero-latency non-uniform (Gardner-style) partitioned convolution.
//
// The IR is split into contiguous segments. The first, h[0, head), runs as a
// direct-form FIR. After it come segments whose partition size grows
// geometrically from `head` up to the tail partition, each an independent
// single-block uniform convolver, and then the long tail as one multi-partition
// uniform engine.
//
// For a segment covering IR samples starting at offset `off` with partition P,
// its single-block convolver produces (h_seg * x)[n - P]. The output needs
// (h_seg * x)[n - off], so the stage wants an extra delay of off - P. The layout
// makes P == off for every segment (each starts where the last ended, at twice
// its offset, and the tail starts at exactly its own partition size), so no stage
// needs any delay, and the FIR plus all the stages is the full convolution with
// no latency. Only the head could not be done this way, since it would need P == 0.
//
// Until 2026-10 the head was an FFT stage at a base block of 64-256 samples
// following the host block, and every other stage went through a shared delay
// line of that length, so the engine ran that many samples late. The FIR costs
// less than those small partitions did.
bool RealtimeConvolver::BuildNonUniform(const std::vector<float>& irSamples)
{
    mInitialized = false;
    mUseDirectConvolution = false;
    mIsNonUniform = false;
    mNonUniformStages.clear();

    const size_t n = irSamples.size();
    const size_t head = ZeroLatencyHeadForIR(n);
    // Never below `head`, which is how ZeroLatencyHeadForIR picks it: the layout relies on
    // the tail starting at exactly its own partition size, which the doubling from `head`
    // can only reach from underneath. Both are powers of two.
    const size_t maxPartition = TailPartitionForIR(n);

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

    // Head segment: the direct-form FIR, no latency.
    BuildFir(irSamples.data(), head);

    // Geometrically growing segments until the partition reaches maxPartition.
    size_t off = head;

    while (off < n && off < maxPartition)
    {
        const size_t partition = off; // P == off: the stage's own latency is its offset

        if (!addStage(off, partition, partition))
        {
            return false;
        }

        off += partition;
    }

    // Efficient tail: one multi-partition uniform engine for the remainder.
    if (off < n)
    {
        // off == maxPartition here, so the tail engine partition is maxPartition and it
        // needs no delay either.
        std::vector<float> tail(irSamples.begin() + static_cast<std::ptrdiff_t>(off), irSamples.end());
        auto stage = std::make_unique<RealtimeConvolver>();
        stage->mLowLatencyMode = false;

        if (!stage->BuildUniform(tail, maxPartition))
        {
            return false;
        }

        mNonUniformStages.push_back(std::move(stage));
    }

    // Aggregate reporting + scratch buffers. GetLatency() reports 0 for this engine.
    mNumPartitions = 0;

    for (const auto& stage : mNonUniformStages)
    {
        mNumPartitions += stage->GetNumPartitions();
    }

    mNuRestScratch.assign(kFirChunk, 0.0f);
    mNuStageScratch.assign(kFirChunk, 0.0f);

    mIsNonUniform = true;
    mInitialized = true;
    return true;
}

void RealtimeConvolver::BuildFir(const float* coefficients, size_t count)
{
    mFirReversed.assign(count, 0.0f);

    for (size_t k = 0; k < count; ++k)
    {
        mFirReversed[count - 1 - k] = coefficients[k];
    }

    mFirHistory.assign(count - 1 + kFirChunk, 0.0f);
}

void RealtimeConvolver::ProcessFir(const float* input, float* output, size_t numSamples)
{
    const size_t taps = mFirReversed.size();
    const float* coefficients = mFirReversed.data();
    float* history = mFirHistory.data();
    size_t done = 0;

    while (done < numSamples)
    {
        const size_t chunk = std::min(numSamples - done, kFirChunk);
        const float* in = input + done;
        float* out = output + done;

        // The chunk goes in behind the history before any output is written, so the output may
        // be the input buffer. A non-finite sample goes in as silence: in the history it would
        // poison the next `taps` outputs.
        float* fresh = history + taps - 1;

        for (size_t i = 0; i < chunk; ++i)
        {
            fresh[i] = IsFinite(in[i]) ? in[i] : 0.0f;
        }

        // y[i] = sum_k h[k] x[i - k], which with the coefficients reversed is the dot product of
        // them with history[i, i + taps). Four outputs share each coefficient load; the compiler
        // vectorises each sum over k (Release builds are fast-math, so it may reorder them).
        size_t i = 0;

        for (; i + 4 <= chunk; i += 4)
        {
            const float* x = history + i;
            float s0 = 0.0f;
            float s1 = 0.0f;
            float s2 = 0.0f;
            float s3 = 0.0f;

            for (size_t k = 0; k < taps; ++k)
            {
                const float c = coefficients[k];
                s0 += c * x[k];
                s1 += c * x[k + 1];
                s2 += c * x[k + 2];
                s3 += c * x[k + 3];
            }

            out[i] = s0;
            out[i + 1] = s1;
            out[i + 2] = s2;
            out[i + 3] = s3;
        }

        for (; i < chunk; ++i)
        {
            const float* x = history + i;
            float s = 0.0f;

            for (size_t k = 0; k < taps; ++k)
            {
                s += coefficients[k] * x[k];
            }

            out[i] = s;
        }

        // Keep the newest taps - 1 samples as the next chunk's history.
        std::memmove(history, history + chunk, (taps - 1) * sizeof(float));
        done += chunk;
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
        ProcessFir(input, output, static_cast<size_t>(numSamples));

        for (int k = 0; k < numSamples; ++k)
        {
            output[k] = SanitizeOutput(output[k]);
        }

        return;
    }

    // Zero-latency non-uniform (Gardner-style) engine.
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
    int processed = 0;

    while (processed < numSamples)
    {
        const size_t chunk = std::min(static_cast<size_t>(numSamples - processed), kFirChunk);
        const float* in = input + processed;
        float* out = output + processed;

        // The stages read the input before the FIR writes any output, so the output may be the
        // input buffer.
        std::fill_n(mNuRestScratch.data(), chunk, 0.0f);

        for (auto& stage : mNonUniformStages)
        {
            stage->Process(in, mNuStageScratch.data(), static_cast<int>(chunk));

            for (size_t k = 0; k < chunk; ++k)
            {
                mNuRestScratch[k] += mNuStageScratch[k];
            }
        }

        // Every stage is already time-aligned (see BuildNonUniform), so they add straight onto
        // the head.
        ProcessFir(in, out, chunk);

        for (size_t k = 0; k < chunk; ++k)
        {
            out[k] = SanitizeOutput(out[k] + mNuRestScratch[k]);
        }

        processed += static_cast<int>(chunk);
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
        std::fill(mFirHistory.begin(), mFirHistory.end(), 0.0f);
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

        std::fill(mFirHistory.begin(), mFirHistory.end(), 0.0f);
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
