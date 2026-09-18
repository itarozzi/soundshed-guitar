/**
 * @file StackSampler.h
 * @brief In-process call-stack sampler for profiling a busy thread on Windows.
 *
 * Suspends a target thread on a timer, copies its registers and the live part of its
 * stack, resumes it, and only then unwinds the copy -- so a profile can be taken
 * without the elevation that ETW/WPR CPU sampling requires.
 *
 * Nothing that can take a lock may run while the target is suspended: the moment the
 * target owns that lock, the sampler waits on a thread it is itself holding frozen, and
 * both stop for good. That rules out DbgHelp and the heap, RtlLookupFunctionEntry (it
 * can lock the function tables), and in a Debug build every standard container --
 * iterator debugging funnels each container and iterator operation through one global
 * lock (std::_Lockit), which the DSP thread takes constantly. A vector::push_back here
 * is what hung the Debug profiler. So the suspended window is GetThreadContext and a
 * memcpy, and names are resolved only after sampling stops.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
    #include <windows.h>
    #include <dbghelp.h>
    #pragma comment(lib, "dbghelp.lib")
#endif

namespace guitarfx::profiling
{
inline constexpr int kMaxStackDepth = 64;

/// Self and inclusive sample counts for one function.
struct SampleTotals
{
    std::uint64_t self = 0;
    std::uint64_t inclusive = 0;
};

/// Immediate callers of a matched function, by sample count.
struct CallerBreakdown
{
    std::string callee;
    std::uint64_t total = 0;
    std::vector<std::pair<std::string, std::uint64_t>> callers;
};

#if defined(_WIN32)

class StackSampler
{
  public:
    /// @p stackLow and @p stackHigh bound the target's stack reservation; get them on the
    /// target thread itself with GetCurrentThreadStackLimits.
    StackSampler(HANDLE targetThread, ULONG_PTR stackLow, ULONG_PTR stackHigh, std::uint64_t intervalUs)
        : mTargetThread(targetThread), mStackLow(stackLow), mStackHigh(stackHigh), mIntervalUs(intervalUs),
          mStackCopy((stackHigh - stackLow) / sizeof(DWORD64))
    {
    }

    void Start()
    {
        mRunning.store(true, std::memory_order_release);
        mThread = std::thread([this]() { Run(); });
    }

    void Stop()
    {
        mRunning.store(false, std::memory_order_release);

        if (mThread.joinable())
        {
            mThread.join();
        }
    }

    [[nodiscard]] const std::vector<std::vector<DWORD64>>& Stacks() const
    {
        return mStacks;
    }

  private:
    void Run()
    {
        // The sampler runs above the DSP thread so a busy machine cannot starve it.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        const double ticksPerUs = static_cast<double>(freq.QuadPart) / 1.0e6;

        LARGE_INTEGER next{};
        QueryPerformanceCounter(&next);

        std::vector<DWORD64> frames;
        frames.reserve(kMaxStackDepth);

        while (mRunning.load(std::memory_order_acquire))
        {
            next.QuadPart += static_cast<LONGLONG>(ticksPerUs * static_cast<double>(mIntervalUs));

            LARGE_INTEGER now{};

            for (;;)
            {
                QueryPerformanceCounter(&now);

                if (now.QuadPart >= next.QuadPart)
                {
                    break;
                }

                const double remainingUs = static_cast<double>(next.QuadPart - now.QuadPart) / ticksPerUs;

                if (remainingUs > 1500.0)
                {
                    Sleep(1);
                }
                else
                {
                    YieldProcessor();
                }

                if (!mRunning.load(std::memory_order_acquire))
                {
                    return;
                }
            }

            frames.clear();
            CaptureOnce(frames);

            if (!frames.empty())
            {
                mStacks.push_back(frames);
            }
        }
    }

    void CaptureOnce(std::vector<DWORD64>& frames)
    {
        DWORD64* const copy = mStackCopy.data();

        if (SuspendThread(mTargetThread) == static_cast<DWORD>(-1))
        {
            return;
        }

        // Suspended: GetThreadContext and memcpy only (see the file comment). The context
        // has to come first -- SuspendThread only requests the suspension, and
        // GetThreadContext is what waits for the thread to actually stop.
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        std::size_t copiedBytes = 0;

        if (GetThreadContext(mTargetThread, &context) != 0 && context.Rsp >= mStackLow && context.Rsp < mStackHigh &&
            context.Rsp % sizeof(DWORD64) == 0)
        {
            copiedBytes = static_cast<std::size_t>(mStackHigh - context.Rsp);
            std::memcpy(copy, reinterpret_cast<const void*>(context.Rsp), copiedBytes);
        }

        ResumeThread(mTargetThread);

        if (copiedBytes != 0)
        {
            UnwindCopy(context, copy, copiedBytes, frames);
        }
    }

    /// Unwinds the stack copied at @p copy. The copy is only unwindable once it looks like
    /// the stack it came from: saved frame pointers, and registers such as RBP, still point
    /// into the original, which the running thread has since overwritten. So every value
    /// that points into the copied range is moved to the same offset in the copy first. A
    /// value that only looks like a stack address gets moved too, which is harmless: the
    /// unwind reads saved registers and return addresses, and code addresses are never in
    /// the stack's range.
    void UnwindCopy(CONTEXT& context, DWORD64* copy, std::size_t copiedBytes, std::vector<DWORD64>& frames) const
    {
        const DWORD64 originalLow = context.Rsp;
        const DWORD64 copyLow = reinterpret_cast<DWORD64>(copy);
        const DWORD64 copyHigh = copyLow + copiedBytes;

        const auto rebase = [&](DWORD64& value) {
            if (value >= originalLow && value < mStackHigh)
            {
                value = value - originalLow + copyLow;
            }
        };

        for (std::size_t slot = 0; slot < copiedBytes / sizeof(DWORD64); ++slot)
        {
            rebase(copy[slot]);
        }

        DWORD64* const registers[] = {&context.Rax, &context.Rcx, &context.Rdx, &context.Rbx,
                                      &context.Rsp, &context.Rbp, &context.Rsi, &context.Rdi,
                                      &context.R8,  &context.R9,  &context.R10, &context.R11,
                                      &context.R12, &context.R13, &context.R14, &context.R15};

        for (DWORD64* reg : registers)
        {
            rebase(*reg);
        }

        // Unwind with RtlVirtualUnwind rather than StackWalk64, which goes through DbgHelp.
        for (int depth = 0; depth < kMaxStackDepth; ++depth)
        {
            if (context.Rip == 0)
            {
                break;
            }

            frames.push_back(context.Rip);

            // Everything the unwind reads is at or above RSP, so once RSP leaves the copy
            // the stack is either finished or corrupt; stop rather than read past the end.
            if (context.Rsp < copyLow || context.Rsp + sizeof(DWORD64) > copyHigh)
            {
                break;
            }

            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION functionEntry = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);

            if (functionEntry == nullptr)
            {
                // A leaf function has no unwind data: its return address is at RSP.
                context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
                context.Rsp += 8;
                continue;
            }

            const DWORD64 previousRsp = context.Rsp;
            PVOID handlerData = nullptr;
            DWORD64 establisherFrame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, functionEntry, &context, &handlerData,
                             &establisherFrame, nullptr);

            // A frame that does not pop the stack means the unwind is not making
            // progress; stop rather than spin on the same address.
            if (context.Rsp <= previousRsp)
            {
                break;
            }
        }
    }

    HANDLE mTargetThread;
    ULONG_PTR mStackLow;
    ULONG_PTR mStackHigh;
    std::uint64_t mIntervalUs;
    std::atomic<bool> mRunning{false};
    std::thread mThread;
    /// Sized for the whole stack reservation up front, so a capture never allocates.
    std::vector<DWORD64> mStackCopy;
    std::vector<std::vector<DWORD64>> mStacks;
};

inline std::string ResolveSymbol(HANDLE process, DWORD64 address, std::unordered_map<DWORD64, std::string>& cache)
{
    if (const auto it = cache.find(address); it != cache.end())
    {
        return it->second;
    }

    alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;

    std::string name;
    DWORD64 displacement = 0;

    if (SymFromAddr(process, address, &displacement, symbol) != 0)
    {
        name = symbol->Name;
    }

    if (name.empty())
    {
        IMAGEHLP_MODULE64 moduleInfo{};
        moduleInfo.SizeOfStruct = sizeof(IMAGEHLP_MODULE64);

        if (SymGetModuleInfo64(process, address, &moduleInfo) != 0)
        {
            name = std::string(moduleInfo.ModuleName) + "!<unknown>";
        }
        else
        {
            name = "<unknown>";
        }
    }

    cache.emplace(address, name);
    return name;
}

#endif // _WIN32
} // namespace guitarfx::profiling
