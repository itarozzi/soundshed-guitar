#include "dsp/RealtimeParallel.h"

#include <algorithm>
#include <climits>

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#elif defined(__APPLE__)
    #include <dispatch/dispatch.h>
#else
    #include <cerrno>
    #include <semaphore.h>
#endif

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    #include <xmmintrin.h>
#elif defined(_MSC_VER) && defined(_M_ARM64)
    #include <float.h>
#endif

namespace guitarfx
{
namespace rtparallel
{
namespace
{
constexpr int kHeadShift = 0;
constexpr int kCountShift = 8;
constexpr int kGenerationShift = 16;
constexpr std::uint64_t kFieldMask = 0xFF;
static_assert(RealtimeTaskPool::kMaxTasks == static_cast<int>(kFieldMask), "the count and head are 8-bit fields");

constexpr int HeadOf(std::uint64_t word) noexcept
{
    return static_cast<int>((word >> kHeadShift) & kFieldMask);
}

constexpr int CountOf(std::uint64_t word) noexcept
{
    return static_cast<int>((word >> kCountShift) & kFieldMask);
}

// How long a dispatcher spins on the last tasks before it starts yielding its core. Spinning
// is the point on the fast path, where the helpers are running and finish in microseconds.
// But a helper that has not been scheduled cannot be waited out by spinning: the audio
// thread usually holds the DSP lock here, so starving the helper behind it would hang every
// message-thread handler waiting on that lock, not just this block.
constexpr int kSpinsBeforeYield = 10000;

std::atomic<RealtimeTaskPool::ClaimHook> gWorkerClaimHook{nullptr};
} // namespace

void FlushDenormalsOnThisThread() noexcept
{
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    _mm_setcsr(_mm_getcsr() | 0x8040u); // FTZ (bit 15) | DAZ (bit 6)
#elif defined(_MSC_VER) && defined(_M_ARM64)
    _control87(_DN_FLUSH, _MCW_DN); // FPCR.FZ
#elif defined(__aarch64__)
    std::uint64_t fpcr = 0;
    asm volatile("mrs %0, fpcr" : "=r"(fpcr));
    asm volatile("msr fpcr, %0" : : "r"(fpcr | (std::uint64_t{1} << 24))); // FZ
#elif defined(__arm__) && defined(__ARM_FP)
    std::uint32_t fpscr = 0;
    asm volatile("vmrs %0, fpscr" : "=r"(fpscr));
    asm volatile("vmsr fpscr, %0" : : "r"(fpscr | (std::uint32_t{1} << 24))); // FZ
#endif
}

// ---------------------------------------------------------------------------
// Semaphore
// ---------------------------------------------------------------------------

RealtimeTaskPool::Semaphore::~Semaphore()
{
    if (!mHandle)
    {
        return;
    }

#if defined(_WIN32)
    CloseHandle(static_cast<HANDLE>(mHandle));
#elif defined(__APPLE__)
    dispatch_release(static_cast<dispatch_semaphore_t>(mHandle));
#else
    auto* semaphore = static_cast<sem_t*>(mHandle);
    sem_destroy(semaphore);
    delete semaphore;
#endif
}

bool RealtimeTaskPool::Semaphore::Create()
{
    if (mHandle)
    {
        return true;
    }

#if defined(_WIN32)
    mHandle = CreateSemaphoreW(nullptr, 0, LONG_MAX, nullptr);
#elif defined(__APPLE__)
    mHandle = dispatch_semaphore_create(0);
#else
    auto* semaphore = new sem_t;

    if (sem_init(semaphore, 0, 0) == 0)
    {
        mHandle = semaphore;
    }
    else
    {
        delete semaphore;
    }

#endif

    return mHandle != nullptr;
}

void RealtimeTaskPool::Semaphore::Post(int count) noexcept
{
    if (count <= 0)
    {
        return;
    }

#if defined(_WIN32)
    ReleaseSemaphore(static_cast<HANDLE>(mHandle), count, nullptr);
#elif defined(__APPLE__)

    for (int i = 0; i < count; ++i)
    {
        dispatch_semaphore_signal(static_cast<dispatch_semaphore_t>(mHandle));
    }

#else

    for (int i = 0; i < count; ++i)
    {
        sem_post(static_cast<sem_t*>(mHandle));
    }

#endif
}

void RealtimeTaskPool::Semaphore::Wait() noexcept
{
#if defined(_WIN32)
    WaitForSingleObject(static_cast<HANDLE>(mHandle), INFINITE);
#elif defined(__APPLE__)
    dispatch_semaphore_wait(static_cast<dispatch_semaphore_t>(mHandle), DISPATCH_TIME_FOREVER);
#else

    while (sem_wait(static_cast<sem_t*>(mHandle)) != 0 && errno == EINTR)
    {
    }

#endif
}

bool RealtimeTaskPool::Semaphore::TryWait() noexcept
{
#if defined(_WIN32)
    return WaitForSingleObject(static_cast<HANDLE>(mHandle), 0) == WAIT_OBJECT_0;
#elif defined(__APPLE__)
    return dispatch_semaphore_wait(static_cast<dispatch_semaphore_t>(mHandle), DISPATCH_TIME_NOW) == 0;
#else

    while (sem_trywait(static_cast<sem_t*>(mHandle)) != 0)
    {
        if (errno != EINTR)
        {
            return false;
        }
    }

    return true;
#endif
}

// ---------------------------------------------------------------------------
// RealtimeTaskPool
// ---------------------------------------------------------------------------

RealtimeTaskPool::~RealtimeTaskPool()
{
    Stop();
}

void RealtimeTaskPool::SetWorkerClaimHookForTesting(ClaimHook hook) noexcept
{
    gWorkerClaimHook.store(hook, std::memory_order_release);
}

void RealtimeTaskPool::Start(int workerCount)
{
    std::lock_guard<std::mutex> lock(mLifecycleMutex);
    StopLocked();

    if (workerCount <= 0 || !mWake.Create())
    {
        return;
    }

    mQuit.store(false, std::memory_order_seq_cst);

    try
    {
        mThreads.reserve(static_cast<std::size_t>(workerCount));

        for (int i = 0; i < workerCount; ++i)
        {
            mThreads.emplace_back([this] { WorkerLoop(); });
        }
    }
    catch (...)
    {
        // Fewer helpers rather than none: the dispatcher does whatever they cannot.
    }

    mWorkerCount.store(static_cast<int>(mThreads.size()), std::memory_order_release);
}

void RealtimeTaskPool::Stop()
{
    std::lock_guard<std::mutex> lock(mLifecycleMutex);
    StopLocked();
}

void RealtimeTaskPool::StopLocked()
{
    if (mThreads.empty())
    {
        return;
    }

    // A Run() already in flight keeps going: a worker finishes the task it has claimed
    // before it looks at mQuit, and the dispatcher takes whatever is left unclaimed.
    mWorkerCount.store(0, std::memory_order_release);
    mQuit.store(true, std::memory_order_seq_cst);

    const int count = static_cast<int>(mThreads.size());
    mPendingWakes.fetch_add(count, std::memory_order_seq_cst);
    mWake.Post(count);

    for (auto& thread : mThreads)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }

    mThreads.clear();

    // Wake-ups nobody took, so the next workers do not start by waking for nothing.
    while (mWake.TryWait())
    {
        mPendingWakes.fetch_sub(1, std::memory_order_seq_cst);
    }
}

void RealtimeTaskPool::WorkerLoop()
{
    FlushDenormalsOnThisThread();

    while (true)
    {
        mWake.Wait();
        mPendingWakes.fetch_sub(1, std::memory_order_seq_cst);

        if (mQuit.load(std::memory_order_seq_cst))
        {
            return;
        }

        RunClaims(true);
    }
}

void RealtimeTaskPool::WakeWorkers(int wanted) noexcept
{
    // The claim word was published (seq_cst) before this read, and a waking worker takes
    // its wake-up off the count (seq_cst) before it reads the word. So either this read
    // sees that decrement and posts another, or the worker sees this dispatch.
    int pending = mPendingWakes.load(std::memory_order_seq_cst);

    while (pending < wanted)
    {
        if (mPendingWakes.compare_exchange_weak(pending, wanted, std::memory_order_seq_cst))
        {
            mWake.Post(wanted - pending);
            return;
        }
    }
}

void RealtimeTaskPool::RunClaims(bool onWorker) noexcept
{
    while (true)
    {
        if (onWorker)
        {
            if (const ClaimHook hook = gWorkerClaimHook.load(std::memory_order_acquire))
            {
                hook();
            }
        }

        std::uint64_t word = mClaim.load(std::memory_order_seq_cst);
        int index = -1;

        while (index < 0)
        {
            if (HeadOf(word) >= CountOf(word))
            {
                return;
            }

            // Succeeds only if the word is still the one just read, generation included, so
            // the index belongs to the dispatch it was read from. A failure reloads the word,
            // which may be a newer dispatch; claiming from that one is just as valid.
            if (mClaim.compare_exchange_weak(word, word + 1, std::memory_order_acq_rel, std::memory_order_acquire))
            {
                index = HeadOf(word);
            }
        }

        mTask(mContext, index);
        mDone.fetch_add(1, std::memory_order_release);
    }
}

void RealtimeTaskPool::Run(TaskFn task, void* context, int count) noexcept
{
    if (count <= 0)
    {
        return;
    }

    const int workers = WorkerCount();
    const int fanned = std::min(count, kMaxTasks);

    if (workers == 0 || fanned < 2)
    {
        for (int i = 0; i < count; ++i)
        {
            task(context, i);
        }

        return;
    }

    // Every index of the previous dispatch finished before its Run() returned, so no worker
    // is reading these, and none can count towards this dispatch until the word below
    // publishes it.
    mTask = task;
    mContext = context;
    mDone.store(0, std::memory_order_relaxed);
    ++mGeneration;
    mClaim.store((mGeneration << kGenerationShift) | (static_cast<std::uint64_t>(fanned) << kCountShift),
                 std::memory_order_seq_cst);

    WakeWorkers(std::min(fanned - 1, workers));
    RunClaims(false);

    for (int i = fanned; i < count; ++i)
    {
        task(context, i);
    }

    int spins = 0;

    while (mDone.load(std::memory_order_acquire) < fanned)
    {
        if (++spins < kSpinsBeforeYield)
        {
            CpuRelax();
        }
        else
        {
            std::this_thread::yield();
        }
    }
}

// ---------------------------------------------------------------------------
// DualLaneExecutor
// ---------------------------------------------------------------------------

DualLaneExecutor& DualLaneExecutor::Instance()
{
    static DualLaneExecutor instance;
    return instance;
}

void DualLaneExecutor::EnsureStarted()
{
    auto& executor = Instance();

    std::call_once(executor.mStarted, [&executor] {
        if (kParallelDspSupported && std::thread::hardware_concurrency() >= 2)
        {
            executor.mPool.Start(1);
        }
    });
}
} // namespace rtparallel
} // namespace guitarfx
