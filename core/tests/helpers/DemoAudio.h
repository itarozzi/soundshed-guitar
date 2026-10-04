#pragma once

/**
 * @file DemoAudio.h
 * @brief The demo clips in core/ui/demo, for tests that play real guitar through the DSP.
 *
 * The clips are what the app's demo preview plays (core/ui/demo/clips.json). The long DI guitar
 * take ships as an MP3 (the riffs stay WAVs), so a clip is read through the core's AudioDecoder,
 * the decoder the app itself plays it with, rather than IRWavLoader, which reads WAV and AIFF only.
 * An MP3 decodes to the recording it was made from within the codec's error: fine for level,
 * pitch and note measurements, which is what the tests do with it, not for anything sample-exact.
 */

#include "dsp/IRWavLoader.h"
#include "util/AudioDecoder.h"
#include "util/FileIO.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

#ifndef GUITARFX_DEMO_AUDIO_DIR
    #error "GUITARFX_DEMO_AUDIO_DIR must be defined"
#endif

namespace guitarfx::test
{
/// The long DI guitar take: 110 s at 48 kHz, mono, staccato riffs that are mostly power chords.
inline constexpr const char* kDemoDiGuitar = "DI_Guitar_L.mp3";

/// Decodes a WAV, AIFF or MP3 file into interleaved float samples. False, with `out` empty, when
/// the file is missing or does not decode.
inline bool LoadDemoClipFile(const std::filesystem::path& path, guitarfx::IRWavData& out)
{
    out = {};
    const auto decoded = guitarfx::util::DecodeAudioBytes(guitarfx::util::ReadFileBytes(path));

    if (!decoded || decoded->channelSamples.empty() || decoded->sampleRate <= 0.0)
    {
        return false;
    }

    const std::size_t channels = decoded->channelSamples.size();
    const std::size_t frames = decoded->channelSamples.front().size();

    if (frames == 0)
    {
        return false;
    }

    out.channels = static_cast<std::uint16_t>(channels);
    out.sampleRate = decoded->sampleRate;
    out.samples.assign(frames * channels, 0.0f);

    for (std::size_t channel = 0; channel < channels; ++channel)
    {
        const auto& source = decoded->channelSamples[channel];
        const std::size_t count = std::min(frames, source.size());

        for (std::size_t frame = 0; frame < count; ++frame)
        {
            out.samples[frame * channels + channel] = static_cast<float>(source[frame]);
        }
    }

    return true;
}

/// A clip by its file name in core/ui/demo.
inline bool LoadDemoClip(const char* file, guitarfx::IRWavData& out)
{
    return LoadDemoClipFile(std::filesystem::path(GUITARFX_DEMO_AUDIO_DIR) / file, out);
}

/// A clip by its file name in core/ui/demo, downmixed to mono, with its sample rate. Empty, with
/// the rate 0, if it does not load.
inline std::vector<float> LoadDemoClipMono(const char* file, double& sampleRate)
{
    guitarfx::IRWavData data;
    std::vector<float> mono;
    sampleRate = 0.0;

    if (LoadDemoClip(file, data))
    {
        guitarfx::irwav::DownmixToMono(data, mono);
        sampleRate = data.sampleRate;
    }

    return mono;
}
} // namespace guitarfx::test
