#pragma once

/**
 * AndroidAudioThread — where the audio callback runs on a phone.
 *
 * Android schedules the AAudio callback, a SCHED_FIFO thread, as it sees fit, and on
 * big.LITTLE phones that is often the little cluster: on a Galaxy S21 it never left the four
 * Cortex-A55 cores, where a NAM model costs about seven times what it does on the X1. Two
 * levers, both applied from the callback thread itself:
 *
 *  - ADPF (Android's performance hint API, API 33+): a session for the callback thread, told
 *    each block's budget and how long the block took, so the scheduler can raise the thread's
 *    clocks and core class to meet it. Used wherever the device opens a session.
 *  - Affinity: without a session, the thread is pinned to every core faster than the slowest
 *    class, read from the kernel's cpu_capacity, never hard-coded core numbers. The pin is
 *    checked a few times a second and set again when lost: Android moves the app between
 *    cpusets as it goes to the background and back, and older kernels reset affinity each time.
 *
 * The debug.soundshed.audio.cores property picks the policy without a rebuild:
 *
 *     adb shell setprop debug.soundshed.audio.cores auto    (default: ADPF, else big cores)
 *     adb shell setprop debug.soundshed.audio.cores big     (ADPF if any, and big cores)
 *     adb shell setprop debug.soundshed.audio.cores prime   (ADPF if any, and the fastest class only)
 *     adb shell setprop debug.soundshed.audio.cores off     (neither: the scheduler's choice)
 *     adb shell am force-stop com.soundshed.guitar          (then relaunch)
 *
 * What was chosen goes to logcat under the SoundshedGuitar tag. Everything here is a no-op
 * outside Android.
 */

#include <cstdint>

#if defined(__ANDROID__)
 #include <sched.h>
 #include <sys/types.h>
#endif

namespace soundshed::android_audio
{
class AudioThreadPlacement
{
public:
    AudioThreadPlacement() = default;
    ~AudioThreadPlacement();

    AudioThreadPlacement (const AudioThreadPlacement&) = delete;
    AudioThreadPlacement& operator= (const AudioThreadPlacement&) = delete;

    /** Off the audio thread, with audio stopped: reads the core layout and the policy, and
        binds the hint API. The next block places whichever thread runs it. */
    void prepare (double sampleRate);

    /** Off the audio thread, with audio stopped: ends the hint session. */
    void release();

#if defined(__ANDROID__)
    /** Audio thread, first thing in a block. Places a thread it has not seen before. */
    void beginBlock() noexcept;

    /** Audio thread, last thing in a block: reports the block's work to the hint session. */
    void endBlock (int numSamples) noexcept;
#else
    void beginBlock() noexcept {}
    void endBlock (int) noexcept {}
#endif

private:
#if defined(__ANDROID__)
    enum class Policy
    {
        automatic, // ADPF where a session opens, big cores otherwise
        big,       // ADPF if any, and every core faster than the slowest class
        prime,     // ADPF if any, and the fastest class only
        off        // neither
    };

    enum class PinState
    {
        unknown,
        pinned,
        refused // the cpuset holds none of the wanted cores; tried again at the next check
    };

    void placeCurrentThread() noexcept;
    void checkAffinity() noexcept;
    void closeSession() noexcept;

    Policy mPolicy = Policy::automatic;
    double mSampleRate = 48000.0;
    cpu_set_t mBigCores {};
    cpu_set_t mPrimeCores {};
    bool mHaveBigCores = false;
    bool mHavePrimeCores = false;

    // Audio thread only, while audio runs; prepare() and release() touch them with audio stopped.
    pid_t mPlacedTid = 0;
    const cpu_set_t* mPin = nullptr; // the cores wanted, or null to leave it to the scheduler
    cpu_set_t mPinnedAs {};          // the affinity the kernel gave the last pin
    PinState mPinState = PinState::unknown;
    int mBlocksUntilAffinityCheck = 0;
    void* mSession = nullptr; // APerformanceHintSession*
    std::int64_t mTargetNanos = 0;
    std::int64_t mBlockStartNanos = 0;
#endif
};
} // namespace soundshed::android_audio
