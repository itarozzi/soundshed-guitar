#include "dsp/NamModelCache.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <list>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

#include "NAM/activations.h"
#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include "dsp/nam/FusedWaveNet.h"
#include "util/LongPath.h"
#include "util/SessionLog.h"

namespace guitarfx::nammodelcache
{
namespace
{
// Parsed model data plus enough file identity to detect an edited or replaced file.
struct Entry
{
    std::shared_ptr<const ::nam::dspData> data;
    // The weights packed for the fused pass, once packing has been tried: null when the shape
    // is not one it takes.
    std::shared_ptr<const fusedwavenet::PackedModel> fused;
    bool fusedTried = false;
    std::uintmax_t fileSize = 0;
    std::int64_t writeTime = 0;
    std::size_t bytes = 0;
};

struct CacheState
{
    std::mutex mutex;
    std::unordered_map<std::string, Entry> entries;
    std::list<std::string> lru; // Front = most recently used.
    std::unordered_map<std::string, std::list<std::string>::iterator> lruPositions;
    std::size_t bytes = 0;
    std::size_t budgetBytes = 512u * 1024u * 1024u;
    std::size_t hits = 0;
    std::size_t misses = 0;
    std::size_t evictions = 0;
    std::size_t fusedBuilds = 0;
    std::size_t coreBuilds = 0;
};

CacheState& State()
{
    static CacheState state;
    return state;
}

std::size_t EstimateBytes(const ::nam::dspData& data)
{
    // Weights dominate; the JSON blobs are small by comparison but are counted so a
    // pathological metadata payload cannot slip past the budget entirely.
    return data.weights.size() * sizeof(float) + data.version.size() + data.architecture.size() +
           data.config.dump().size() + data.metadata.dump().size();
}

/// Canonical path where possible, so two spellings of one file share an entry.
std::string MakeKey(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(path, ec);
    const auto& resolved = ec ? path : canonical;
    // u8string avoids the code-page conversion that string() performs, which throws
    // on model names outside the active ANSI code page.
    const auto utf8 = resolved.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

bool StatFile(const std::filesystem::path& path, std::uintmax_t& outSize, std::int64_t& outWriteTime)
{
    std::error_code ec;
    outSize = std::filesystem::file_size(path, ec);

    if (ec)
    {
        return false;
    }

    const auto writeTime = std::filesystem::last_write_time(path, ec);

    if (ec)
    {
        return false;
    }

    outWriteTime = writeTime.time_since_epoch().count();
    return true;
}

// Caller must hold the lock.
void TouchLocked(CacheState& state, const std::string& key)
{
    auto positionIt = state.lruPositions.find(key);

    if (positionIt != state.lruPositions.end())
    {
        state.lru.erase(positionIt->second);
    }

    state.lru.push_front(key);
    state.lruPositions[key] = state.lru.begin();
}

// Caller must hold the lock.
void EraseLocked(CacheState& state, const std::string& key)
{
    auto entryIt = state.entries.find(key);

    if (entryIt == state.entries.end())
    {
        return;
    }

    state.bytes -= std::min(state.bytes, entryIt->second.bytes);
    state.entries.erase(entryIt);

    auto positionIt = state.lruPositions.find(key);

    if (positionIt != state.lruPositions.end())
    {
        state.lru.erase(positionIt->second);
        state.lruPositions.erase(positionIt);
    }
}

// Caller must hold the lock. Entries still referenced by a live model are only
// dropped from the cache; the shared_ptr keeps the data alive until that model
// finishes constructing, so eviction can never free data in use.
void EvictToBudgetLocked(CacheState& state)
{
    while (state.bytes > state.budgetBytes && !state.lru.empty())
    {
        const std::string victim = state.lru.back();
        EraseLocked(state, victim);
        ++state.evictions;
    }
}

// Switch the NAM core from std::tanh to its rational fast_tanh, once, before the first
// model is built. WaveNet and ConvNet take their activation from the core's registry
// when they are constructed, and that registry is a plain unordered_map, so it must not
// change while SignalGraphExecutor builds models on other threads; the function-local
// static makes concurrent first callers wait. LSTM reads the flag per sample instead.
// Measured on the standard WaveNet: about 9% faster, output within 52-55 dB of
// std::tanh (tools/nam-compression.mjs).
void EnableFastTanhOnce()
{
    static const bool enabled = [] {
        ::nam::activations::Activation::enable_fast_tanh();
        return true;
    }();
    (void)enabled;
}

// -1 until SetFusedEngineEnabled is called; the environment decides until then.
std::atomic<int> gFusedEngineOverride{-1};

bool FusedEngineFromEnvironment()
{
    std::string value;
#ifdef _WIN32
    char* raw = nullptr;
    std::size_t length = 0;

    if (_dupenv_s(&raw, &length, "GUITARFX_NAM_ENGINE") == 0 && raw != nullptr)
    {
        value = raw;
        std::free(raw);
    }

#else

    if (const char* raw = std::getenv("GUITARFX_NAM_ENGINE"); raw != nullptr)
    {
        value = raw;
    }

#endif
    return value != "core";
}

// One line per process, so a session log says which engine its NAM models ran on.
void LogEngineOnce()
{
    static const bool logged = [] {
        std::string line = "[NamModelCache] NAM engine: ";

        if (!fusedwavenet::IsAvailable())
        {
            line += "NAM core (no fused kernel in this build)";
        }
        else if (!IsFusedEngineEnabled())
        {
            line += "NAM core (fused WaveNet switched off)";
        }
        else
        {
            line += std::string("fused WaveNet (") + fusedwavenet::KernelName() + "), NAM core for other shapes";
        }

        util::AppendSessionLog(line);
        return true;
    }();
    (void)logged;
}

// The file as nam::get_dsp(path, dspData&) reads it, without building a model from it.
::nam::dspData ParseModelFile(const std::filesystem::path& path)
{
    std::ifstream stream(path);
    nlohmann::json json;
    stream >> json;

    ::nam::dspData data;
    data.version = json.at("version").get<std::string>();
    ::nam::verify_config_version(data.version);
    data.architecture = json.at("architecture").get<std::string>();
    data.config = json.at("config");
    data.metadata = json.value("metadata", nlohmann::json());

    const auto weights = json.find("weights");

    if (weights == json.end())
    {
        throw std::runtime_error("Corrupted model file is missing weights.");
    }

    data.weights = weights->get<std::vector<float>>();
    data.expected_sample_rate = ::nam::get_sample_rate_from_nam_file(json);
    return data;
}

// Packs the entry's weights for the fused pass, once per entry, outside the lock. Another
// thread may pack the same entry meanwhile; the first to store wins, and both results match.
std::shared_ptr<const fusedwavenet::PackedModel> PackForFusedEngine(CacheState& state, const std::string& key,
                                                                    const std::shared_ptr<const ::nam::dspData>& data)
{
    auto packed = fusedwavenet::Pack(*data);
    std::lock_guard<std::mutex> lock(state.mutex);
    auto it = state.entries.find(key);

    // Only into the entry this parse came from: the file may have been replaced since.
    if (it != state.entries.end() && it->second.data == data && !it->second.fusedTried)
    {
        it->second.fused = packed;
        it->second.fusedTried = true;

        if (packed)
        {
            it->second.bytes += packed->bytes;
            state.bytes += packed->bytes;
            EvictToBudgetLocked(state);
        }
    }

    return packed;
}
} // namespace

void SetFusedEngineEnabled(const bool enabled)
{
    gFusedEngineOverride.store(enabled ? 1 : 0, std::memory_order_relaxed);
}

bool IsFusedEngineEnabled()
{
    const int forced = gFusedEngineOverride.load(std::memory_order_relaxed);

    if (forced >= 0)
    {
        return forced != 0;
    }

    static const bool fromEnvironment = FusedEngineFromEnvironment();
    return fromEnvironment;
}

std::unique_ptr<::nam::DSP> GetModel(const std::filesystem::path& path)
{
    EnableFastTanhOnce();
    LogEngineOnce();

    // A model past MAX_PATH reads as missing on Windows without the extended-length prefix.
    const auto osPath = util::LongPathSafe(path);
    std::uintmax_t fileSize = 0;
    std::int64_t writeTime = 0;

    if (!StatFile(osPath, fileSize, writeTime))
    {
        return nullptr;
    }

    const std::string key = MakeKey(osPath);
    auto& state = State();

    std::shared_ptr<const ::nam::dspData> cached;
    std::shared_ptr<const fusedwavenet::PackedModel> fused;
    bool fusedTried = false;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        auto it = state.entries.find(key);

        if (it != state.entries.end())
        {
            if (it->second.fileSize == fileSize && it->second.writeTime == writeTime)
            {
                cached = it->second.data;
                fused = it->second.fused;
                fusedTried = it->second.fusedTried;
                TouchLocked(state, key);
                ++state.hits;
            }
            else
            {
                // File changed on disk since it was cached.
                EraseLocked(state, key);
            }
        }

        if (!cached)
        {
            ++state.misses;
        }
    }

    try
    {
        if (!cached)
        {
            auto parsed = std::make_shared<const ::nam::dspData>(ParseModelFile(osPath));
            const std::size_t bytes = EstimateBytes(*parsed);
            std::lock_guard<std::mutex> lock(state.mutex);
            // A concurrent load of the same path may have inserted already; replacing is
            // harmless and keeps the newest stat data.
            EraseLocked(state, key);

            Entry entry;
            entry.data = parsed;
            entry.fileSize = fileSize;
            entry.writeTime = writeTime;
            entry.bytes = bytes;

            state.bytes += bytes;
            state.entries.emplace(key, std::move(entry));
            TouchLocked(state, key);
            EvictToBudgetLocked(state);
            cached = std::move(parsed);
        }

        if (IsFusedEngineEnabled() && fusedwavenet::IsAvailable())
        {
            if (!fusedTried)
            {
                fused = PackForFusedEngine(state, key, cached);
            }

            if (fused)
            {
                auto model = fusedwavenet::Build(*fused);
                std::lock_guard<std::mutex> lock(state.mutex);
                ++state.fusedBuilds;
                return model;
            }
        }

        // get_dsp takes a non-const reference and may consume the data, so the cached
        // copy is never handed out directly.
        ::nam::dspData copy = *cached;
        auto model = ::nam::get_dsp(copy);

        if (model)
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            ++state.coreBuilds;
        }

        return model;
    }
    catch (const std::exception& e)
    {
        util::AppendSessionLog(std::string{"[NamModelCache] ERROR: failed to load model: "} + e.what());
        return nullptr;
    }
    catch (...)
    {
        util::AppendSessionLog("[NamModelCache] ERROR: unknown failure loading model");
        return nullptr;
    }
}

void SetBudgetBytes(std::size_t bytes)
{
    auto& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.budgetBytes = bytes;
    EvictToBudgetLocked(state);
}

std::size_t GetBudgetBytes()
{
    auto& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.budgetBytes;
}

void Clear()
{
    auto& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.entries.clear();
    state.lru.clear();
    state.lruPositions.clear();
    state.bytes = 0;
}

Stats GetStats()
{
    auto& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    Stats stats;
    stats.entries = state.entries.size();
    stats.bytes = state.bytes;
    stats.hits = state.hits;
    stats.misses = state.misses;
    stats.evictions = state.evictions;
    stats.fusedBuilds = state.fusedBuilds;
    stats.coreBuilds = state.coreBuilds;
    return stats;
}

void ResetStats()
{
    auto& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.hits = 0;
    state.misses = 0;
    state.evictions = 0;
    state.fusedBuilds = 0;
    state.coreBuilds = 0;
}
} // namespace guitarfx::nammodelcache
