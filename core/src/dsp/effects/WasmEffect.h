#pragma once

#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <wasmtime.h>

namespace guitarfx
{
struct WasmMetadataEntry
{
    std::string key;
    std::string value;
};

// A guest param's place in WasmModuleDescriptor::parameters is its declaration order, which is
// only its order in the UI: the module reads it through the handle param_handle gives for its id.
struct WasmGuestParameterDescriptor
{
    ParameterDef definition;
};

struct WasmModuleDescriptor
{
    std::vector<WasmMetadataEntry> entries;
    std::string displayName;
    std::string version;
    std::string description;
    std::string category = "utility";
    std::string thumbnailDataUrl;
    std::vector<WasmGuestParameterDescriptor> parameters;
    std::vector<ExposedResource> exposedResources;
};

class WasmSharedRuntime;

// Hosts a Custom Effect: a WASM module, which is code from whoever wrote or shared it. It runs in
// Wasmtime with a few read-only host imports and no WASI, so it cannot reach files or the network.
// The interface is Custom Effect ABI 2 (docs/wasm-module-authoring.md): the module looks its params
// up by id with param_handle and reads them with param_value; ABI 1 modules are refused.
// What is left is bounded here: every call into the guest has an epoch deadline (a guest that
// loops traps and the node bypasses), the store caps memory and tables, the wasm stack is capped
// below the smallest audio-thread stack, and modules load only while Custom Effects are enabled.
class WasmEffect final : public EffectProcessor
{
  public:
    static constexpr std::int32_t kAbiVersion = 2;
    static constexpr std::size_t kMaxGuestParams = 64;
    static constexpr std::size_t kMaxGuestIdLength = 32;
    // SetParam's own keys. A guest param may not take one of these ids.
    static constexpr std::array<std::string_view, 4> kHostParamIds{"mix", "inputGainDb", "outputGainDb", "bpm"};
    static constexpr const char* kDescriptorConfigKey = "wasmGuestDescriptor";

    static constexpr std::size_t kMaxModuleBytes = 32u * 1024u * 1024u;
    static constexpr std::int64_t kMaxGuestMemoryBytes = 256ll * 1024 * 1024;
    static constexpr std::int64_t kMaxGuestTableElements = 100000;
    static constexpr std::size_t kMaxGuestStackBytes = 256u * 1024u;

    // Process-wide, from the Custom Effects feature setting. While it is off no module is compiled
    // or run, whatever a preset, archive or project asks for.
    static void SetHostingEnabled(bool enabled);
    [[nodiscard]] static bool IsHostingEnabled();

    // A module path on a network share is refused before anything touches it: opening one would
    // fetch code from another machine, and on Windows send the user's credentials to it.
    [[nodiscard]] static bool IsNetworkPath(const std::filesystem::path& path);

    WasmEffect();
    ~WasmEffect() override;

    [[nodiscard]] static std::optional<WasmModuleDescriptor> InspectModuleFile(const std::filesystem::path& modulePath,
                                                                               std::string* error = nullptr);
    [[nodiscard]] static std::optional<WasmModuleDescriptor> ParseDescriptorConfig(const std::string& configJson,
                                                                                   std::string* error = nullptr);
    [[nodiscard]] static std::string SerializeDescriptorConfig(const std::vector<WasmMetadataEntry>& entries);

    void Prepare(double sampleRate, int maxBlockSize) override;
    void Reset() override;
    void Process(float** inputs, float** outputs, int numSamples) override;

    void SetParam(const std::string& key, double value) override;
    double GetParam(const std::string& key) const override;

    void SetConfig(const std::string& key, const std::string& value) override;
    std::string GetConfig(const std::string& key) const override;

    bool LoadResources(const std::vector<ResourceRef>& refs, const std::vector<std::filesystem::path>& paths) override;

    bool RequiresResource() const override
    {
        return true;
    }

    bool HasResource() const override
    {
        return !mModuleBytes.empty();
    }

    std::filesystem::path GetResourcePath() const override
    {
        return mModulePath;
    }

    int GetLatencySamples() const override
    {
        return mLatencySamples;
    }

    std::string GetType() const override
    {
        return "wasm_host";
    }

    std::string GetCategory() const override
    {
        return "utility";
    }

    [[nodiscard]] static bool IsHostParamId(std::string_view id);

    // The host imports. A handle is opaque: numbered from a base picked afresh at every load, so a
    // module that hard-codes one instead of asking param_handle fails on its first run.
    std::int32_t GuestParamHandle(std::string_view id) const;
    bool ReadGuestParamHandle(std::int32_t handle, float& value) const;
    float GuestTempoBpm() const;
    int ReadGuestResourceSize(int slot) const;
    int ReadGuestResourceByte(int slot, int offset) const;

  private:
    bool LoadGuestDescriptor();
    bool RebuildRuntime();
    bool BuildRuntimeOnly();
    bool CheckModuleImports();
    bool CheckAbiVersion();
    bool LoadFunctionExport(const char* exportName, wasmtime_func_t& outFunction, bool required, bool& found);
    std::optional<std::int32_t> CallGuestI32(wasmtime_func_t& function, const char* functionName);
    void ArmGuestDeadline(double budgetSeconds);
    bool InvokePrepare();
    bool InvokeProcess(float inputLeft, float inputRight, float& outputLeft, float& outputRight);
    int QueryLatencySamples();
    void ResetGuestDescriptor();
    void ApplyGuestDescriptor(const WasmModuleDescriptor& descriptor);
    void TeardownRuntime();
    void SetError(const std::string& message);

    // Indexed by declaration order. Values for ids no descriptor has declared yet wait in
    // mPendingGuestParamValues; it is only written before a descriptor loads, or when one is
    // replaced, never once SetParam can come from the audio thread.
    std::array<double, kMaxGuestParams> mGuestValues{};
    std::size_t mGuestParamCount = 0;
    bool mHasGuestDescriptor = false;
    std::int32_t mGuestHandleBase = 0;
    double mMix = 1.0;
    double mInputGainDb = 0.0;
    double mOutputGainDb = 0.0;
    double mBpm = 120.0;
    std::unordered_map<std::string, std::size_t> mGuestParamIndex;
    std::unordered_map<std::string, double> mPendingGuestParamValues;
    WasmModuleDescriptor mGuestDescriptor;

    std::vector<uint8_t> mModuleBytes;
    std::vector<std::vector<uint8_t>> mResourceBytesBySlot;
    std::filesystem::path mModulePath;

    // Shared by every Custom Effect; mEngine is its engine, kept here for the many call sites.
    std::shared_ptr<WasmSharedRuntime> mRuntime;
    wasm_engine_t* mEngine = nullptr;
    wasmtime_store_t* mStore = nullptr;
    wasmtime_context_t* mContext = nullptr;
    wasmtime_linker_t* mLinker = nullptr;
    wasmtime_module_t* mModule = nullptr;
    wasmtime_instance_t mInstance{};
    wasmtime_func_t mPrepareFunction{};
    wasmtime_func_t mResetFunction{};
    wasmtime_func_t mProcessFunction{};
    wasmtime_func_t mLatencyFunction{};
    bool mHasInstance = false;
    bool mHasPrepareFunction = false;
    bool mHasResetFunction = false;
    bool mHasProcessFunction = false;
    bool mHasLatencyFunction = false;

    int mLatencySamples = 0;
    bool mRuntimeReady = false;
    std::string mLastError;
};

void RegisterWasmEffect();
} // namespace guitarfx