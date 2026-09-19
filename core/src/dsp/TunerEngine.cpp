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
    mOrderedBuffer.resize(kBufferSize, 0.0);
    mAnalysisWriteBuffer.resize(kBufferSize, 0.0);
    mAnalysisReadBuffer.resize(kBufferSize, 0.0);
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

            std::swap(mAnalysisReadBuffer, mAnalysisWriteBuffer);
            referenceFrequency = mAnalysisReferenceFrequency;
            queuedGeneration = mQueuedGeneration;
            mAnalysisPending = false;
            callback = mCallback;
        }

        if (!callback || mAnalysisReadBuffer.empty())
        {
            continue;
        }

        double sumSq = 0.0;
        for (const auto sample : mAnalysisReadBuffer)
        {
            sumSq += sample * sample;
        }

        const double rms = std::sqrt(sumSq / static_cast<double>(mAnalysisReadBuffer.size()));
        const double frequency = DetectPitch(mAnalysisReadBuffer);
        Result result = FrequencyToNote(frequency, referenceFrequency);
        result.debugRms = rms;
        result.debugRawFreq = frequency;

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
        mBuffer.resize(kBufferSize, 0.0);
        std::fill(mBuffer.begin(), mBuffer.end(), 0.0);
        mOrderedBuffer.resize(kBufferSize, 0.0);
        mAnalysisWriteBuffer.resize(kBufferSize, 0.0);
        mAnalysisReadBuffer.resize(kBufferSize, 0.0);
        mBufferWriteIndex = 0;
        mSampleCounter = 0;
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

void TunerEngine::Process(const float* input, int numSamples)
{
    if (!mEnabled || !input)
    {
        return;
    }

    // Do not allocate on the audio thread if the control thread has not provisioned buffers.
    if (mBuffer.size() != kBufferSize || mOrderedBuffer.size() != kBufferSize)
    {
        return;
    }

    for (int i = 0; i < numSamples; ++i)
    {
        mBuffer[mBufferWriteIndex] = static_cast<double>(input[i]);
        mBufferWriteIndex = (mBufferWriteIndex + 1) % kBufferSize;
        ++mSampleCounter;
    }

    if (mSampleCounter >= kUpdateInterval)
    {
        mSampleCounter = 0;

        for (std::size_t i = 0; i < kBufferSize; ++i)
        {
            mOrderedBuffer[i] = mBuffer[(mBufferWriteIndex + i) % kBufferSize];
        }

        bool queuedForAnalysis = false;
        {
            std::unique_lock<std::mutex> lock(mAnalysisMutex, std::try_to_lock);
            if (lock.owns_lock() && mAnalysisWriteBuffer.size() == kBufferSize)
            {
                std::copy(mOrderedBuffer.begin(), mOrderedBuffer.end(), mAnalysisWriteBuffer.begin());
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
}

double TunerEngine::DetectPitch(const std::vector<double>& samples) const
{
    // Autocorrelation-based pitch detection (YIN-inspired algorithm).
    const std::size_t n = samples.size();
    if (n < 2)
    {
        return 0.0;
    }

    double sumSquares = 0.0;
    for (const auto& sample : samples)
    {
        sumSquares += sample * sample;
    }

    const double rms = std::sqrt(sumSquares / static_cast<double>(n));
    if (rms < 0.003)
    {
        return 0.0;
    }

    // Search from 50Hz (low tunings) to 1500Hz (F#6).
    const int minPeriod = static_cast<int>(mSampleRate / 1500.0);
    const int maxPeriod = static_cast<int>(mSampleRate / 50.0);
    if (maxPeriod >= static_cast<int>(n / 2) || minPeriod < 2)
    {
        return 0.0;
    }

    std::vector<double> diff(static_cast<std::size_t>(maxPeriod) + 1, 0.0);
    for (int tau = minPeriod; tau <= maxPeriod; ++tau)
    {
        double sum = 0.0;
        for (std::size_t i = 0; i < n - static_cast<std::size_t>(tau); ++i)
        {
            const double delta = samples[i] - samples[i + tau];
            sum += delta * delta;
        }
        diff[static_cast<std::size_t>(tau)] = sum;
    }

    std::vector<double> cmndf(static_cast<std::size_t>(maxPeriod) + 1, 1.0);
    double runningSum = 0.0;
    for (int tau = minPeriod; tau <= maxPeriod; ++tau)
    {
        runningSum += diff[static_cast<std::size_t>(tau)];
        if (runningSum > 0.0)
        {
            cmndf[static_cast<std::size_t>(tau)] =
                diff[static_cast<std::size_t>(tau)] * static_cast<double>(tau) / runningSum;
        }
    }

    constexpr double threshold = 0.15;
    int bestPeriod = -1;
    for (int tau = minPeriod; tau < maxPeriod; ++tau)
    {
        if (cmndf[static_cast<std::size_t>(tau)] < threshold)
        {
            while (tau + 1 <= maxPeriod &&
                   cmndf[static_cast<std::size_t>(tau + 1)] < cmndf[static_cast<std::size_t>(tau)])
            {
                ++tau;
            }
            bestPeriod = tau;
            break;
        }
    }

    if (bestPeriod < 0)
    {
        double minVal = cmndf[static_cast<std::size_t>(minPeriod)];
        bestPeriod = minPeriod;
        for (int tau = minPeriod + 1; tau <= maxPeriod; ++tau)
        {
            if (cmndf[static_cast<std::size_t>(tau)] < minVal)
            {
                minVal = cmndf[static_cast<std::size_t>(tau)];
                bestPeriod = tau;
            }
        }
        if (minVal > 0.5)
        {
            return 0.0;
        }
    }

    double period = static_cast<double>(bestPeriod);
    if (bestPeriod > minPeriod && bestPeriod < maxPeriod)
    {
        const double s0 = cmndf[static_cast<std::size_t>(bestPeriod - 1)];
        const double s1 = cmndf[static_cast<std::size_t>(bestPeriod)];
        const double s2 = cmndf[static_cast<std::size_t>(bestPeriod + 1)];
        const double denom = 2.0 * (2.0 * s1 - s0 - s2);
        if (std::abs(denom) > 1e-10)
        {
            period += (s2 - s0) / denom;
        }
    }

    return mSampleRate / period;
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
