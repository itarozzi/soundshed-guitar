#pragma once

#include "dsp/SignalGraphExecutor.h"
#include "dsp/SignalTelemetry.h"

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace guitarfx
{
/// Owns the lock-free level readings written by the audio thread and read by the UI thread.
class MixerTelemetry
{
  public:
    struct LevelStats
    {
        double peak = 0.0;
        double rms = 0.0;
        int clipCount = 0;
    };

    struct NodeSignalLevel
    {
        using AnalyzerTelemetry = guitarfx::AnalyzerTelemetry;

        std::string scope;
        std::string presetId;
        std::string nodeId;
        std::string nodeType;
        int channelCount = 0;
        LevelStats levels;
        std::optional<AnalyzerTelemetry> analyzer;
    };

    struct Snapshot
    {
        LevelStats rawInput;
        LevelStats input;
        LevelStats output;
        std::vector<NodeSignalLevel> nodes;
    };

    enum class Stage { RawInput, Input, Output };

    void SetEnabled(bool enabled) noexcept { mEnabled.store(enabled, std::memory_order_release); }
    [[nodiscard]] bool IsEnabled() const noexcept { return mEnabled.load(std::memory_order_acquire); }
    void Record(Stage stage, const float* left, const float* right, int numSamples) noexcept;
    [[nodiscard]] Snapshot GetSnapshot() const noexcept;
    [[nodiscard]] static NodeSignalLevel ToSnapshotNode(const SignalGraphExecutor::NodeSignalLevel& entry,
                                                        std::string_view scope, const std::string& presetId);

    void NoteOversizedBlock() noexcept { mOversizedBlockCount.fetch_add(1, std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t GetOversizedBlockCount() const noexcept
    {
        return mOversizedBlockCount.load(std::memory_order_relaxed);
    }

    // Atomics make the type intentionally non-movable. A moving mixer copies its latest readings.
    void CopyFrom(const MixerTelemetry& other) noexcept;

  private:
    struct AtomicLevelStats
    {
        std::atomic<double> peak{0.0};
        std::atomic<double> rms{0.0};
        std::atomic<int> clipCount{0};
    };

    [[nodiscard]] static LevelStats ComputeLevels(const float* left, const float* right, int numSamples) noexcept;
    [[nodiscard]] static LevelStats ReadLevels(const AtomicLevelStats& source) noexcept;
    static void WriteLevels(AtomicLevelStats& destination, const LevelStats& source) noexcept;

    std::atomic<bool> mEnabled{true};
    std::atomic<std::uint64_t> mOversizedBlockCount{0};
    AtomicLevelStats mRawInputLevels;
    AtomicLevelStats mInputLevels;
    AtomicLevelStats mOutputLevels;
};
} // namespace guitarfx
