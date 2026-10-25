// LooperRender: a layer's DI through detached graphs, twice round the loop. See LooperRender.h.

#include "controller/LooperRender.h"

#include "dsp/RealtimeParallel.h"

#include <algorithm>

namespace guitarfx::looper_render
{
Channels RenderLoop(Graphs& graphs, const Channels& di, float calibrationGain, std::atomic<float>* progress)
{
    if (di.empty() || di[0].empty() || !graphs.pre || !graphs.preset || !graphs.post)
    {
        return {};
    }

    // Our work never borrows the helper threads the live chain's effects share.
    const rtparallel::ScopedSharedHelpersRefused singleThreaded;
    graphs.pre->Reset();
    graphs.preset->Reset();
    graphs.post->Reset();

    const auto length = static_cast<std::int64_t>(di[0].size());
    const std::int64_t latency = std::max(0, graphs.pre->GetTotalLatencySamples()) +
                                 std::max(0, graphs.preset->GetTotalLatencySamples()) +
                                 std::max(0, graphs.post->GetTotalLatencySamples());
    const std::int64_t total = 2 * length + latency;
    const std::vector<float>& left = di[0];
    const std::vector<float>& right = di.size() > 1 ? di[1] : di[0];

    std::vector<float> inL(kBlockSize), inR(kBlockSize), preL(kBlockSize), preR(kBlockSize);
    std::vector<float> rigL(kBlockSize), rigR(kBlockSize), outL(kBlockSize), outR(kBlockSize);
    Channels wet(2, std::vector<float>(static_cast<std::size_t>(length)));

    for (std::int64_t done = 0; done < total; done += kBlockSize)
    {
        const int n = static_cast<int>(std::min<std::int64_t>(kBlockSize, total - done));

        for (int i = 0; i < n; ++i)
        {
            const auto pos = static_cast<std::size_t>((done + i) % length);
            inL[static_cast<std::size_t>(i)] = left[pos] * calibrationGain;
            inR[static_cast<std::size_t>(i)] = right[pos] * calibrationGain;
        }

        float* in[2] = {inL.data(), inR.data()};
        float* pre[2] = {preL.data(), preR.data()};
        float* rig[2] = {rigL.data(), rigR.data()};
        float* out[2] = {outL.data(), outR.data()};
        graphs.pre->Process(in, pre, n);
        graphs.preset->Process(pre, rig, n);

        for (int i = 0; i < n; ++i)
        {
            rigL[static_cast<std::size_t>(i)] *= graphs.slotGainLeft;
            rigR[static_cast<std::size_t>(i)] *= graphs.slotGainRight;
        }

        graphs.post->Process(rig, out, n);

        // The second pass, the graphs' latency taken off.
        for (int i = 0; i < n; ++i)
        {
            const std::int64_t p = done + i - length - latency;

            if (p >= 0 && p < length)
            {
                wet[0][static_cast<std::size_t>(p)] = outL[static_cast<std::size_t>(i)];
                wet[1][static_cast<std::size_t>(p)] = outR[static_cast<std::size_t>(i)];
            }
        }

        if (progress)
        {
            progress->store(static_cast<float>(done + n) / static_cast<float>(total), std::memory_order_relaxed);
        }
    }

    return wet;
}
} // namespace guitarfx::looper_render
