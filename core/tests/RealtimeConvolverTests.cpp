/**
 * @file RealtimeConvolverTests.cpp
 * @brief Unit tests for the RealtimeConvolver: zero-latency non-uniform
 *        (Gardner-style) partitioning.
 *
 * The tests verify that:
 *  - The impulse response is the IR itself from the first sample, for IRs on
 *    either side of every FIR/partition boundary and at odd block sizes.
 *  - The output matches a direct reference convolution, sample for sample.
 *  - Reset() clears state so a subsequent run is identical to a fresh one.
 *  - A multi-second IR is split into a bounded number of partitions rather than
 *    one per host block, so its per-sample cost does not scale with IR length.
 *  - The partition accumulation, which is spread across the samples of a block
 *    rather than done in one burst at the block boundary, still completes when
 *    the host feeds irregular chunk sizes.
 *  - Processing in place (output buffer == input buffer) gives the same output.
 *  - A long IR's spread stages, which compute each block over the next block's
 *    samples, finish every block in time under ragged chunks and reset cleanly.
 */

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <random>
#include <vector>

#include "dsp/RealtimeConvolver.h"

#ifndef M_PI
    #define M_PI 3.14159265358979323846
#endif

namespace
{
// Run a convolver over the input, feeding it in fixed-size blocks to mimic a
// real host. Returns an output vector the same length as the input.
std::vector<double> RunConvolver(guitarfx::RealtimeConvolver& conv, const std::vector<float>& input, int blockSize)
{
    std::vector<double> output(input.size(), 0.0);
    std::vector<float> inBlock(static_cast<std::size_t>(blockSize));
    std::vector<float> outBlock(static_cast<std::size_t>(blockSize));

    std::size_t pos = 0;

    while (pos < input.size())
    {
        const int n = static_cast<int>(std::min<std::size_t>(blockSize, input.size() - pos));

        for (int i = 0; i < n; ++i)
        {
            inBlock[static_cast<std::size_t>(i)] = input[pos + static_cast<std::size_t>(i)];
        }

        conv.Process(inBlock.data(), outBlock.data(), n);

        for (int i = 0; i < n; ++i)
        {
            output[pos + static_cast<std::size_t>(i)] = static_cast<double>(outBlock[static_cast<std::size_t>(i)]);
        }

        pos += static_cast<std::size_t>(n);
    }

    return output;
}

// Direct (time-domain) reference convolution: ref[n] = sum_k input[n-k]*ir[k].
std::vector<double> ReferenceConvolve(const std::vector<float>& input, const std::vector<float>& ir)
{
    std::vector<double> ref(input.size(), 0.0);
    const std::size_t irLen = ir.size();

    for (std::size_t n = 0; n < input.size(); ++n)
    {
        double acc = 0.0;
        const std::size_t kMax = std::min(n + 1, irLen);

        for (std::size_t k = 0; k < kMax; ++k)
        {
            acc += static_cast<double>(input[n - k]) * static_cast<double>(ir[k]);
        }

        ref[n] = acc;
    }

    return ref;
}

// Normalised RMS difference between two signals over [start, end).
double NormalizedRmsDiff(const std::vector<double>& a, const std::vector<double>& b, std::size_t start, std::size_t end)
{
    double diffSq = 0.0;
    double refSq = 0.0;
    std::size_t count = 0;

    for (std::size_t i = start; i < end && i < a.size() && i < b.size(); ++i)
    {
        const double d = a[i] - b[i];
        diffSq += d * d;
        refSq += b[i] * b[i];
        ++count;
    }

    if (count == 0 || refSq < 1e-20)
    {
        return 0.0;
    }

    return std::sqrt(diffSq / refSq);
}

// Build a decaying, oscillating impulse response of the requested length.
std::vector<float> MakeIR(std::size_t length)
{
    std::vector<float> ir(length, 0.0f);

    for (std::size_t i = 0; i < length; ++i)
    {
        const double t = static_cast<double>(i) / static_cast<double>(length);
        const double env = std::exp(-6.0 * t);
        ir[i] = static_cast<float>(env * std::sin(2.0 * M_PI * 60.0 * t));
    }

    ir[0] = 1.0f; // strong direct component
    return ir;
}

// Same as RunConvolver, but hands over chunks of varying size instead of a steady
// block. The accumulation schedule is driven by how far through a partition the
// convolver is, so a host that chops the stream unevenly must still get the whole
// partition list folded in before each block boundary.
std::vector<double> RunConvolverRaggedly(guitarfx::RealtimeConvolver& conv, const std::vector<float>& input,
                                         unsigned seed)
{
    std::vector<double> output(input.size(), 0.0);
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> chunkDist(1, 700);
    std::vector<float> inBlock(700);
    std::vector<float> outBlock(700);

    std::size_t pos = 0;

    while (pos < input.size())
    {
        const int n =
            static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(chunkDist(rng)), input.size() - pos));

        for (int i = 0; i < n; ++i)
        {
            inBlock[static_cast<std::size_t>(i)] = input[pos + static_cast<std::size_t>(i)];
        }

        conv.Process(inBlock.data(), outBlock.data(), n);

        for (int i = 0; i < n; ++i)
        {
            output[pos + static_cast<std::size_t>(i)] = static_cast<double>(outBlock[static_cast<std::size_t>(i)]);
        }

        pos += static_cast<std::size_t>(n);
    }

    return output;
}

std::vector<float> MakeNoise(std::size_t length, unsigned seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
    std::vector<float> out(length);

    for (auto& s : out)
    {
        s = dist(rng);
    }

    return out;
}

// ---------------------------------------------------------------------------

// The output must match a direct reference convolution, with no delay between them.
bool TestNonUniformMatchesReference()
{
    std::cout << "Test: non-uniform output matches reference convolution... ";

    const std::vector<float> ir = MakeIR(4096);
    const int blockSize = 128;
    const std::vector<float> input = MakeNoise(16384, 1234u);

    guitarfx::RealtimeConvolver conv;

    if (!conv.SetImpulse(ir))
    {
        std::cout << "FAILED (SetImpulse returned false)\n";
        return false;
    }

    const std::vector<double> got = RunConvolver(conv, input, blockSize);
    const std::vector<double> ref = ReferenceConvolve(input, ir);

    // Compare once the IR has filled, clear of the start-up edge.
    const double err = NormalizedRmsDiff(got, ref, ir.size(), ref.size());

    if (!(err < 1e-3))
    {
        std::cout << "FAILED (normalized RMS error " << err << ")\n";
        return false;
    }

    std::cout << "OK (error " << err << ")\n";
    return true;
}

// A reverb-length IR must not be split into one partition per host block: the
// per-sample cost of a partitioned convolution is proportional to the partition
// count, and the non-uniform engine has no latency to keep down, so its tail is
// free to use a partition far larger than the host asked for.
bool TestLongIrKeepsPartitionCountBounded()
{
    std::cout << "Test: long IR keeps the partition count bounded... ";

    const std::vector<float> ir = MakeIR(226304); // ~4.7 s at 48 kHz, a plate reverb

    guitarfx::RealtimeConvolver conv;

    if (!conv.SetImpulse(ir))
    {
        std::cout << "FAILED (SetImpulse returned false)\n";
        return false;
    }

    // Tying the tail partition to a 64-sample host block would give ~880 partitions here.
    const std::size_t partitions = conv.GetNumPartitions();

    if (partitions > 128)
    {
        std::cout << "FAILED (" << partitions << " partitions for a 4.7 s IR)\n";
        return false;
    }

    std::cout << "OK (" << partitions << " partitions)\n";
    return true;
}

// The accumulation is spread across the samples of a block, so it must still be
// complete at every block boundary no matter how the host divides the stream.
bool TestRaggedBlockSizesMatchReference()
{
    std::cout << "Test: ragged host chunks match reference convolution... ";

    const std::vector<float> ir = MakeIR(8192); // long enough for a multi-partition tail
    const std::vector<float> input = MakeNoise(32768, 4242u);

    guitarfx::RealtimeConvolver conv;

    if (!conv.SetImpulse(ir))
    {
        std::cout << "FAILED (SetImpulse returned false)\n";
        return false;
    }

    const std::vector<double> got = RunConvolverRaggedly(conv, input, 31337u);
    const std::vector<double> ref = ReferenceConvolve(input, ir);
    const double err = NormalizedRmsDiff(got, ref, ir.size(), ref.size());

    if (!(err < 1e-3))
    {
        std::cout << "FAILED (normalized RMS error " << err << ")\n";
        return false;
    }

    std::cout << "OK (error " << err << ")\n";
    return true;
}

// With no latency, the response to an impulse is the IR itself from the very first sample. IR
// lengths sit either side of each place the layout changes: the FIR alone (up to 256), the FIR
// plus one 256 tail partition, growing stages, and from 32,769 the spread stages (a 2048 tail at
// 70,000, a 4096 tail at 140,000). Odd block sizes put every partition boundary mid-call, and a
// second impulse well into the stream checks the alignment does not depend on where the blocks
// fall.
bool TestZeroLatencyImpulseIsTheIR()
{
    std::cout << "Test: zero-latency impulse response is the IR... ";

    double worst = 0.0;

    for (const std::size_t length :
         std::vector<std::size_t>{1, 100, 256, 257, 480, 2064, 8160, 24000, 32768, 32769, 70000, 140000})
    {
        const std::vector<float> ir = MakeIR(length);
        const std::size_t secondAt = length + 1237; // after the first response has died away
        std::vector<float> input(secondAt + length + 300, 0.0f);
        input[0] = 1.0f;
        input[secondAt] = -0.5f;

        for (const int block : {1, 37, 64, 513})
        {
            if (block == 1 && length > 8160)
            {
                continue; // per-sample calls over a long stream are slow in Debug, and add nothing
            }

            guitarfx::RealtimeConvolver conv;

            if (!conv.SetImpulse(ir))
            {
                std::cout << "FAILED (IR " << length << ": SetImpulse returned false)\n";
                return false;
            }

            const std::vector<double> got = RunConvolver(conv, input, block);
            double err = 0.0;

            for (std::size_t n = 0; n < got.size(); ++n)
            {
                double expected = n < length ? static_cast<double>(ir[n]) : 0.0;

                if (n >= secondAt && n - secondAt < length)
                {
                    expected += -0.5 * static_cast<double>(ir[n - secondAt]);
                }

                err = std::max(err, std::abs(got[n] - expected));
            }

            // MakeIR peaks at 1.0, so this is relative to the peak: float rounding is ~1e-7.
            if (!(err < 1e-5))
            {
                std::cout << "FAILED (IR " << length << ", block " << block << ": worst error " << err << ")\n";
                return false;
            }

            worst = std::max(worst, err);
        }
    }

    std::cout << "OK (worst error " << worst << ")\n";
    return true;
}

// The FIR head and the stages both read the input, so the output buffer being the input
// buffer must not change what comes out. The cab and reverb hand over separate buffers today,
// but nothing in the interface says they must.
bool TestInPlaceMatchesOutOfPlace()
{
    std::cout << "Test: in-place processing matches out-of-place... ";

    const std::vector<float> input = MakeNoise(12000, 555u);

    for (const std::size_t length : std::vector<std::size_t>{200, 6000})
    {
        const std::vector<float> ir = MakeIR(length);

        guitarfx::RealtimeConvolver outOfPlace;
        outOfPlace.SetImpulse(ir);
        const std::vector<double> expected = RunConvolver(outOfPlace, input, 300);

        guitarfx::RealtimeConvolver inPlace;
        inPlace.SetImpulse(ir);
        std::vector<float> buffer(300);
        double err = 0.0;

        for (std::size_t pos = 0; pos < input.size(); pos += buffer.size())
        {
            const std::size_t n = std::min(buffer.size(), input.size() - pos);
            std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(pos), n, buffer.begin());
            inPlace.Process(buffer.data(), buffer.data(), static_cast<int>(n));

            for (std::size_t i = 0; i < n; ++i)
            {
                err = std::max(err, std::abs(static_cast<double>(buffer[i]) - expected[pos + i]));
            }
        }

        if (err != 0.0)
        {
            std::cout << "FAILED (IR " << length << ": in-place differs by " << err << ")\n";
            return false;
        }
    }

    std::cout << "OK\n";
    return true;
}

// Reset() must clear all internal state so a second run is identical to a
// fresh convolver's run.
bool TestResetClearsState()
{
    std::cout << "Test: reset clears non-uniform state... ";

    const std::vector<float> ir = MakeIR(4096);
    const int blockSize = 128;
    const std::vector<float> input = MakeNoise(8192, 7u);

    guitarfx::RealtimeConvolver conv;
    conv.SetImpulse(ir);

    const std::vector<double> first = RunConvolver(conv, input, blockSize);

    conv.Reset();
    const std::vector<double> second = RunConvolver(conv, input, blockSize);

    const double err = NormalizedRmsDiff(first, second, ir.size(), input.size());

    if (!(err < 1e-9))
    {
        std::cout << "FAILED (post-reset run differs, error " << err << ")\n";
        return false;
    }

    std::cout << "OK\n";
    return true;
}

// A long IR's stages compute each block over the samples of the next (the spread engine), a
// slice per call in proportion to how far that block has filled. Ragged chunks, from one sample
// to more than a partition, must still have every block finished when it is due, and a Reset()
// with a block half computed must leave nothing of it behind.
bool TestSpreadStagesRaggedAndReset()
{
    std::cout << "Test: spread stages under ragged chunks and a mid-block reset... ";

    const std::size_t length = 70000;
    const std::vector<float> ir = MakeIR(length);
    const std::size_t secondAt = length + 1237;
    std::vector<float> input(secondAt + length + 300, 0.0f);
    input[0] = 1.0f;
    input[secondAt] = -0.5f;

    guitarfx::RealtimeConvolver conv;

    if (!conv.SetImpulse(ir))
    {
        std::cout << "FAILED (SetImpulse returned false)\n";
        return false;
    }

    const std::vector<double> got = RunConvolverRaggedly(conv, input, 2718u);
    double err = 0.0;

    for (std::size_t n = 0; n < got.size(); ++n)
    {
        double expected = n < length ? static_cast<double>(ir[n]) : 0.0;

        if (n >= secondAt && n - secondAt < length)
        {
            expected += -0.5 * static_cast<double>(ir[n - secondAt]);
        }

        err = std::max(err, std::abs(got[n] - expected));
    }

    if (!(err < 1e-5))
    {
        std::cout << "FAILED (ragged: worst error " << err << ")\n";
        return false;
    }

    // Stop 1000 samples in, mid-block for every stage, with noise in flight; after the reset the
    // impulse run must match a fresh convolver's.
    guitarfx::RealtimeConvolver fresh;
    fresh.SetImpulse(ir);
    const std::vector<double> expected = RunConvolver(fresh, input, 96);

    RunConvolver(conv, MakeNoise(1000, 99u), 96);
    conv.Reset();
    const std::vector<double> afterReset = RunConvolver(conv, input, 96);
    double resetErr = 0.0;

    for (std::size_t n = 0; n < expected.size(); ++n)
    {
        resetErr = std::max(resetErr, std::abs(afterReset[n] - expected[n]));
    }

    if (resetErr != 0.0)
    {
        std::cout << "FAILED (after reset: differs from a fresh run by " << resetErr << ")\n";
        return false;
    }

    std::cout << "OK (worst error " << err << ")\n";
    return true;
}
} // anonymous namespace

int main()
{
    std::cout << "RealtimeConvolver Non-Uniform Partitioning Tests\n";
    std::cout << "================================================\n\n";

    int passed = 0;
    int failed = 0;

    auto runTest = [&](bool (*test)()) {
        if (test())
        {
            ++passed;
        }
        else
        {
            ++failed;
        }
    };

    runTest(TestZeroLatencyImpulseIsTheIR);
    runTest(TestNonUniformMatchesReference);
    runTest(TestLongIrKeepsPartitionCountBounded);
    runTest(TestRaggedBlockSizesMatchReference);
    runTest(TestInPlaceMatchesOutOfPlace);
    runTest(TestResetClearsState);
    runTest(TestSpreadStagesRaggedAndReset);

    std::cout << "\n================================================\n";
    std::cout << "Results: " << passed << "/" << (passed + failed) << " tests passed.\n";

    if (failed > 0)
    {
        std::cout << "\nSome tests FAILED.\n";
        return 1;
    }

    std::cout << "\nAll tests PASSED.\n";
    return 0;
}
