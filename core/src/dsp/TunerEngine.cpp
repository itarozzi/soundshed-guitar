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
constexpr std::array<const char*, 12> kNoteNamesFlat = {"C",  "Db", "D",  "Eb", "E",  "F",
                                                        "Gb", "G",  "Ab", "A",  "Bb", "B"};
} // namespace

TunerEngine::~TunerEngine()
{
    StopWorker();
}

void TunerEngine::Prepare(double sampleRate)
{
    mSampleRate = sampleRate;
    // Down to an extended-range bass's low C#, before Prepare() so the buffers are sized for it.
    // A held note reads the same over the longer window, which only costs following fast playing,
    // and a tuner is not used for that.
    mTracker.SetLowestFrequency(PitchTracker::kBassLowestHz);
    mTracker.Prepare(sampleRate);
    mReadingLength = static_cast<std::size_t>(std::max(1L, std::lround(sampleRate * kReadingSeconds)));
    mDetectionsSeen = 0;
    ResetReading();

    // The chroma window and its copies, sized here so the audio thread never allocates. The worker
    // makes its analyzer when it first needs one, on its own thread.
    const std::size_t window = ChromaAnalyzer::WindowFor(sampleRate);
    mRing.assign(window, 0.0f);
    mRingPos = 0;
    {
        std::lock_guard<std::mutex> lock(mAnalysisMutex);
        mPendingSamples.assign(window, 0.0f);
        mPendingReading.hasSamples = false;
    }
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

            if (reading.hasSamples)
            {
                mWorkerSamples = mPendingSamples;
            }

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
        result.levelDb = reading.rms > 1e-6 ? 20.0 * std::log10(reading.rms) : -120.0;

        if (reading.hasSamples)
        {
            if (!mChromaAnalyzer || mChromaAnalyzer->Size() != mWorkerSamples.size())
            {
                mChromaAnalyzer = std::make_unique<ChromaAnalyzer>(mWorkerSamples.size());
            }

            result.chroma = mChromaAnalyzer->Analyze(mWorkerSamples.data(), mSampleRate, referenceFrequency);
            result.hasChroma = true;
        }

        if (queuedGeneration != mAnalysisGeneration.load(std::memory_order_acquire))
        {
            continue;
        }

        callback(result);
    }
}

void TunerEngine::SetEnabled(bool enabled)
{
    const bool wasActive = IsActive();
    mEnabled = enabled;

    if (!wasActive || !IsActive())
    {
        Restart();
    }
}

void TunerEngine::SetListening(bool listening, bool chroma)
{
    const bool wasActive = IsActive();
    const bool chromaStarts = listening && chroma && !mChroma;
    mListening = listening;
    mChroma = listening && chroma;

    if (chromaStarts)
    {
        std::fill(mRing.begin(), mRing.end(), 0.0f);
        mRingPos = 0;
    }

    if (!wasActive || !IsActive())
    {
        Restart();
    }
}

/// Starts reading afresh: readings already queued are dropped, as they belong to the old state.
void TunerEngine::Restart()
{
    const std::uint64_t generation = mAnalysisGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;

    if (IsActive())
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
    if (!IsActive() || !input)
    {
        return;
    }

    const bool chroma = mChroma && !mRing.empty();

    for (int i = 0; i < numSamples; ++i)
    {
        const float sample = input[i];
        mTracker.Push(sample);

        if (chroma)
        {
            mRing[mRingPos] = sample;
            mRingPos = mRingPos + 1 == mRing.size() ? 0 : mRingPos + 1;
        }

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
            // The chroma window in time order, oldest first. Sized in Prepare(): no allocation.
            reading.hasSamples = mChroma && !mRing.empty() && mPendingSamples.size() == mRing.size();

            if (reading.hasSamples)
            {
                const auto split = mRing.begin() + static_cast<std::ptrdiff_t>(mRingPos);
                std::copy(split, mRing.end(), mPendingSamples.begin());
                std::copy(mRing.begin(), split, mPendingSamples.begin() + (mRing.end() - split));
            }

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

    if (!(frequency >= PitchTracker::kBassLowestHz && frequency <= 20000.0))
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
