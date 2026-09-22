#include "dsp/TunerEngine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace guitarfx
{
namespace
{
constexpr std::array<const char*, 12> kNoteNames = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
constexpr std::array<const char*, 12> kNoteNamesFlat = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};
} // namespace

TunerEngine::~TunerEngine()
{
    StopWorker();
}

void TunerEngine::Prepare(double sampleRate)
{
    mSampleRate = sampleRate;
    mTracker.Prepare(sampleRate);
    mReadingLength = static_cast<std::size_t>(std::max(1L, std::lround(sampleRate * kReadingSeconds)));
    mDetectionsSeen = 0;
    ResetReading();
}

void TunerEngine::StartWorker()
{
    if (mWorkerThread.joinable())
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mAnalysisMutex);
        mWorkerQuit = false;
        mAnalysisPending = false;
    }

    mWorkerThread = std::thread([this] { WorkerLoop(); });
}

void TunerEngine::StopWorker()
{
    if (!mWorkerThread.joinable())
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mAnalysisMutex);
        mWorkerQuit = true;
        mAnalysisPending = false;
    }
    mAnalysisCv.notify_all();
    mWorkerThread.join();
}

void TunerEngine::WorkerLoop()
{
    while (true)
    {
        Reading reading;
        double referenceFrequency = 440.0;
        std::uint64_t queuedGeneration = 0;
        Callback callback;

        {
            std::unique_lock<std::mutex> lock(mAnalysisMutex);
            mAnalysisCv.wait(lock, [&] { return mWorkerQuit || mAnalysisPending; });

            if (mWorkerQuit)
            {
                return;
            }

            reading = mPendingReading;
            referenceFrequency = mAnalysisReferenceFrequency;
            queuedGeneration = mQueuedGeneration;
            mAnalysisPending = false;
            callback = mCallback;
        }

        if (!callback)
        {
            continue;
        }

        Result result = FrequencyToNote(reading.frequency, referenceFrequency);
        result.debugRms = reading.rms;
        result.debugRawFreq = reading.rawFrequency;

        if (queuedGeneration != mAnalysisGeneration.load(std::memory_order_acquire))
        {
            continue;
        }

        callback(result);
    }
}

void TunerEngine::SetEnabled(bool enabled)
{
    mEnabled = enabled;
    const std::uint64_t generation = mAnalysisGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;

    if (enabled)
    {
        StartWorker();
        mTracker.Reset();
        mDetectionsSeen = 0;
        ResetReading();
    }

    std::lock_guard<std::mutex> lock(mAnalysisMutex);
    mAnalysisPending = false;
    mQueuedGeneration = generation;
    mAnalysisReferenceFrequency = mReferenceFrequency;
}

void TunerEngine::SetCallback(Callback callback)
{
    std::lock_guard<std::mutex> lock(mAnalysisMutex);
    mCallback = std::move(callback);
}

void TunerEngine::SetReferenceFrequency(double frequency)
{
    mReferenceFrequency = std::clamp(frequency, 400.0, 480.0);
}

void TunerEngine::ResetReading()
{
    mSampleCounter = 0;
    mReadingEnergy = 0.0;
    mFrequencySum = 0.0;
    mFrequencyCount = 0;
    mRawFrequency = 0.0;
}

void TunerEngine::Process(const float* input, int numSamples)
{
    if (!mEnabled || !input)
    {
        return;
    }

    for (int i = 0; i < numSamples; ++i)
    {
        const float sample = input[i];
        mTracker.Push(sample);
        mReadingEnergy += static_cast<double>(sample) * static_cast<double>(sample);

        if (mTracker.DetectionCount() != mDetectionsSeen)
        {
            mDetectionsSeen = mTracker.DetectionCount();
            AddDetection();
        }

        if (++mSampleCounter >= mReadingLength)
        {
            QueueReading();
        }
    }
}

void TunerEngine::AddDetection()
{
    const double frequency = mTracker.FrequencyHz();

    if (!mTracker.HasPitch() || !(frequency > 0.0))
    {
        return;
    }

    // The tracker has accepted a different note: the reading describes only the new one.
    if (mFrequencyCount > 0 && std::abs(12.0 * std::log2(frequency * mFrequencyCount / mFrequencySum)) > 1.0)
    {
        mFrequencySum = 0.0;
        mFrequencyCount = 0;
    }

    mFrequencySum += frequency;
    ++mFrequencyCount;
    mRawFrequency = mTracker.RawFrequencyHz();
}

void TunerEngine::QueueReading()
{
    Reading reading;
    reading.frequency = mFrequencyCount > 0 ? mFrequencySum / mFrequencyCount : 0.0;
    reading.rawFrequency = mFrequencyCount > 0 ? mRawFrequency : 0.0;
    reading.rms = std::sqrt(mReadingEnergy / static_cast<double>(mSampleCounter));
    ResetReading();

    bool queuedForAnalysis = false;
    {
        std::unique_lock<std::mutex> lock(mAnalysisMutex, std::try_to_lock);

        if (lock.owns_lock())
        {
            mPendingReading = reading;
            mAnalysisReferenceFrequency = mReferenceFrequency;
            mQueuedGeneration = mAnalysisGeneration.load(std::memory_order_acquire);
            mAnalysisPending = true;
            queuedForAnalysis = true;
        }
    }

    if (queuedForAnalysis)
    {
        mAnalysisCv.notify_one();
    }
}

TunerEngine::Result TunerEngine::FrequencyToNote(double frequency, double referenceFrequency)
{
    Result result;
    if (frequency < 20.0 || frequency > 20000.0)
    {
        return result;
    }

    result.frequency = frequency;
    result.detected = true;
    const double semitonesFromA4 = 12.0 * std::log2(frequency / referenceFrequency);
    const int nearestSemitone = static_cast<int>(std::round(semitonesFromA4));
    const double nearestFrequency = referenceFrequency * std::pow(2.0, nearestSemitone / 12.0);
    result.centOffset = 1200.0 * std::log2(frequency / nearestFrequency);
    result.centOffset = std::clamp(result.centOffset, -50.0, 50.0);

    const int totalSemitones = nearestSemitone + 57;
    const int noteIndex = ((totalSemitones % 12) + 12) % 12;
    result.octave = totalSemitones / 12;
    if (totalSemitones < 0 && totalSemitones % 12 != 0)
    {
        result.octave -= 1;
    }

    const char* sharpName = kNoteNames[static_cast<std::size_t>(noteIndex)];
    const char* flatName = kNoteNamesFlat[static_cast<std::size_t>(noteIndex)];
    if (std::string(sharpName) != std::string(flatName))
    {
        result.noteName = std::string(sharpName) + "/" + std::string(flatName);
    }
    else
    {
        result.noteName = sharpName;
    }

    result.confidence = std::clamp(1.0 - std::abs(result.centOffset) / 50.0, 0.0, 1.0);
    return result;
}
} // namespace guitarfx
