#include "AudioIO.h"

#include "AppLog.h"
#include "ClapHost.h"

#import <AVFoundation/AVFoundation.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#if !defined(NANOQ_IOS_BUFFER_FRAMES)
 #define NANOQ_IOS_BUFFER_FRAMES 128
#endif

namespace nanoq::ios
{
namespace
{
// The most frames one render callback can ask for. iOS raises the slice to 4096 when the screen
// locks, so that is what the unit is told and the input is sized for.
constexpr UInt32 kMaxFrames = 4096;
constexpr UInt32 kChannels = 2;
constexpr AudioUnitElement kOutputBus = 0;
constexpr AudioUnitElement kInputBus = 1;

// What the JUCE apps ask for: 128 frames (2.7 ms at 48 kHz), and 256 in a legacy (iOS 12)
// build, where an A7 glitched at less. The session may give something else.
constexpr double kPreferredRate = 48000.0;
constexpr int kPreferredFrames = NANOQ_IOS_BUFFER_FRAMES;

bool Check(OSStatus status, const char* what)
{
    if (status == noErr)
        return true;
    AppLog(@"Nano Q audio: %s failed (%d)", what, int(status));
    return false;
}

AudioStreamBasicDescription StereoFloat(double rate)
{
    AudioStreamBasicDescription f{};
    f.mSampleRate = rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
    f.mBitsPerChannel = 32;
    f.mChannelsPerFrame = kChannels;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = sizeof(float); // per channel, as the format is non-interleaved
    f.mBytesPerPacket = sizeof(float);
    return f;
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
// AVAudioApplication replaces these in iOS 17; the session's own still work, and reach iOS 12.
bool HasRecordPermission()
{
    return [AVAudioSession sharedInstance].recordPermission == AVAudioSessionRecordPermissionGranted;
}

void RequestRecordPermission(void (^done)(BOOL))
{
    [[AVAudioSession sharedInstance] requestRecordPermission:done];
}
#pragma clang diagnostic pop
} // namespace

AudioIO::AudioIO(ClapHost& host) : mHost(host)
{
    for (auto& channel : mInput)
        channel.assign(kMaxFrames, 0.0f);

    // An AudioBufferList with room for both channels' buffers.
    mInputList = static_cast<AudioBufferList*>(calloc(1, offsetof(AudioBufferList, mBuffers) + sizeof(AudioBuffer) * kChannels));
    mInputList->mNumberBuffers = kChannels;

    AudioIO* self = this;
    auto* center = [NSNotificationCenter defaultCenter];
    auto* session = [AVAudioSession sharedInstance];
    NSOperationQueue* main = [NSOperationQueue mainQueue];

    id interruption = [center addObserverForName:AVAudioSessionInterruptionNotification
                                          object:session
                                           queue:main
                                      usingBlock:^(NSNotification* note) {
                                          auto type = AVAudioSessionInterruptionType(
                                              [note.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue]);
                                          if (type == AVAudioSessionInterruptionTypeBegan)
                                          {
                                              AppLog(@"Nano Q audio: interrupted");
                                              self->StopUnit();
                                              self->mHost.Deactivate();
                                          }
                                          else
                                          {
                                              AppLog(@"Nano Q audio: interruption ended");
                                              self->Restart();
                                          }
                                      }];
    id route = [center addObserverForName:AVAudioSessionRouteChangeNotification
                                   object:session
                                    queue:main
                               usingBlock:^(NSNotification*) {
                                   self->Restart();
                               }];
    // The audio server restarted: every unit and the session's setup are gone.
    id reset = [center addObserverForName:AVAudioSessionMediaServicesWereResetNotification
                                   object:session
                                    queue:main
                               usingBlock:^(NSNotification*) {
                                   AppLog(@"Nano Q audio: media services were reset");
                                   self->StopUnit();
                                   self->mHost.Deactivate();
                                   self->CloseUnit();
                                   self->mSampleRate = 0;
                                   self->Restart();
                               }];
    mObservers = (__bridge_retained void*)@[interruption, route, reset];
}

AudioIO::~AudioIO()
{
    NSArray* observers = (__bridge_transfer NSArray*)mObservers;
    for (id observer in observers)
        [[NSNotificationCenter defaultCenter] removeObserver:observer];
    Stop();
    free(mInputList);
}

void AudioIO::Start()
{
    AudioIO* self = this;
    RequestRecordPermission(^(BOOL) {
        dispatch_async(dispatch_get_main_queue(), ^{
            self->mSampleRate = 0;
            self->Restart();
        });
    });
}

void AudioIO::Stop()
{
    StopUnit();
    mHost.Deactivate();
    CloseUnit();
    [[AVAudioSession sharedInstance] setActive:NO error:nil];
}

void AudioIO::Restart()
{
    auto* session = [AVAudioSession sharedInstance];
    const bool wantInput = HasRecordPermission() && session.isInputAvailable;

    // Nothing that matters moved: a route change between two outputs at the same rate is
    // RemoteIO's to follow by itself.
    if (mRunning && session.sampleRate == mSampleRate && wantInput == mInputEnabled)
        return;

    StopUnit();
    mHost.Deactivate();
    CloseUnit();

    if (!ConfigureSession() || !OpenUnit())
        return;
    if (!mHost.Activate(mSampleRate, mBlock))
    {
        AppLog(@"Nano Q audio: the plugin did not activate at %.0f Hz", mSampleRate);
        return;
    }
    StartUnit();
}

bool AudioIO::ConfigureSession()
{
    auto* session = [AVAudioSession sharedInstance];
    NSError* error = nil;

    // PlayAndRecord, mixing with other apps (a backing track), the speaker rather than the
    // receiver, and Bluetooth and AirPlay outputs allowed: the set JUCE's iOS device opens.
    const AVAudioSessionCategoryOptions options = AVAudioSessionCategoryOptionMixWithOthers |
                                                  AVAudioSessionCategoryOptionDefaultToSpeaker |
                                                  AVAudioSessionCategoryOptionAllowBluetoothA2DP |
                                                  AVAudioSessionCategoryOptionAllowAirPlay;
    if (![session setCategory:AVAudioSessionCategoryPlayAndRecord withOptions:options error:&error])
    {
        AppLog(@"Nano Q audio: setCategory failed: %@", error);
        return false;
    }
    [session setPreferredSampleRate:kPreferredRate error:nil];
    [session setPreferredIOBufferDuration:kPreferredFrames / kPreferredRate error:nil];
    if (![session setActive:YES error:&error])
    {
        AppLog(@"Nano Q audio: setActive failed: %@", error);
        return false;
    }

    mSampleRate = session.sampleRate;
    mInputEnabled = HasRecordPermission() && session.isInputAvailable;
    const auto frames = UInt32(std::lround(session.IOBufferDuration * mSampleRate));
    mBlock = 64;
    while (mBlock < frames && mBlock < kMaxFrames)
        mBlock *= 2;
    AppLog(@"Nano Q audio: %.0f Hz, %.1f ms buffer (blocks of %u), input %s", mSampleRate,
          session.IOBufferDuration * 1000.0, unsigned(mBlock), mInputEnabled ? "on" : "off");
    return true;
}

bool AudioIO::OpenUnit()
{
    AudioComponentDescription desc{};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_RemoteIO;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent component = AudioComponentFindNext(nullptr, &desc);
    if (component == nullptr || !Check(AudioComponentInstanceNew(component, &mUnit), "AudioComponentInstanceNew"))
    {
        mUnit = nullptr;
        return false;
    }

    const UInt32 enableInput = mInputEnabled ? 1 : 0;
    Check(AudioUnitSetProperty(mUnit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, kInputBus, &enableInput,
                               sizeof(enableInput)),
          "EnableIO (input)");

    // What the app reads from the input bus and writes to the output bus. The unit converts
    // to and from the hardware's format (a mono microphone included).
    const auto format = StereoFloat(mSampleRate);
    Check(AudioUnitSetProperty(mUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, kOutputBus, &format,
                               sizeof(format)),
          "StreamFormat (output)");
    if (mInputEnabled)
        Check(AudioUnitSetProperty(mUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, kInputBus, &format,
                                   sizeof(format)),
              "StreamFormat (input)");

    const UInt32 maxFrames = kMaxFrames;
    Check(AudioUnitSetProperty(mUnit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames,
                               sizeof(maxFrames)),
          "MaximumFramesPerSlice");

    AURenderCallbackStruct callback{Render, this};
    Check(AudioUnitSetProperty(mUnit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, kOutputBus,
                               &callback, sizeof(callback)),
          "SetRenderCallback");

    if (!Check(AudioUnitInitialize(mUnit), "AudioUnitInitialize"))
    {
        CloseUnit();
        return false;
    }
    return true;
}

void AudioIO::CloseUnit()
{
    if (mUnit == nullptr)
        return;
    AudioUnitUninitialize(mUnit);
    AudioComponentInstanceDispose(mUnit);
    mUnit = nullptr;
}

bool AudioIO::StartUnit()
{
    if (mUnit == nullptr || mRunning)
        return mRunning;
    mCallbacks = 0;
    mInPeak = 0.0f;
    mOutPeak = 0.0f;
    mRunning = Check(AudioOutputUnitStart(mUnit), "AudioOutputUnitStart");

    // A unit that starts without error can still never call back (no output route, the session
    // taken by another app): say so in the log, where silence would otherwise be all there is.
    AudioIO* self = this;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        if (self->mRunning)
            AppLog(@"Nano Q audio: %llu render callbacks in the first 3 s, plugin %s, peak in %.4f, out %.4f",
                  (unsigned long long)self->mCallbacks.load(), self->mHost.IsActive() ? "active" : "inactive",
                  self->mInPeak.load(), self->mOutPeak.load());
    });
    return mRunning;
}

void AudioIO::StopUnit()
{
    if (mUnit != nullptr && mRunning)
        AudioOutputUnitStop(mUnit);
    mRunning = false;
}

OSStatus AudioIO::Render(void* refCon, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* timeStamp, UInt32,
                         UInt32 frames, AudioBufferList* ioData)
{
    auto& self = *static_cast<AudioIO*>(refCon);
    self.mCallbacks.fetch_add(1, std::memory_order_relaxed);
    if (ioData == nullptr || ioData->mNumberBuffers < kChannels)
        return noErr;

    float* out[kChannels] = {static_cast<float*>(ioData->mBuffers[0].mData),
                             static_cast<float*>(ioData->mBuffers[1].mData)};
    if (frames > kMaxFrames)
    {
        for (auto* channel : out)
            std::memset(channel, 0, frames * sizeof(float));
        return noErr;
    }

    const float* in[kChannels] = {self.mInput[0].data(), self.mInput[1].data()};
    bool haveInput = false;
    if (self.mInputEnabled)
    {
        for (UInt32 ch = 0; ch < kChannels; ++ch)
        {
            self.mInputList->mBuffers[ch].mNumberChannels = 1;
            self.mInputList->mBuffers[ch].mDataByteSize = frames * sizeof(float);
            self.mInputList->mBuffers[ch].mData = self.mInput[ch].data();
        }
        haveInput = AudioUnitRender(self.mUnit, flags, timeStamp, kInputBus, frames, self.mInputList) == noErr;
    }

    for (UInt32 done = 0; done < frames;)
    {
        const auto n = std::min(frames - done, self.mBlock);
        const float* inBlock[kChannels] = {in[0] + done, in[1] + done};
        float* outBlock[kChannels] = {out[0] + done, out[1] + done};
        self.mHost.Process(haveInput ? inBlock : nullptr, outBlock, n);
        done += n;
    }

    float inPeak = self.mInPeak.load(std::memory_order_relaxed);
    float outPeak = self.mOutPeak.load(std::memory_order_relaxed);
    for (UInt32 i = 0; i < frames; ++i)
    {
        if (haveInput)
            inPeak = std::max(inPeak, std::abs(in[0][i]));
        outPeak = std::max(outPeak, std::abs(out[0][i]));
    }
    self.mInPeak.store(inPeak, std::memory_order_relaxed);
    self.mOutPeak.store(outPeak, std::memory_order_relaxed);
    return noErr;
}
} // namespace nanoq::ios
