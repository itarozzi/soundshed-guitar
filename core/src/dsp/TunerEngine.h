#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace guitarfx
{
/// Captures the raw input on the audio thread and analyzes pitch on a worker thread.
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
    static constexpr std::size_t kBufferSize = 4096;
    static constexpr std::size_t kUpdateInterval = 2048;

    void StartWorker();
    void StopWorker();
    void WorkerLoop();
    [[nodiscard]] double DetectPitch(const std::vector<double>& samples) const;
    [[nodiscard]] static Result FrequencyToNote(double frequency, double referenceFrequency);

    double mSampleRate = 44100.0;
    bool mEnabled = false;
    bool mLiveMode = true;
    double mReferenceFrequency = 440.0;
    Callback mCallback;
    std::vector<double> mBuffer;
    std::vector<double> mOrderedBuffer;
    std::vector<double> mAnalysisWriteBuffer;
    std::vector<double> mAnalysisReadBuffer;
    std::size_t mBufferWriteIndex = 0;
    std::size_t mSampleCounter = 0;
    std::mutex mAnalysisMutex;
    std::condition_variable mAnalysisCv;
    std::thread mWorkerThread;
    bool mWorkerQuit = false;
    bool mAnalysisPending = false;
    double mAnalysisReferenceFrequency = 440.0;
    std::uint64_t mQueuedGeneration = 0;
    std::atomic<std::uint64_t> mAnalysisGeneration{0};
};
} // namespace guitarfx
