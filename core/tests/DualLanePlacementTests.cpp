/**
 * @file DualLanePlacementTests.cpp
 * @brief Where the dual-lane helper runs against the thread that hands it work.
 *
 * Left to itself, Windows woke the helper (rtparallel::DualLaneExecutor) on the audio thread's
 * SMT sibling in 70-98% of blocks, where both lanes ran at about half speed. After each lane the
 * helper now narrows its own CPU sets to the cores other than the dispatcher's. This pins the
 * calling thread to one core, then to the core the helper last ran on, and checks, once the
 * helper has had a few blocks to follow, that its CPU sets leave out the caller's core and that
 * it runs elsewhere. Unsteered, the second check alone usually passes here too; the first does
 * not. Windows only: elsewhere placement is left to the scheduler and there is nothing to check.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <utility>
#include <vector>

#include "dsp/RealtimeParallel.h"

#include "CpuPlacement.h"

namespace
{
using guitarfx::rtparallel::DualLaneExecutor;
using guitarfx::test::CpuTopology;
using guitarfx::test::CurrentCpu;

#if defined(_WIN32)
void SpinFor(std::chrono::microseconds duration)
{
    const auto until = std::chrono::steady_clock::now() + duration;

    while (std::chrono::steady_clock::now() < until)
    {
    }
}

// CPU set id -> logical processor, for reading back what the helper selected.
std::vector<std::pair<ULONG, int>> CpuOfEachSet()
{
    std::vector<std::pair<ULONG, int>> cpuOfSet;
    ULONG length = 0;
    GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
    std::vector<std::uint8_t> buffer(length);

    if (length > 0 && GetSystemCpuSetInformation(reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buffer.data()), length,
                                                 &length, GetCurrentProcess(), 0))
    {
        for (ULONG offset = 0; offset < length;)
        {
            const auto* set = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + offset);
            offset += set->Size;

            if (set->Type == CpuSetInformation)
            {
                cpuOfSet.emplace_back(set->CpuSet.Id, set->CpuSet.Group * 64 + set->CpuSet.LogicalProcessorIndex);
            }
        }
    }

    return cpuOfSet;
}

bool TestHelperKeepsOffTheCallersCore()
{
    if (!guitarfx::rtparallel::kParallelDspSupported || std::thread::hardware_concurrency() < 2)
    {
        return true;
    }

    const CpuTopology topology;

    if (topology.CoreCount() < 2 || topology.CpusOf(0).front() >= 64 || topology.CpusOf(1).front() >= 64)
    {
        std::cout << "  fewer than two cores in processor group 0: nothing to check\n";
        return true;
    }

    const auto cpuOfSet = CpuOfEachSet();
    const auto coreOfSet = [&](ULONG id) {
        for (const auto& [setId, cpu] : cpuOfSet)
        {
            if (setId == id)
            {
                return topology.CoreOf(cpu);
            }
        }

        return -1;
    };

    const auto lease = DualLaneExecutor::Acquire();
    auto& lanes = DualLaneExecutor::Instance();
    const auto caller = std::this_thread::get_id();
    const DWORD_PTR previousAffinity =
        SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << topology.CpusOf(0).front());
    bool ok = previousAffinity != 0;
    int pinnedCore = 0;

    for (int pass = 0; pass < 3 && ok; ++pass)
    {
        const int pinnedCpu = topology.CpusOf(pinnedCore).front();

        if (pinnedCpu >= 64)
        {
            break; // a core outside processor group 0, where a mask cannot pin the caller
        }

        SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << pinnedCpu);
        std::this_thread::yield();

        constexpr int kSettleRuns = 5;
        std::atomic<int> lastHelperCpu{-1};
        std::atomic<int> helperLanes{0};
        std::atomic<int> onCallersCore{0};
        // The helper's selected CPU sets, read on the helper in the last lane it ran.
        std::array<ULONG, 256> selected{};
        std::atomic<ULONG> selectedCount{0};

        for (int i = 0; i < 40; ++i)
        {
            const bool settled = i >= kSettleRuns;
            lanes.Run(
                [&] {
                    if (std::this_thread::get_id() != caller)
                    {
                        const int cpu = CurrentCpu();
                        lastHelperCpu.store(cpu);

                        if (settled)
                        {
                            ULONG count = 0;
                            GetThreadSelectedCpuSets(GetCurrentThread(), selected.data(),
                                                     static_cast<ULONG>(selected.size()), &count);
                            selectedCount.store(count);
                            helperLanes.fetch_add(1);
                            onCallersCore.fetch_add(topology.CoreOf(cpu) == pinnedCore ? 1 : 0);
                        }
                    }
                },
                [] { SpinFor(std::chrono::microseconds(100)); });
        }

        if (helperLanes.load() == 0 || onCallersCore.load() != 0)
        {
            std::cerr << "  caller pinned to core " << pinnedCore << ": the helper ran " << onCallersCore.load()
                      << " of its " << helperLanes.load() << " lanes there\n";
            ok = false;
        }

        const ULONG count = std::min<ULONG>(selectedCount.load(), static_cast<ULONG>(selected.size()));
        const bool selectsCallersCore = std::any_of(selected.begin(), selected.begin() + count,
                                                    [&](ULONG id) { return coreOfSet(id) == pinnedCore; });

        if (count == 0 || selectsCallersCore)
        {
            std::cerr << "  caller pinned to core " << pinnedCore << ": the helper selects " << count << " CPU sets"
                      << (selectsCallersCore ? ", the caller's core among them" : "") << "\n";
            ok = false;
        }

        // Next onto the core the helper was on, which it must then leave.
        pinnedCore = std::max(topology.CoreOf(lastHelperCpu.load()), 0);
    }

    if (previousAffinity != 0)
    {
        SetThreadAffinityMask(GetCurrentThread(), previousAffinity);
    }

    return ok;
}
#endif
} // namespace

int main()
{
    std::cout << "========================================\n";
    std::cout << "Dual-lane helper placement tests\n";
    std::cout << "========================================\n";

#if defined(_WIN32)
    std::cout << "Dual-lane helper keeps off the calling thread's core\n";
    const bool ok = TestHelperKeepsOffTheCallersCore();
    std::cout << (ok ? "  PASS\n" : "  FAIL\n");
    std::cout << (ok ? "All tests passed\n" : "Some tests FAILED\n");
    return ok ? 0 : 1;
#else
    std::cout << "Placement is left to the scheduler here: nothing to check\n";
    return 0;
#endif
}
