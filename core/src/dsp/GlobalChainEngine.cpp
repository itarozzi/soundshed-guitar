#include "dsp/GlobalChainEngine.h"

namespace guitarfx
{
void GlobalChainEngine::TakeStateFrom(GlobalChainEngine& other)
{
    mConfig = std::move(other.mConfig);
    mPre = std::move(other.mPre);
    mPost = std::move(other.mPost);
    mNeedsRebuild.store(other.mNeedsRebuild.load(std::memory_order_acquire), std::memory_order_release);
}

void GlobalChainEngine::Load(SignalGraphExecutor& executor, const SignalGraph& graph, const double* inputTrimDb,
                             const ExecutorSetup& setup)
{
    executor.SetResourceLibrary(setup.resourceLibrary);

    if (setup.nodeTypeConfigDefaults != nullptr)
    {
        executor.SeedNodeTypeConfigDefaults(*setup.nodeTypeConfigDefaults);
    }

    executor.SetGraph(graph);

    if (inputTrimDb != nullptr)
    {
        executor.SetInputTrim(*inputTrimDb);
    }

    executor.SetSignalDiagnosticsEnabled(setup.signalDiagnostics);
    executor.Prepare(setup.sampleRate, setup.maxBlockSize);
}

bool GlobalChainEngine::EnsureUpToDate(const ExecutorSetup& setup)
{
    if (!setup.prepared || !mNeedsRebuild.load(std::memory_order_acquire))
    {
        return false;
    }

    Rebuild(setup);
    return true;
}

void GlobalChainEngine::Rebuild(const ExecutorSetup& setup)
{
    mPre.Reset();
    mPost.Reset();

    auto preGraph = mConfig.BuildPreChainGraph();

    if (preGraph.nodes.empty() && preGraph.edges.empty())
    {
        preGraph = GlobalSignalChainConfig::BuildDefaultPreChainGraph();
        mConfig.preChainGraph = preGraph;
    }

    Load(mPre, preGraph, &mConfig.inputGain, setup);

    auto postGraph = mConfig.BuildPostChainGraph();

    if (postGraph.nodes.empty() && postGraph.edges.empty())
    {
        postGraph = GlobalSignalChainConfig::BuildDefaultPostChainGraph();
        mConfig.postChainGraph = postGraph;
    }

    Load(mPost, postGraph, nullptr, setup);

    mNeedsRebuild.store(false, std::memory_order_release);
}

bool GlobalChainEngine::Adopt(GlobalSignalChainConfig normalized)
{
    // Rebuilding tears down and recreates both executors — construction, resource loading
    // and allocation. Skip it entirely when the graphs are unchanged, which is the common
    // case: global settings are per-instance state and do not come from presets, so most
    // preset loads pass through a config identical to the one already running.
    const bool graphsChanged =
        normalized.preChainGraph != mConfig.preChainGraph || normalized.postChainGraph != mConfig.postChainGraph;

    mConfig = std::move(normalized);

    if (graphsChanged)
    {
        MarkNeedsRebuild();
    }

    return graphsChanged;
}

bool GlobalChainEngine::PrepareSwap(GlobalSignalChainConfig normalized, const ExecutorSetup& setup)
{
    const bool graphsChanged =
        normalized.preChainGraph != mConfig.preChainGraph || normalized.postChainGraph != mConfig.postChainGraph;
    const bool rebuildNeeded = setup.prepared && (graphsChanged || mNeedsRebuild.load(std::memory_order_acquire));

    mPendingPre.reset();
    mPendingPost.reset();
    mPendingConfig = std::move(normalized);

    if (!rebuildNeeded)
    {
        return false;
    }

    // Expensive part: runs on the caller's thread with no DSP lock held.
    Load(mPendingPre.emplace(), mPendingConfig->preChainGraph, &mPendingConfig->inputGain, setup);
    Load(mPendingPost.emplace(), mPendingConfig->postChainGraph, nullptr, setup);
    return true;
}

bool GlobalChainEngine::CommitSwap()
{
    if (!mPendingConfig.has_value())
    {
        return false;
    }

    mConfig = std::move(*mPendingConfig);
    mPendingConfig.reset();

    if (mPendingPre.has_value() && mPendingPost.has_value())
    {
        // Hand the outgoing executors to the reaper rather than destroying them here: the
        // audio thread try_locks the DSP mutex and outputs silence when it cannot take it,
        // so freeing node state under that lock is an audible dropout.
        //
        // Residual: the move-assignment below stops any worker threads the outgoing executor
        // owned, and SignalGraphExecutor's move does not transfer them, so that join happens
        // under the caller's DSP lock. It is a wake-and-join of parked threads (bounded, tens
        // of microseconds) and is zero for the linear default chains, which never start
        // workers. Preset instances avoid this entirely by being held via unique_ptr.
        mReaper.RetireExecutor(mPre);
        mReaper.RetireExecutor(mPost);

        mPre = std::move(*mPendingPre);
        mPost = std::move(*mPendingPost);
        mNeedsRebuild.store(false, std::memory_order_release);
    }

    mPendingPre.reset();
    mPendingPost.reset();
    return true;
}
} // namespace guitarfx
