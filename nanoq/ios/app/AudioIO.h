#pragma once

// The iOS app's audio: an AVAudioSession in PlayAndRecord and a RemoteIO unit, input on bus 1
// and output on bus 0, both Float32 stereo, non-interleaved. Each render callback pulls the
// input and hands both to the ClapHost on Core Audio's real-time thread.
//
// The plugin's activation follows the unit: Start activates it at the session's rate, and an
// interruption, a media-services reset or a rate change stops the unit, deactivates the plugin
// and, where it can, starts both again. Main thread only, apart from the render callback.
//
// See docs/plans/nano-q-ios.md, "Audio".

#include <AudioToolbox/AudioToolbox.h>

#include <atomic>
#include <cstdint>
#include <vector>

namespace nanoq::ios
{
class ClapHost;

class AudioIO
{
public:
    explicit AudioIO(ClapHost& host);
    ~AudioIO();

    AudioIO(const AudioIO&) = delete;
    AudioIO& operator=(const AudioIO&) = delete;

    /// Configures and activates the session, asking for the microphone first. Plays as soon as
    /// the answer comes; without permission the input is silence.
    void Start();
    void Stop();

    /// A session event the app observes (see AudioIO.mm): restart if the rate has moved.
    void Restart();

private:
    static OSStatus Render(void* refCon, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* timeStamp,
                           UInt32 bus, UInt32 frames, AudioBufferList* ioData);

    bool ConfigureSession();
    bool OpenUnit();
    void CloseUnit();
    bool StartUnit();
    void StopUnit();

    ClapHost& mHost;
    AudioUnit mUnit = nullptr;
    bool mRunning = false;
    // Render callbacks since the unit last started, for the start-up check in StartUnit.
    std::atomic<uint64_t> mCallbacks{0};
    // The loudest input and output sample since the unit started, for the same check.
    std::atomic<float> mInPeak{0.0f};
    std::atomic<float> mOutPeak{0.0f};
    bool mInputEnabled = false;
    double mSampleRate = 0;
    // The block the plugin is activated for: the session's buffer, rounded up to a power of
    // two. A callback that brings more (iOS raises the slice to 4096 with the screen locked) is
    // processed in blocks of this size, so the engine is prepared for what it usually gets.
    uint32_t mBlock = 0;

    // The input, pulled in each callback before the plugin runs. Sized for the most frames a
    // callback can bring (kMaxFrames), allocated before the unit starts.
    std::vector<float> mInput[2];
    AudioBufferList* mInputList = nullptr;

    // Session notifications, as NSObject observers (id, opaque here).
    void* mObservers = nullptr;
};
} // namespace nanoq::ios
