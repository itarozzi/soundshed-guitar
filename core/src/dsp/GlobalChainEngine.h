#pragma once

#include "dsp/DspReaper.h"
#include "dsp/SignalGraphExecutor.h"
#include "presets/PresetTypes.h"

#include <atomic>
#include <map>
#include <optional>
#include <string>

namespace guitarfx
{
class ResourceLibrary;

/// What every executor the mixer builds starts from: where resources resolve, this plugin
/// instance's node-type config defaults (NAM quality), the diagnostics switch, and the
/// stream format — which is only known once the mixer has been prepared.
struct ExecutorSetup
{
    ResourceLibrary* resourceLibrary = nullptr;
    const std::map<std::string, std::map<std::string, std::string>>* nodeTypeConfigDefaults = nullptr;
    bool signalDiagnostics = false;
    bool prepared = false;
    double sampleRate = 44100.0;
    int maxBlockSize = 512;
};

/// The global pre-chain (input → gate → transpose) and post-chain (EQ → doubler → output)
/// executors over their lifetime: built from the global chain config, rebuilt when its
/// graphs change, staged off the DSP lock and swapped in under it, and the outgoing pair
/// handed to the reaper.
///
/// The config's scalars — gains, mono, limiter — belong to the mixer, which applies them;
/// the edit rules for the chain's nodes are GlobalChainEditor's. This class is what the
/// executors are and when they are replaced.
class GlobalChainEngine
{
  public:
    explicit GlobalChainEngine(DspReaper& reaper) : mReaper(reaper)
    {
    }

    GlobalChainEngine(const GlobalChainEngine&) = delete;
    GlobalChainEngine& operator=(const GlobalChainEngine&) = delete;

    /// Takes the other engine's config, executors and rebuild flag, as a moved mixer does.
    /// Each engine keeps its own reaper, and a staged swap is left behind.
    void TakeStateFrom(GlobalChainEngine& other);

    [[nodiscard]] GlobalSignalChainConfig& Config() noexcept
    {
        return mConfig;
    }

    [[nodiscard]] const GlobalSignalChainConfig& Config() const noexcept
    {
        return mConfig;
    }

    [[nodiscard]] SignalGraphExecutor& Pre() noexcept
    {
        return mPre;
    }

    [[nodiscard]] const SignalGraphExecutor& Pre() const noexcept
    {
        return mPre;
    }

    [[nodiscard]] SignalGraphExecutor& Post() noexcept
    {
        return mPost;
    }

    [[nodiscard]] const SignalGraphExecutor& Post() const noexcept
    {
        return mPost;
    }

    /// Asks for the next EnsureUpToDate() to rebuild both executors.
    void MarkNeedsRebuild() noexcept
    {
        mNeedsRebuild.store(true, std::memory_order_release);
    }

    /// Rebuilds both executors in place if a rebuild is pending and the stream format is
    /// known. Returns whether it rebuilt. Message thread; allocates, so never from the
    /// audio thread.
    bool EnsureUpToDate(const ExecutorSetup& setup);

    /// Takes an already-normalised config, and asks for a rebuild only if its graphs differ
    /// from the running ones. Returns whether they did.
    bool Adopt(GlobalSignalChainConfig normalized);

    /// First half of a swap, with no DSP lock held: records `normalized` for the commit and,
    /// only when a rebuild is needed, builds the replacement executors. Returns whether it
    /// built them. Skipping the build is the common case: global settings do not come from
    /// presets, so most preset loads pass an identical config.
    bool PrepareSwap(GlobalSignalChainConfig normalized, const ExecutorSetup& setup);

    /// Second half, under the DSP lock: installs the staged config and, if they were built,
    /// the staged executors, retiring the outgoing pair. Returns false when nothing was
    /// staged, so there is nothing for the caller to apply.
    bool CommitSwap();

  private:
    void Rebuild(const ExecutorSetup& setup);
    /// Loads `graph` into `executor` and prepares it, the same way for a rebuild in place and
    /// for a staged replacement. The input trim is the pre-chain's alone.
    static void Load(SignalGraphExecutor& executor, const SignalGraph& graph, const double* inputTrimDb,
                     const ExecutorSetup& setup);

    DspReaper& mReaper;
    GlobalSignalChainConfig mConfig;
    SignalGraphExecutor mPre;
    SignalGraphExecutor mPost;
    std::atomic<bool> mNeedsRebuild{true};

    // Staged by PrepareSwap(). The two executor slots stay empty when the graphs were
    // unchanged and only the scalars need applying.
    std::optional<GlobalSignalChainConfig> mPendingConfig;
    std::optional<SignalGraphExecutor> mPendingPre;
    std::optional<SignalGraphExecutor> mPendingPost;
};
} // namespace guitarfx
