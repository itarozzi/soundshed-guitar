/**
 * @file CpuPlacement.h
 * @brief Which logical processor a thread is on, and which physical core that is.
 *
 * For the tests and benchmark of where the dual-lane helper runs against the thread that hands
 * it work (rtparallel::DualLaneExecutor). Logical processors are numbered group * 64 + number.
 * Windows only: elsewhere CurrentCpu() is -1, there are no cores, and CoreOf() is always -1.
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#endif

namespace guitarfx::test
{
inline int CurrentCpu()
{
#if defined(_WIN32)
    PROCESSOR_NUMBER number{};
    GetCurrentProcessorNumberEx(&number);
    return number.Group * 64 + number.Number;
#else
    return -1;
#endif
}

/// The SMT topology: each physical core's logical processors.
class CpuTopology
{
  public:
    CpuTopology()
    {
#if defined(_WIN32)
        DWORD length = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
        std::vector<std::uint8_t> buffer(length);

        if (length == 0 || !GetLogicalProcessorInformationEx(
                               RelationProcessorCore,
                               reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data()), &length))
        {
            return;
        }

        for (DWORD offset = 0; offset < length;)
        {
            const auto* entry =
                reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
            offset += entry->Size;
            const int core = static_cast<int>(mCpusOfCore.size());
            mCpusOfCore.emplace_back();

            for (WORD g = 0; g < entry->Processor.GroupCount; ++g)
            {
                const GROUP_AFFINITY& mask = entry->Processor.GroupMask[g];

                for (int bit = 0; bit < 64; ++bit)
                {
                    if ((mask.Mask >> bit) & 1)
                    {
                        const std::size_t cpu = static_cast<std::size_t>(mask.Group) * 64 + bit;
                        mCoreOfCpu.resize(std::max(mCoreOfCpu.size(), cpu + 1), -1);
                        mCoreOfCpu[cpu] = core;
                        mCpusOfCore.back().push_back(static_cast<int>(cpu));
                    }
                }
            }
        }

#endif
    }

    [[nodiscard]] int CoreCount() const
    {
        return static_cast<int>(mCpusOfCore.size());
    }

    /// The physical core of a logical processor, or -1.
    [[nodiscard]] int CoreOf(int cpu) const
    {
        return cpu >= 0 && static_cast<std::size_t>(cpu) < mCoreOfCpu.size() ? mCoreOfCpu[static_cast<std::size_t>(cpu)]
                                                                             : -1;
    }

    /// A core's logical processors, lowest first.
    [[nodiscard]] const std::vector<int>& CpusOf(int core) const
    {
        return mCpusOfCore[static_cast<std::size_t>(core)];
    }

  private:
    std::vector<int> mCoreOfCpu;
    std::vector<std::vector<int>> mCpusOfCore;
};
} // namespace guitarfx::test
