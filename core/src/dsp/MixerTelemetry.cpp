#include "dsp/MixerTelemetry.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace guitarfx
{
MixerTelemetry::LevelStats MixerTelemetry::ComputeLevels(const float* left, const float* right, int numSamples) noexcept
{
    LevelStats stats;
    if (numSamples <= 0)
    {
        return stats;
    }

    double sumSquares = 0.0;
    std::size_t sampleCount = 0;
    for (const float* channel : {left, right})
    {
        if (!channel)
        {
            continue;
        }

        for (int i = 0; i < numSamples; ++i)
        {
            const float value = channel[i];
            const float absValue = std::abs(value);
            stats.peak = std::max(stats.peak, static_cast<double>(absValue));
            sumSquares += static_cast<double>(value) * static_cast<double>(value);
            if (absValue > 1.0f)
            {
                ++stats.clipCount;
            }
        }
        sampleCount += static_cast<std::size_t>(numSamples);
    }

    if (sampleCount > 0)
    {
        stats.rms = std::sqrt(sumSquares / static_cast<double>(sampleCount));
    }
    return stats;
}

MixerTelemetry::LevelStats MixerTelemetry::ReadLevels(const AtomicLevelStats& source) noexcept
{
    LevelStats stats;
    stats.peak = source.peak.load(std::memory_order_relaxed);
    stats.rms = source.rms.load(std::memory_order_relaxed);
    stats.clipCount = source.clipCount.load(std::memory_order_relaxed);
    return stats;
}

void MixerTelemetry::WriteLevels(AtomicLevelStats& destination, const LevelStats& source) noexcept
{
    destination.peak.store(source.peak, std::memory_order_relaxed);
    destination.rms.store(source.rms, std::memory_order_relaxed);
    destination.clipCount.store(source.clipCount, std::memory_order_relaxed);
}

void MixerTelemetry::Record(Stage stage, const float* left, const float* right, int numSamples) noexcept
{
    AtomicLevelStats* destination = nullptr;
    switch (stage)
    {
    case Stage::RawInput:
        destination = &mRawInputLevels;
        break;
    case Stage::Input:
        destination = &mInputLevels;
        break;
    case Stage::Output:
        destination = &mOutputLevels;
        break;
    default:
        return;
    }
    WriteLevels(*destination, ComputeLevels(left, right, numSamples));
}

MixerTelemetry::Snapshot MixerTelemetry::GetSnapshot() const noexcept
{
    Snapshot snapshot;
    snapshot.rawInput = ReadLevels(mRawInputLevels);
    snapshot.input = ReadLevels(mInputLevels);
    snapshot.output = ReadLevels(mOutputLevels);
    return snapshot;
}

MixerTelemetry::NodeSignalLevel MixerTelemetry::ToSnapshotNode(
    const SignalGraphExecutor::NodeSignalLevel& entry, std::string_view scope, const std::string& presetId)
{
    NodeSignalLevel node;
    node.scope = scope;
    node.presetId = presetId;
    node.nodeId = entry.nodeId;
    node.nodeType = entry.nodeType;
    node.channelCount = entry.channelCount;
    node.levels.peak = entry.peak;
    node.levels.rms = entry.rms;
    node.levels.clipCount = entry.clipCount;
    node.analyzer = entry.analyzer;
    return node;
}

void MixerTelemetry::CopyFrom(const MixerTelemetry& other) noexcept
{
    SetEnabled(other.IsEnabled());
    mOversizedBlockCount.store(other.GetOversizedBlockCount(), std::memory_order_relaxed);
    WriteLevels(mRawInputLevels, ReadLevels(other.mRawInputLevels));
    WriteLevels(mInputLevels, ReadLevels(other.mInputLevels));
    WriteLevels(mOutputLevels, ReadLevels(other.mOutputLevels));
}
} // namespace guitarfx
