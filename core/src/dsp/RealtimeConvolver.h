#pragma once

#include <vector>
#include <complex>
#include <memory>
#include <cstddef>

namespace guitarfx
{
class SimdFFT;
}

namespace guitarfx
{
/**
 * Real-time convolver using Uniformly Partitioned Overlap-Save (UPOLS) algorithm.
 *
 * Optimized for low-latency real-time audio:
 * - Fixed-size FFT blocks regardless of IR length
 * - All buffers pre-allocated (zero allocations in audio thread)
 * - Partitioned convolution for O(B log B) per-block complexity
 * - Latency = partition size (typically 256-512 samples)
 *
 * Optional zero-latency, non-uniform (Gardner-style) partitioning, enabled via
 * SetLowLatencyMode(true) before SetImpulse(). In this mode the first 256 or 512
 * IR samples run as a direct-form FIR, and the rest as FFT partitions that grow
 * with their offset into the IR, so the output has no latency at all. It is also
 * cheaper than small uniform partitions would be at a small host block, and the
 * big tail partition makes a long IR cheaper than the uniform engine. An IR no
 * longer than the FIR runs on the FIR alone. See RealtimeConvolver.cpp.
 *
 * The FFT engine spreads a block's partition multiplies across the samples of
 * that block rather than doing them all at the block boundary, so per-callback
 * cost stays flat even when the host block is much smaller than the partition.
 */
class RealtimeConvolver
{
  public:
    RealtimeConvolver();
    ~RealtimeConvolver();

    // Non-copyable, movable
    RealtimeConvolver(const RealtimeConvolver&) = delete;
    RealtimeConvolver& operator=(const RealtimeConvolver&) = delete;
    RealtimeConvolver(RealtimeConvolver&&) noexcept;
    RealtimeConvolver& operator=(RealtimeConvolver&&) noexcept;

    /**
     * Initialize with impulse response.
     * @param irSamples The impulse response samples
     * @param blockSize The most samples the host promised per call, which may be far more
     *                  than it sends. Low-latency mode ignores it: its latency is 0 whatever
     *                  the block. Otherwise the partition, and so the latency, follows it
     *                  from 256 up to 2048.
     * @return true if initialization succeeded
     */
    bool SetImpulse(const std::vector<float>& irSamples, int blockSize);

    /**
     * Enable/disable the zero-latency engine (FIR head plus non-uniform partitions).
     * Must be called BEFORE SetImpulse() to take effect; the mode is captured at
     * SetImpulse() time alongside the partition layout. Defaults to false (uniform).
     */
    void SetLowLatencyMode(bool enabled) noexcept
    {
        mLowLatencyMode = enabled;
    }

    [[nodiscard]] bool IsLowLatencyMode() const noexcept
    {
        return mLowLatencyMode;
    }

    /**
     * Process audio samples through the convolver.
     * ZERO heap allocations in this path - all buffers pre-allocated.
     */
    void Process(const float* input, float* output, int numSamples);

    /**
     * Clears the history (input, output and delay-line buffers) and keeps the impulse, so
     * IsInitialized() stays true and the next Process() still convolves. To drop the impulse,
     * assign a default-constructed convolver.
     */
    void Reset();

    [[nodiscard]] bool IsInitialized() const noexcept
    {
        return mInitialized;
    }

    [[nodiscard]] int GetLatency() const noexcept
    {
        return (mUseDirectConvolution || mIsNonUniform) ? 0 : static_cast<int>(mPartitionSize);
    }

    [[nodiscard]] size_t GetNumPartitions() const noexcept
    {
        return mNumPartitions;
    }

  private:
    void ProcessBlock();

    // Direct-form FIR over `count` coefficients: the whole IR when it is short enough, or the
    // zero-latency engine's head. ProcessFir writes (not adds) its output.
    void BuildFir(const float* coefficients, size_t count);
    void ProcessFir(const float* input, float* output, size_t numSamples);

    // Folds IR partitions [mPreAccumPartition, target) into mAccumulator for the block
    // that is still filling. Called a slice at a time from Process() so a long IR costs
    // the same on every callback. See RealtimeConvolver.cpp.
    void AccumulatePartitionsUpTo(size_t target);

    // Builds a uniformly-partitioned overlap-save engine with an explicit
    // partition size (power of two). Used both for the default uniform path and
    // for each stage of the non-uniform engine.
    bool BuildUniform(const std::vector<float>& irSamples, size_t partitionSize);

    // Builds and processes the zero-latency non-uniform (Gardner-style) engine.
    bool BuildNonUniform(const std::vector<float>& irSamples);
    void ProcessNonUniform(const float* input, float* output, int numSamples);

    bool mInitialized = false;
    bool mUseDirectConvolution = false; // The whole IR runs on the FIR (no latency)
    bool mLowLatencyMode = false;       // Requested at SetImpulse time (zero-latency engine)
    bool mIsNonUniform = false;         // True when the zero-latency non-uniform engine is active

    // Direct-form FIR. Uniform mode uses it alone for IRs up to kDirectConvolutionThreshold;
    // low-latency mode for IRs up to its head length, and as the head of longer ones.
    std::vector<float> mFirReversed; // h[count - 1] .. h[0], so each output is one dot product
    std::vector<float> mFirHistory;  // count - 1 samples of history, then one chunk of input
    static constexpr size_t kFirChunk = 256;
    static constexpr size_t kDirectConvolutionThreshold = 64;

    // Partition configuration
    size_t mPartitionSize = 0;
    size_t mFFTSize = 0;
    size_t mNumPartitions = 0;

    // IR in frequency domain (pre-computed at SetImpulse time)
    std::vector<std::vector<std::complex<float>>> mIRPartitionsFFT;

    // Input delay line (frequency domain) - circular buffer
    std::vector<std::vector<std::complex<float>>> mInputFFTDelayLine;
    size_t mDelayLineIndex = 0;

    // Input/output sample buffering
    std::vector<float> mInputBuffer;
    std::vector<float> mOutputBuffer;
    std::vector<float> mPreviousInputBlock; // Previous block for overlap-save
    size_t mInputBufferPos = 0;
    size_t mOutputBufferReadPos = 0;

    // Pre-allocated working buffers (NO allocations in ProcessBlock)
    std::vector<std::complex<float>> mFFTInputBuffer;  // Input to FFT
    std::vector<std::complex<float>> mFFTOutputBuffer; // Output from FFT
    std::vector<std::complex<float>> mAccumulator;     // Freq domain accumulator

    // Accumulation schedule. mAccumulator is built up across the block rather than in
    // one burst at the block boundary: mPreAccumAnchor is the delay-line slot holding
    // the newest input frame at the start of the current block, and mPreAccumPartition
    // is the next IR partition still to be folded in.
    size_t mPreAccumAnchor = 0;
    size_t mPreAccumPartition = 0;

    // FFT plan
    std::unique_ptr<SimdFFT> mFFT;

    // --- Zero-latency non-uniform (Gardner-style) engine ---
    // The FIR above covers the IR's head. Each stage here is a uniform engine for the IR
    // segment starting at the offset equal to its own partition, so its block latency is
    // exactly the delay that segment needs, and the stages add straight into the FIR's
    // output. See RealtimeConvolver.cpp.
    std::vector<std::unique_ptr<RealtimeConvolver>> mNonUniformStages;
    std::vector<float> mNuRestScratch;  // accumulated output of the stages
    std::vector<float> mNuStageScratch; // per-stage temporary
};
} // namespace guitarfx
