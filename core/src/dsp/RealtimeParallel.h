#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    #include <immintrin.h>
#endif

namespace guitarfx
{
namespace rtparallel
{
/// Whether the realtime DSP may fan work out to helper threads on this platform.
///
/// Android: the AAudio callback runs SCHED_FIFO while the helpers are ordinary
/// threads, so a callback spinning on a helper the scheduler has parked on a
/// little core, or behind the callback on its own core, is a missed deadline.
/// At one-burst blocks the hand-off also costs about as much as the work. The
/// dual-lane executor below, the graph executor's level workers and the mixer's
/// per-preset workers all key off this.
#if defined(__ANDROID__)
inline constexpr bool kParallelDspSupported = false;
#else
inline constexpr bool kParallelDspSupported = true;
#endif

inline void CpuRelax() noexcept
{
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(__GNUC__) || defined(__clang__)
    #if defined(__x86_64__) || defined(__i386__)
    __asm volatile("pause" ::: "memory");
    #elif defined(__aarch64__) || defined(__arm__)
    __asm volatile("yield" ::: "memory");
    #endif
#endif
}

/// Flush-to-zero and denormals-are-zero for the calling thread (x86 MXCSR FTZ|DAZ, ARM
/// FPCR/FPSCR FZ): the mode juce::ScopedNoDenormals gives the host's audio callback. A
/// thread that runs DSP on the audio thread's behalf needs it too, or a decaying tail it
/// happens to process falls into denormal arithmetic, many times slower, that the same
/// work on the audio thread never does. Not restored: call it at the top of a thread the
/// DSP owns.
void FlushDenormalsOnThisThread() noexcept;

/// Drops the calling thread below normal priority, so DSP it runs in the background (an offline
/// render beside the live audio) gives way to everything interactive. Not restored: for a thread
/// of its own. A no-op where there is no such priority to ask for.
void LowerPriorityOfThisThread() noexcept;

/// Puts the calling thread in the scheduling class of the host's audio thread: MMCSS "Pro Audio"
/// on Windows, a time-constraint policy on macOS, SCHED_FIFO where Linux allows it. For a helper
/// the audio thread waits on, which otherwise gets scheduled like any background thread. Best
/// effort and not restored: for a thread of its own that sleeps whenever it has no work.
void RaisePriorityOfThisThreadForAudio() noexcept;

namespace detail
{
inline thread_local bool tSharedHelpersRefused = false;
}

/// Keeps the calling thread's DSP off every helper thread for as long as it lives: while one is in
/// scope, DualLaneExecutor::Run() refuses (the effect does both halves itself) and
/// RealtimeTaskPool::Run() runs every task on the caller. For an offline render on a worker
/// thread, which must never take the one helper the audio thread's effects lean on, nor leave the
/// audio thread to find it busy and run serially; and for the mixer's block when multi-core
/// processing is off.
class ScopedSharedHelpersRefused
{
  public:
    ScopedSharedHelpersRefused() noexcept : mPrevious(detail::tSharedHelpersRefused)
    {
        detail::tSharedHelpersRefused = true;
    }

    ~ScopedSharedHelpersRefused()
    {
        detail::tSharedHelpersRefused = mPrevious;
    }

    ScopedSharedHelpersRefused(const ScopedSharedHelpersRefused&) = delete;
    ScopedSharedHelpersRefused& operator=(const ScopedSharedHelpersRefused&) = delete;

  private:
    bool mPrevious;
};

/// A fixed set of helper threads the audio thread fans work out to, and works alongside.
///
/// Run() publishes a dispatch of `count` tasks and claims them together with the workers;
/// each index runs exactly once, and Run() returns when every one has finished. The
/// calling thread takes no lock: workers park on a semaphore and waking them is a post, and
/// a worker slow to wake only costs parallelism, since the caller takes whatever is left.
///
/// A claim is one compare-exchange on a word holding the dispatch's generation, its task
/// count and the next index, so it can only succeed against the dispatch it read. The
/// pools this replaced loaded the count once on waking and then claimed with a bare
/// fetch_add, so a worker that stalled past the end of one dispatch came back into the
/// next, smaller one: it ran an index beyond that dispatch's count (a stale slot, in the
/// mixer a preset instance possibly already retired) and counted it done, letting the
/// audio thread stop waiting while real work was still running.
///
/// One dispatcher at a time. Start() and Stop() are for the message thread.
class RealtimeTaskPool
{
  public:
    using TaskFn = void (*)(void* context, int index);
    using ClaimHook = void (*)();

    /// The most tasks one dispatch fans out; the claim word gives the count eight bits.
    /// Run() does any beyond it on the calling thread.
    static constexpr int kMaxTasks = 255;

    RealtimeTaskPool() = default;
    ~RealtimeTaskPool();

    RealtimeTaskPool(const RealtimeTaskPool&) = delete;
    RealtimeTaskPool& operator=(const RealtimeTaskPool&) = delete;

    /// Replaces the workers with `workerCount` new ones; zero only stops them. If some
    /// threads cannot be created it keeps the ones that were.
    void Start(int workerCount);
    void Stop();

    [[nodiscard]] int WorkerCount() const noexcept
    {
        return mWorkerCount.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool HasWorkers() const noexcept
    {
        return WorkerCount() > 0;
    }

    /// Runs task(context, i) for every i in [0, count) on this thread and the workers, and
    /// returns once all of them have finished. With no workers, one task, or a
    /// ScopedSharedHelpersRefused in scope on this thread, it runs them here in order.
    void Run(TaskFn task, void* context, int count) noexcept;

    /// Run() for a callable taking the task index. It is called by reference, from several
    /// threads at once, and must outlive the call (a local lambda does).
    template <typename Fn> void Run(int count, Fn&& fn) noexcept
    {
        using Callable = std::remove_reference_t<Fn>;
        auto* callable = const_cast<std::remove_const_t<Callable>*>(std::addressof(fn));
        Run([](void* context, int index) { (*static_cast<Callable*>(context))(index); }, callable, count);
    }

    /// Tests only: called on every pool's workers before each claim attempt, which is where
    /// a stall exposed the stale-claim race. Null (the default) turns it off.
    static void SetWorkerClaimHookForTesting(ClaimHook hook) noexcept;

  private:
    /// A counting semaphore whose post never blocks and is never lost to a worker that
    /// is only on its way to waiting. Created by the first Start() and kept until the
    /// pool is destroyed, so a Run() racing a Stop() never posts to a closed one.
    class Semaphore
    {
      public:
        Semaphore() = default;
        ~Semaphore();
        Semaphore(const Semaphore&) = delete;
        Semaphore& operator=(const Semaphore&) = delete;

        bool Create();
        void Post(int count) noexcept;
        void Wait() noexcept;
        bool TryWait() noexcept;

      private:
        void* mHandle = nullptr;
    };

    void StopLocked();
    void WorkerLoop();
    /// Claims and runs tasks of whichever dispatch is current until it has none left.
    void RunClaims(bool onWorker) noexcept;
    void WakeWorkers(int wanted) noexcept;

    // Bits 63..16 the generation, 15..8 the task count, 7..0 the next index to claim.
    std::atomic<std::uint64_t> mClaim{0};
    std::atomic<int> mDone{0};
    // The dispatch's task. Written before mClaim publishes it and not again until every
    // claimed index is done, so a worker only reads it between a claim and that claim's
    // completion. mGeneration is the dispatcher's own count.
    TaskFn mTask = nullptr;
    void* mContext = nullptr;
    std::uint64_t mGeneration = 0;

    std::atomic<int> mWorkerCount{0};
    // Posted wake-ups no worker has taken yet. A dispatch tops them up to what it needs
    // rather than adding to them, so a worker that is slow to wake is not woken twice.
    std::atomic<int> mPendingWakes{0};
    std::atomic<bool> mQuit{false};
    Semaphore mWake;
    std::vector<std::thread> mThreads;
    std::mutex mLifecycleMutex;
};

/// Runs the two independent halves of an effect's block (left and right, slot A and B)
/// on the calling thread and one helper. One per process, shared by every effect: a caller
/// that finds it busy, another effect on another thread mid-Run, is told to run both
/// halves itself.
///
/// The helper runs only while some effect holds a Lease. The last Lease to go stops it and
/// joins it on whichever thread drops that Lease, which is a plugin instance's own teardown.
/// It used to run until the process-wide object was destroyed. In a plugin that is during
/// DLL unload, under the Windows loader lock, and a thread cannot finish exiting while
/// another holds that lock, so the join would hang the host as it unloaded the plugin.
class DualLaneExecutor
{
  public:
    static DualLaneExecutor& Instance();

    /// Keeps the helper running while held. Move-only; an empty one holds nothing.
    class Lease
    {
      public:
        Lease() noexcept = default;
        ~Lease();
        Lease(Lease&& other) noexcept : mHeld(std::exchange(other.mHeld, false))
        {
        }
        Lease& operator=(Lease&& other) noexcept;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        [[nodiscard]] bool IsHeld() const noexcept
        {
            return mHeld;
        }

      private:
        friend class DualLaneExecutor;
        explicit Lease(bool held) noexcept : mHeld(held)
        {
        }
        bool mHeld = false;
    };

    /// Takes a Lease, starting the helper if the platform allows one and it is not running.
    /// Call it from an effect's Prepare() and keep the Lease for the effect's lifetime: Run()
    /// never starts the helper, so it is never created from inside an audio block. Dropping
    /// the last Lease joins the helper, so never let that happen on the audio thread.
    [[nodiscard]] static Lease Acquire();

    /// How many Leases are held; for tests.
    [[nodiscard]] static int LeaseCountForTesting();

    DualLaneExecutor(const DualLaneExecutor&) = delete;
    DualLaneExecutor& operator=(const DualLaneExecutor&) = delete;

    [[nodiscard]] bool IsAvailable() const noexcept
    {
        return mPool.HasWorkers();
    }

    /// Runs mainFn and workerFn once each and returns true when both are done. The calling
    /// thread takes mainFn first, and workerFn as well if the helper has not claimed it by
    /// then. False, having run neither, if the helper is not running or another caller is
    /// mid-Run.
    template <typename WorkerFn, typename MainFn> bool Run(WorkerFn&& workerFn, MainFn&& mainFn)
    {
        if (!IsAvailable() || detail::tSharedHelpersRefused)
        {
            return false;
        }

        bool expected = false;

        if (!mBusy.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        {
            return false;
        }

        auto lanes = [&](int lane) {
            if (lane == 0)
            {
                mainFn();
            }
            else
            {
                workerFn();
            }
        };

        mPool.Run(2, lanes);
        mBusy.store(false, std::memory_order_release);
        return true;
    }

  private:
    DualLaneExecutor() = default;
    ~DualLaneExecutor() = default;

    void Release();

    RealtimeTaskPool mPool;
    std::atomic<bool> mBusy{false};
    std::mutex mLeaseMutex;
    int mLeases = 0;
};

/// Whether an IR effect hands its second convolver to the dual-lane helper at this block size.
/// The NAM effects decide by what a lane costs instead (StereoLaneGate); this rule was not
/// re-measured when they changed.
[[nodiscard]] inline bool ShouldParallelizeStereoWork(int numSamples) noexcept
{
    constexpr int kMinSamples = 96;
    return numSamples >= kMinSamples;
}

/// Whether a stereo effect's second lane is worth handing to the dual-lane helper, judged by
/// what a lane costs rather than by the block size. The helper sleeps between blocks and starts
/// a lane 5-20 µs after it is handed one, then runs it a little slower than a warm core would,
/// so a short lane is done sooner on the calling thread. Measured with NAM models on both
/// engines (docs/signal-chain.md): a lane of about 11 µs broke even, and from about 15 µs the
/// hand-off took 10-40% off the block. In samples that line ran from 16 (the NAM core) to 64
/// (a fused A2 model, back to back), so no one block size can draw it.
///
/// The effect runs the lane it keeps on the calling thread through TimeLane(), whichever way
/// the block goes, and asks ShouldParallelize() before each block. Audio thread only.
class StereoLaneGate
{
  public:
    [[nodiscard]] bool ShouldParallelize(int numSamples) const noexcept
    {
        return mLaneUsPerSample * static_cast<float>(numSamples) >= kMinLaneUs;
    }

    template <typename Fn> void TimeLane(int numSamples, Fn&& lane)
    {
        const auto start = std::chrono::steady_clock::now();
        lane();
        const float us = std::chrono::duration<float, std::micro>(std::chrono::steady_clock::now() - start).count();
        const float usPerSample = us / static_cast<float>(numSamples > 0 ? numSamples : 1);

        // Smoothed over about eight blocks, so one preempted block does not hold the decision.
        mLaneUsPerSample =
            mLaneUsPerSample > 0.0f ? mLaneUsPerSample + (usPerSample - mLaneUsPerSample) * 0.125f : usPerSample;
    }

  private:
    static constexpr float kMinLaneUs = 15.0f;

    float mLaneUsPerSample = 0.0f; // unmeasured: the first block stays on the calling thread
};
} // namespace rtparallel
} // namespace guitarfx
