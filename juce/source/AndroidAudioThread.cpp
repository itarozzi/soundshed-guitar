#include "AndroidAudioThread.h"

#if defined(__ANDROID__)
 #include <android/log.h>
 #include <dlfcn.h>
 #include <sys/system_properties.h>
 #include <time.h>
 #include <unistd.h>

 #include <algorithm>
 #include <cmath>
 #include <cstdio>
 #include <cstring>
 #include <vector>
#endif

namespace soundshed::android_audio
{
#if defined(__ANDROID__)
namespace
{
constexpr const char* kLogTag = "SoundshedGuitar";
constexpr const char* kPolicyProperty = "debug.soundshed.audio.cores";

// The NDK's APerformanceHint_* functions arrived in API 33 and minSdk is 29, so they are looked
// up at runtime, as Oboe's AdpfWrapper does.
using GetManagerFn = void* (*)();
using CreateSessionFn = void* (*)(void* manager, const std::int32_t* tids, std::size_t count,
                                  std::int64_t targetNanos);
using UpdateTargetFn = int (*)(void* session, std::int64_t targetNanos);
using ReportActualFn = int (*)(void* session, std::int64_t actualNanos);
using CloseSessionFn = void (*)(void* session);

struct HintApi
{
    GetManagerFn getManager = nullptr;
    CreateSessionFn createSession = nullptr;
    UpdateTargetFn updateTarget = nullptr;
    ReportActualFn reportActual = nullptr;
    CloseSessionFn closeSession = nullptr;

    [[nodiscard]] bool usable() const noexcept
    {
        return getManager && createSession && updateTarget && reportActual && closeSession;
    }
};

HintApi gHintApi;

void BindHintApi()
{
    static const bool bound = [] {
        // libandroid is always loaded in an app process; RTLD_NODELETE keeps the symbols valid.
        if (void* lib = dlopen ("libandroid.so", RTLD_NOW | RTLD_NODELETE))
        {
            gHintApi.getManager = reinterpret_cast<GetManagerFn> (dlsym (lib, "APerformanceHint_getManager"));
            gHintApi.createSession =
                reinterpret_cast<CreateSessionFn> (dlsym (lib, "APerformanceHint_createSession"));
            gHintApi.updateTarget =
                reinterpret_cast<UpdateTargetFn> (dlsym (lib, "APerformanceHint_updateTargetWorkDuration"));
            gHintApi.reportActual =
                reinterpret_cast<ReportActualFn> (dlsym (lib, "APerformanceHint_reportActualWorkDuration"));
            gHintApi.closeSession = reinterpret_cast<CloseSessionFn> (dlsym (lib, "APerformanceHint_closeSession"));
        }

        return true;
    }();
    (void) bound;
}

std::int64_t NowNanos() noexcept
{
    timespec ts {};
    clock_gettime (CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t> (ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

// A cpufreq or capacity file's single number, or -1.
long ReadNumber (const char* path)
{
    long value = -1;

    if (FILE* file = std::fopen (path, "r"))
    {
        if (std::fscanf (file, "%ld", &value) != 1)
            value = -1;

        std::fclose (file);
    }

    return value;
}

// How fast each core is, by the kernel's own measure: cpu_capacity is what the energy-aware
// scheduler places by (1024 for the fastest class); the maximum clock stands in without it.
std::vector<long> ReadCoreCapacities()
{
    std::vector<long> capacities;
    char path[96];

    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
    {
        std::snprintf (path, sizeof (path), "/sys/devices/system/cpu/cpu%d/cpu_capacity", cpu);
        long capacity = ReadNumber (path);

        if (capacity <= 0)
        {
            std::snprintf (path, sizeof (path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
            capacity = ReadNumber (path);
        }

        if (capacity <= 0)
        {
            std::snprintf (path, sizeof (path), "/sys/devices/system/cpu/cpu%d", cpu);

            // A core with neither file but present (offline, say) is skipped; past the last one, stop.
            if (access (path, F_OK) != 0)
                break;
        }

        capacities.push_back (capacity);
    }

    return capacities;
}

void FormatCores (const cpu_set_t& set, char* out, std::size_t size) noexcept
{
    std::size_t used = 0;
    out[0] = '\0';

    for (int cpu = 0; cpu < CPU_SETSIZE && used + 5 < size; ++cpu)
    {
        if (CPU_ISSET (cpu, &set))
            used += static_cast<std::size_t> (std::snprintf (out + used, size - used, used ? ",%d" : "%d", cpu));
    }
}
} // namespace

AudioThreadPlacement::~AudioThreadPlacement()
{
    closeSession();
}

void AudioThreadPlacement::prepare (double sampleRate)
{
    closeSession();
    mPlacedTid = 0;
    mTargetNanos = 0;
    mSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;

    char value[PROP_VALUE_MAX] = {};
    __system_property_get (kPolicyProperty, value);

    if (std::strcmp (value, "big") == 0)
        mPolicy = Policy::big;
    else if (std::strcmp (value, "prime") == 0)
        mPolicy = Policy::prime;
    else if (std::strcmp (value, "off") == 0)
        mPolicy = Policy::off;
    else
        mPolicy = Policy::automatic;

    // Only cores this process may use: a cpuset narrower than the device (a backgrounded app's,
    // say) is respected rather than fought.
    cpu_set_t allowed;
    CPU_ZERO (&allowed);

    if (sched_getaffinity (0, sizeof (allowed), &allowed) != 0)
        CPU_ZERO (&allowed);

    const auto capacities = ReadCoreCapacities();
    long slowest = 0;
    long fastest = 0;

    for (std::size_t cpu = 0; cpu < capacities.size(); ++cpu)
    {
        if (capacities[cpu] <= 0 || !CPU_ISSET (static_cast<int> (cpu), &allowed))
            continue;

        slowest = slowest == 0 ? capacities[cpu] : std::min (slowest, capacities[cpu]);
        fastest = std::max (fastest, capacities[cpu]);
    }

    CPU_ZERO (&mBigCores);
    CPU_ZERO (&mPrimeCores);

    for (std::size_t cpu = 0; cpu < capacities.size(); ++cpu)
    {
        if (capacities[cpu] <= 0 || !CPU_ISSET (static_cast<int> (cpu), &allowed))
            continue;

        if (capacities[cpu] > slowest)
            CPU_SET (static_cast<int> (cpu), &mBigCores);

        if (capacities[cpu] == fastest)
            CPU_SET (static_cast<int> (cpu), &mPrimeCores);
    }

    // One core class (every core alike) has nothing to choose between.
    mHaveBigCores = CPU_COUNT (&mBigCores) > 0;
    mHavePrimeCores = mHaveBigCores && CPU_COUNT (&mPrimeCores) > 0;

    BindHintApi();
}

void AudioThreadPlacement::release()
{
    closeSession();
    mPlacedTid = 0;
}

void AudioThreadPlacement::beginBlock() noexcept
{
    // Bionic caches the tid in the thread, so this costs no system call.
    if (gettid() != mPlacedTid)
        placeCurrentThread();

    if (mSession != nullptr)
        mBlockStartNanos = NowNanos();
}

void AudioThreadPlacement::endBlock (int numSamples) noexcept
{
    if (mSession == nullptr || numSamples <= 0)
        return;

    const auto actual = NowNanos() - mBlockStartNanos;
    const auto target = static_cast<std::int64_t> (std::llround (1.0e9 * numSamples / mSampleRate));

    if (target != mTargetNanos)
    {
        mTargetNanos = target;
        gHintApi.updateTarget (mSession, target);
    }

    gHintApi.reportActual (mSession, std::max<std::int64_t> (actual, 1));
}

void AudioThreadPlacement::placeCurrentThread() noexcept
{
    // Once per callback thread: AAudio makes a new one when a stream is reopened. The session
    // and affinity are system calls, not real-time safe, but this runs only on that first block.
    const pid_t tid = gettid();
    mPlacedTid = tid;
    closeSession();

    if (mPolicy == Policy::off)
    {
        __android_log_print (ANDROID_LOG_INFO, kLogTag, "Audio thread %d: left to the scheduler (cores=off)", tid);
        return;
    }

    if (gHintApi.usable())
    {
        if (void* manager = gHintApi.getManager())
        {
            const std::int32_t tids[] = { tid };
            // A guess at the first block's budget; endBlock corrects it from the real block size.
            mTargetNanos = static_cast<std::int64_t> (std::llround (1.0e9 * 96.0 / mSampleRate));
            mSession = gHintApi.createSession (manager, tids, 1, mTargetNanos);
        }
    }

    const cpu_set_t* pin = nullptr;

    if (mPolicy == Policy::prime && mHavePrimeCores)
        pin = &mPrimeCores;
    else if (mPolicy == Policy::big && mHaveBigCores)
        pin = &mBigCores;
    else if (mPolicy == Policy::automatic && mSession == nullptr && mHaveBigCores)
        pin = &mBigCores;

    char cores[96] = "all";
    bool pinned = false;

    if (pin != nullptr)
    {
        pinned = sched_setaffinity (0, sizeof (cpu_set_t), pin) == 0;

        if (pinned)
            FormatCores (*pin, cores, sizeof (cores));
    }

    __android_log_print (ANDROID_LOG_INFO, kLogTag, "Audio thread %d: performance hint %s, cores %s%s", tid,
                         mSession != nullptr ? "on" : "unavailable", cores,
                         pin != nullptr && !pinned ? " (pinning refused)" : "");
}

void AudioThreadPlacement::closeSession() noexcept
{
    if (mSession != nullptr && gHintApi.closeSession != nullptr)
        gHintApi.closeSession (mSession);

    mSession = nullptr;
}

#else

AudioThreadPlacement::~AudioThreadPlacement() = default;
void AudioThreadPlacement::prepare (double) {}
void AudioThreadPlacement::release() {}

#endif
} // namespace soundshed::android_audio
