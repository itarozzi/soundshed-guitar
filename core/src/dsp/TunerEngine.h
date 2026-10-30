#pragma once

#include "dsp/ChromaAnalyzer.h"
#include "dsp/PitchTracker.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace guitarfx
{
/// Tracks the raw input's pitch on the audio thread and reports it from a worker thread.
///
/// Process() runs the shared PitchTracker (a detection every 5 ms), its range taken down to a
/// bass's low C# (PitchTracker::kBassLowestHz), and, once per reading, averages
/// that reading's detections and hands the result to the worker, which names the note against the
/// reference pitch and calls the callback. A reading comes every 2048 samples at 48 kHz (about
/// 43 ms at any rate). Averaging the 5 ms detections over it reads a held note with a third to a
/// tenth of the scatter of the single 85 ms YIN window it replaces, so the tuner needs no longer
/// window of its own.
///
/// It also listens for the Scale Guide (SetListening): the same readings without the tuner being
/// on, so a player whose tuner mutes the output still hears themselves, and optionally a chroma
/// vector of the last ~170 ms (ChromaAnalyzer), worked out on the worker, for chord matching.
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
        /// The input level over the reading, dBFS.
        double levelDb = -120.0;
        /// Set only while listening with chroma (SetListening): energy per pitch class, C first.
        bool hasChroma = false;
        ChromaAnalyzer::Chroma chroma{};
    };

    using Callback = std::function<void(const Result&)>;

    TunerEngine() = default;
    ~TunerEngine();
    TunerEngine(const TunerEngine&) = delete;
    TunerEngine& operator=(const TunerEngine&) = delete;

    void Prepare(double sampleRate);
    void SetEnabled(bool enabled);

    [[nodiscard]] bool IsEnabled() const noexcept
    {
        return mEnabled;
    }

    /// The Scale Guide's LISTEN: readings without the tuner on (and without muting), with a chroma
    /// vector each reading when `chroma` is set.
    void SetListening(bool listening, bool chroma);

    [[nodiscard]] bool IsListening() const noexcept
    {
        return mListening;
    }

    /// Reading the input for the tuner or for LISTEN.
    [[nodiscard]] bool IsActive() const noexcept
    {
        return mEnabled || mListening;
    }

    void SetCallback(Callback callback);
    void SetReferenceFrequency(double frequency);

    [[nodiscard]] double GetReferenceFrequency() const noexcept
    {
        return mReferenceFrequency;
    }

    void SetLiveMode(bool enabled) noexcept
    {
        mLiveMode = enabled;
    }

    [[nodiscard]] bool IsLiveMode() const noexcept
    {
        return mLiveMode;
    }

    void Process(const float* input, int numSamples);

  private:
    /// One reading's worth of tracking, handed from the audio thread to the worker.
    struct Reading
    {
        double frequency = 0.0;    ///< the mean of the reading's detections, 0 when none found a pitch
        double rawFrequency = 0.0; ///< the latest of those detections' own estimates
        double rms = 0.0;
        bool hasSamples = false; ///< mPendingSamples holds the chroma window for this reading
    };

    static constexpr double kReadingSeconds = 2048.0 / 48000.0;

    void StartWorker();
    void StopWorker();
    void WorkerLoop();
    void ResetReading();
    void Restart();
    void AddDetection();
    void QueueReading();
    [[nodiscard]] static Result FrequencyToNote(double frequency, double referenceFrequency);

    double mSampleRate = 44100.0;
    bool mEnabled = false;
    bool mListening = false;
    bool mChroma = false;
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
    std::vector<float> mRing; ///< the last ChromaAnalyzer window of input, when listening with chroma
    std::size_t mRingPos = 0;

    // Handoff to the worker, under mAnalysisMutex
    std::mutex mAnalysisMutex;
    std::condition_variable mAnalysisCv;
    std::thread mWorkerThread;
    bool mWorkerQuit = false;
    bool mAnalysisPending = false;
    Reading mPendingReading;
    std::vector<float> mPendingSamples;
    double mAnalysisReferenceFrequency = 440.0;
    std::uint64_t mQueuedGeneration = 0;
    std::atomic<std::uint64_t> mAnalysisGeneration{0};

    // Worker only
    std::unique_ptr<ChromaAnalyzer> mChromaAnalyzer;
    std::vector<float> mWorkerSamples;
};
} // namespace guitarfx
