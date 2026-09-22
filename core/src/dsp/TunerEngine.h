#pragma once

#include "dsp/PitchTracker.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace guitarfx
{
/// Tracks the raw input's pitch on the audio thread and reports it from a worker thread.
///
/// Process() runs the shared PitchTracker (a detection every 5 ms) and, once per reading, averages
/// that reading's detections and hands the result to the worker, which names the note against the
/// reference pitch and calls the callback. A reading comes every 2048 samples at 48 kHz (about
/// 43 ms at any rate). Averaging the 5 ms detections over it reads a held note with a third to a
/// tenth of the scatter of the single 85 ms YIN window it replaces, so the tuner needs no longer
/// window of its own.
///
/// Control methods are called under the mixer's DSP lock; Process() never allocates or waits.
class TunerEngine
{
  public:
    struct Result
    {
        std::string noteName;
        int octave = 0;
        double frequency = 0.0;
        double centOffset = 0.0;
        double confidence = 0.0;
        bool detected = false;
        double debugRms = 0.0;
        double debugRawFreq = 0.0;
    };

    using Callback = std::function<void(const Result&)>;

    TunerEngine() = default;
    ~TunerEngine();
    TunerEngine(const TunerEngine&) = delete;
    TunerEngine& operator=(const TunerEngine&) = delete;

    void Prepare(double sampleRate);
    void SetEnabled(bool enabled);
    [[nodiscard]] bool IsEnabled() const noexcept { return mEnabled; }
    void SetCallback(Callback callback);
    void SetReferenceFrequency(double frequency);
    [[nodiscard]] double GetReferenceFrequency() const noexcept { return mReferenceFrequency; }
    void SetLiveMode(bool enabled) noexcept { mLiveMode = enabled; }
    [[nodiscard]] bool IsLiveMode() const noexcept { return mLiveMode; }
    void Process(const float* input, int numSamples);

  private:
    /// One reading's worth of tracking, handed from the audio thread to the worker.
    struct Reading
    {
        double frequency = 0.0;    ///< the mean of the reading's detections, 0 when none found a pitch
        double rawFrequency = 0.0; ///< the latest of those detections' own estimates
        double rms = 0.0;
    };

    static constexpr double kReadingSeconds = 2048.0 / 48000.0;

    void StartWorker();
    void StopWorker();
    void WorkerLoop();
    void ResetReading();
    void AddDetection();
    void QueueReading();
    [[nodiscard]] static Result FrequencyToNote(double frequency, double referenceFrequency);

    double mSampleRate = 44100.0;
    bool mEnabled = false;
    bool mLiveMode = true;
    double mReferenceFrequency = 440.0;
    Callback mCallback;

    // Audio thread
    PitchTracker mTracker;
    std::uint64_t mDetectionsSeen = 0;
    std::size_t mReadingLength = 2048;
    std::size_t mSampleCounter = 0;
    double mReadingEnergy = 0.0;
    double mFrequencySum = 0.0;
    int mFrequencyCount = 0;
    double mRawFrequency = 0.0;

    // Handoff to the worker, under mAnalysisMutex
    std::mutex mAnalysisMutex;
    std::condition_variable mAnalysisCv;
    std::thread mWorkerThread;
    bool mWorkerQuit = false;
    bool mAnalysisPending = false;
    Reading mPendingReading;
    double mAnalysisReferenceFrequency = 440.0;
    std::uint64_t mQueuedGeneration = 0;
    std::atomic<std::uint64_t> mAnalysisGeneration{0};
};
} // namespace guitarfx
