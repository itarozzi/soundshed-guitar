#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>

namespace nam
{
class DSP;
}

namespace guitarfx
{
/**
 * Process-wide cache of parsed NAM model data, keyed by file identity.
 *
 * Loading a .nam file is dominated by reading it and parsing the weights blob out of
 * JSON — roughly 5-12 ms for a typical model, and every NAM node pays it twice (left and
 * right channel). That cost sits directly on the preset-switch critical path.
 *
 * nam::dspData is a plain copyable struct, and nam::get_dsp() has an overload that builds
 * a model from one. So the parse result is cached and each construction skips the file. The
 * NAM core still re-walks the config and prewarms on every build, though: 7-46 ms per model
 * (docs/nam-dsp-performance-review-2026-10-16.md).
 *
 * So where the model is a WaveNet shape the fused pass takes (dsp/nam/FusedWaveNet.h), the
 * entry also holds its packed weights, and every model built from it is a fused one sharing
 * them: a build and prewarm in well under a millisecond, and 2-4.8x less CPU while it runs.
 * Anything else is built by the NAM core from the cached dspData.
 *
 * Entries are keyed by canonical path + file size + last-write time, so editing or
 * replacing a model file invalidates its entry. Eviction is LRU against a byte budget.
 *
 * Thread-safe: loads are dispatched concurrently by SignalGraphExecutor::CreateProcessors().
 * The file parse itself runs outside the lock, so a cold load never blocks other threads;
 * two threads racing on the same uncached path may both parse it, and the second insert
 * simply wins. That is rare and strictly cheaper than serialising every load.
 */
namespace nammodelcache
{
/**
 * Construct a NAM model for `path`, reusing cached parse output when available.
 * Returns nullptr if the file cannot be read or parsed. Propagates no exceptions.
 *
 * Every production NAM model is built here, so the first call also switches the NAM
 * core to its fast tanh approximation, before any model exists.
 */
std::unique_ptr<::nam::DSP> GetModel(const std::filesystem::path& path);

/**
 * Whether GetModel builds fused models where the shape allows, or always the NAM core's. On by
 * default where this build has a fused kernel (fusedwavenet::IsAvailable()). Three layers decide,
 * the first that is set winning:
 * 1. SetFusedEngineEnabled, for tests and tools.
 * 2. The environment variable GUITARFX_NAM_ENGINE=core, set before launch, for A/B listening and
 *    support. It can only turn the engine off.
 * 3. SetFusedEngineSettingEnabled, the app's "Fast NAM Engine" setting (audio.nam.fusedEngine).
 * The engine is process-wide: every plugin instance in a DAW shares it. Models already built keep
 * the engine they were built with, so a change takes effect at the next model load
 * (PluginController::ApplyNamEngineSettingFromAppSettings rebuilds the running presets for it).
 */
void SetFusedEngineEnabled(bool enabled);
void SetFusedEngineSettingEnabled(bool enabled);
[[nodiscard]] bool IsFusedEngineEnabled();
/// Whether GUITARFX_NAM_ENGINE=core is set, which outranks the app setting.
[[nodiscard]] bool IsNamCoreForcedByEnvironment();

/// Maximum bytes of parsed model data to retain. Setting a lower budget evicts immediately.
void SetBudgetBytes(std::size_t bytes);
[[nodiscard]] std::size_t GetBudgetBytes();

/// Drop every entry. Intended for tests and for reclaiming memory on demand.
void Clear();

struct Stats
{
    std::size_t entries = 0;
    std::size_t bytes = 0;
    std::size_t hits = 0;
    std::size_t misses = 0;
    std::size_t evictions = 0;
    std::size_t fusedBuilds = 0; // models GetModel built on the fused pass
    std::size_t coreBuilds = 0;  // and on the NAM core
};

[[nodiscard]] Stats GetStats();
void ResetStats();
} // namespace nammodelcache
} // namespace guitarfx
