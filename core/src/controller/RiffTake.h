#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace guitarfx
{
/// How a riff take was asked for: its tempo and meter, the count-in and click, and the preset it was
/// played through.
struct RiffCaptureConfig
{
    double tempoBpm = 120.0;
    int timeSigNum = 4;
    int timeSigDen = 4;
    int bars = 1;
    int countInBars = 1;
    bool metronomeClickEnabled = true;
    std::string patternType = "click";
    std::string patternId;
    std::string beatPattern; // e.g. "HLLL" (H=High L=Low S=Silent)
    std::string presetId;
    std::string presetName;
};

/// A riff take on the message thread: one being recorded (no audio yet, see RiffRecorder) or one
/// finished, imported or loaded for editing, waiting to be trimmed, previewed or saved. Never touched
/// by the audio thread; a finished take is replaced whole, not edited in place.
struct RiffTake
{
    bool complete = false;
    std::string takeId;
    RiffCaptureConfig config;
    std::vector<float> left;
    std::vector<float> right;
    double sampleRate = 0.0;
    int bitsPerSample = 16;
    std::chrono::steady_clock::time_point startedAt;
    std::chrono::steady_clock::time_point endedAt;
};
} // namespace guitarfx
