#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#include "dsp/SignalGraphExecutor.h"
#include "dsp/effects/BuiltinEffects.h"
#include "dsp/effects/WasmEffect.h"
#include "resources/ResourceLibrary.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace guitarfx
{
namespace
{
using ByteVector = std::vector<uint8_t>;

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 8;
constexpr float kTolerance = 1.0e-4f;

enum class WasmValueType : uint8_t
{
    I32 = 0x7f,
    F32 = 0x7d,
};

struct FuncTypeDef
{
    std::vector<WasmValueType> params;
    std::vector<WasmValueType> results;
};

struct ImportFuncDef
{
    std::string moduleName;
    std::string fieldName;
    uint32_t typeIndex = 0;
};

struct GlobalDef
{
    WasmValueType valueType = WasmValueType::F32;
    bool isMutable = false;
    ByteVector initExpr;
};

struct DefinedFuncDef
{
    uint32_t typeIndex = 0;
    ByteVector ops;
};

struct DataSegmentDef
{
    uint32_t offset = 0;
    ByteVector bytes;
};

struct TempDir
{
    TempDir()
    {
        root = std::filesystem::temp_directory_path() / "soundshed_guitar_wasm_host_tests";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
    }

    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    std::filesystem::path root;
};

void AppendU8(ByteVector& out, uint8_t value)
{
    out.push_back(value);
}

void AppendBytes(ByteVector& out, const ByteVector& bytes)
{
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void AppendU32Leb(ByteVector& out, uint32_t value)
{
    do
    {
        uint8_t byte = static_cast<uint8_t>(value & 0x7fu);
        value >>= 7u;

        if (value != 0)
        {
            byte |= 0x80u;
        }

        out.push_back(byte);
    } while (value != 0);
}

void AppendI32Leb(ByteVector& out, int32_t value)
{
    bool more = true;

    while (more)
    {
        uint8_t byte = static_cast<uint8_t>(value & 0x7f);
        value >>= 7;
        const bool signBitSet = (byte & 0x40u) != 0;
        more = !((value == 0 && !signBitSet) || (value == -1 && signBitSet));

        if (more)
        {
            byte |= 0x80u;
        }

        out.push_back(byte);
    }
}

void AppendF32(ByteVector& out, float value)
{
    std::array<uint8_t, sizeof(float)> bytes{};
    std::memcpy(bytes.data(), &value, sizeof(float));
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void AppendString(ByteVector& out, const std::string& value)
{
    AppendU32Leb(out, static_cast<uint32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

ByteVector MakeSection(uint8_t sectionId, const ByteVector& payload)
{
    ByteVector section;
    AppendU8(section, sectionId);
    AppendU32Leb(section, static_cast<uint32_t>(payload.size()));
    AppendBytes(section, payload);
    return section;
}

ByteVector LocalGet(uint32_t index)
{
    ByteVector ops{0x20};
    AppendU32Leb(ops, index);
    return ops;
}

ByteVector GlobalGet(uint32_t index)
{
    ByteVector ops{0x23};
    AppendU32Leb(ops, index);
    return ops;
}

ByteVector GlobalSet(uint32_t index)
{
    ByteVector ops{0x24};
    AppendU32Leb(ops, index);
    return ops;
}

ByteVector Call(uint32_t index)
{
    ByteVector ops{0x10};
    AppendU32Leb(ops, index);
    return ops;
}

ByteVector I32Const(int32_t value)
{
    ByteVector ops{0x41};
    AppendI32Leb(ops, value);
    return ops;
}

ByteVector F32Const(float value)
{
    ByteVector ops{0x43};
    AppendF32(ops, value);
    return ops;
}

void AppendOp(ByteVector& target, const ByteVector& op)
{
    target.insert(target.end(), op.begin(), op.end());
}

ByteVector MakeModule(const std::vector<FuncTypeDef>& types, const std::vector<ImportFuncDef>& imports,
                      const std::vector<GlobalDef>& globals, const std::vector<DefinedFuncDef>& definedFunctions,
                      const std::vector<std::pair<std::string, uint32_t>>& exports, bool exportMemory = false,
                      const std::vector<DataSegmentDef>& dataSegments = {}, uint32_t memoryMinPages = 1,
                      std::optional<uint32_t> startFunction = std::nullopt)
{
    ByteVector module{0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};

    ByteVector typePayload;
    AppendU32Leb(typePayload, static_cast<uint32_t>(types.size()));

    for (const auto& type : types)
    {
        AppendU8(typePayload, 0x60);
        AppendU32Leb(typePayload, static_cast<uint32_t>(type.params.size()));

        for (const auto valueType : type.params)
        {
            AppendU8(typePayload, static_cast<uint8_t>(valueType));
        }

        AppendU32Leb(typePayload, static_cast<uint32_t>(type.results.size()));

        for (const auto valueType : type.results)
        {
            AppendU8(typePayload, static_cast<uint8_t>(valueType));
        }
    }

    AppendBytes(module, MakeSection(1, typePayload));

    if (!imports.empty())
    {
        ByteVector importPayload;
        AppendU32Leb(importPayload, static_cast<uint32_t>(imports.size()));

        for (const auto& import : imports)
        {
            AppendString(importPayload, import.moduleName);
            AppendString(importPayload, import.fieldName);
            AppendU8(importPayload, 0x00);
            AppendU32Leb(importPayload, import.typeIndex);
        }

        AppendBytes(module, MakeSection(2, importPayload));
    }

    if (!definedFunctions.empty())
    {
        ByteVector functionPayload;
        AppendU32Leb(functionPayload, static_cast<uint32_t>(definedFunctions.size()));

        for (const auto& function : definedFunctions)
        {
            AppendU32Leb(functionPayload, function.typeIndex);
        }

        AppendBytes(module, MakeSection(3, functionPayload));
    }

    if (exportMemory || !dataSegments.empty())
    {
        ByteVector memoryPayload;
        AppendU32Leb(memoryPayload, 1);
        AppendU8(memoryPayload, 0x00);
        AppendU32Leb(memoryPayload, memoryMinPages);
        AppendBytes(module, MakeSection(5, memoryPayload));
    }

    if (!globals.empty())
    {
        ByteVector globalPayload;
        AppendU32Leb(globalPayload, static_cast<uint32_t>(globals.size()));

        for (const auto& global : globals)
        {
            AppendU8(globalPayload, static_cast<uint8_t>(global.valueType));
            AppendU8(globalPayload, global.isMutable ? 0x01 : 0x00);
            AppendBytes(globalPayload, global.initExpr);
            AppendU8(globalPayload, 0x0b);
        }

        AppendBytes(module, MakeSection(6, globalPayload));
    }

    ByteVector exportPayload;
    AppendU32Leb(exportPayload, static_cast<uint32_t>(exports.size() + (exportMemory ? 1 : 0)));

    for (const auto& [name, functionIndex] : exports)
    {
        AppendString(exportPayload, name);
        AppendU8(exportPayload, 0x00);
        AppendU32Leb(exportPayload, functionIndex);
    }

    if (exportMemory)
    {
        AppendString(exportPayload, "memory");
        AppendU8(exportPayload, 0x02);
        AppendU32Leb(exportPayload, 0);
    }

    AppendBytes(module, MakeSection(7, exportPayload));

    if (startFunction)
    {
        ByteVector startPayload;
        AppendU32Leb(startPayload, *startFunction);
        AppendBytes(module, MakeSection(8, startPayload));
    }

    ByteVector codePayload;
    AppendU32Leb(codePayload, static_cast<uint32_t>(definedFunctions.size()));

    for (const auto& function : definedFunctions)
    {
        ByteVector body;
        AppendU32Leb(body, 0);
        AppendBytes(body, function.ops);
        AppendU8(body, 0x0b);
        AppendU32Leb(codePayload, static_cast<uint32_t>(body.size()));
        AppendBytes(codePayload, body);
    }

    AppendBytes(module, MakeSection(10, codePayload));

    if (!dataSegments.empty())
    {
        ByteVector dataPayload;
        AppendU32Leb(dataPayload, static_cast<uint32_t>(dataSegments.size()));

        for (const auto& segment : dataSegments)
        {
            AppendU8(dataPayload, 0x00);
            AppendOp(dataPayload, I32Const(static_cast<int32_t>(segment.offset)));
            AppendU8(dataPayload, 0x0b);
            AppendU32Leb(dataPayload, static_cast<uint32_t>(segment.bytes.size()));
            AppendBytes(dataPayload, segment.bytes);
        }

        AppendBytes(module, MakeSection(11, dataPayload));
    }

    return module;
}

ByteVector BuildDescriptorBlob(const std::vector<std::pair<std::string, std::string>>& entries)
{
    ByteVector blob;

    for (const auto& [key, value] : entries)
    {
        blob.insert(blob.end(), key.begin(), key.end());
        blob.push_back('=');
        blob.insert(blob.end(), value.begin(), value.end());
        blob.push_back('\n');
    }

    return blob;
}

// Every fixture is a Custom Effect ABI 2 module laid out the same way: its imports, then prepare,
// reset, process, latency, the descriptor's pointer and length, audiofx_abi_version and an
// optional start function. The descriptor sits at offset 0 of memory, the id strings at kIdOffset.
constexpr uint32_t kIdOffset = 4096;

struct GuestSpec
{
    std::vector<FuncTypeDef> importTypes;
    std::vector<ImportFuncDef> imports;
    std::vector<GlobalDef> globals;
    ByteVector descriptor;
    std::vector<std::string> ids;
    ByteVector prepareOps = I32Const(0);
    ByteVector resetOps;
    ByteVector processOps;
    ByteVector startOps;
    std::optional<int32_t> abiVersion = WasmEffect::kAbiVersion;
    uint32_t memoryMinPages = 1;
};

ByteVector MakeGuestModule(const GuestSpec& spec)
{
    std::vector<FuncTypeDef> types = spec.importTypes;
    const auto firstType = static_cast<uint32_t>(types.size());
    types.push_back({{WasmValueType::F32, WasmValueType::I32}, {WasmValueType::I32}});
    types.push_back({{}, {}});
    types.push_back({{WasmValueType::F32, WasmValueType::F32}, {WasmValueType::F32, WasmValueType::F32}});
    types.push_back({{}, {WasmValueType::I32}});

    ByteVector processOps = spec.processOps;

    if (processOps.empty())
    {
        AppendOp(processOps, LocalGet(0));
        AppendOp(processOps, LocalGet(1));
    }

    std::vector<DefinedFuncDef> functions = {
        {firstType, spec.prepareOps},
        {firstType + 1, spec.resetOps},
        {firstType + 2, processOps},
        {firstType + 3, I32Const(0)},
        {firstType + 3, I32Const(0)},
        {firstType + 3, I32Const(static_cast<int32_t>(spec.descriptor.size()))},
        {firstType + 3, I32Const(spec.abiVersion.value_or(0))},
    };

    const auto first = static_cast<uint32_t>(spec.imports.size());
    std::vector<std::pair<std::string, uint32_t>> exports = {
        {"audiofx_prepare", first},
        {"audiofx_reset", first + 1},
        {"audiofx_process", first + 2},
        {"audiofx_get_latency_samples", first + 3},
        {"audiofx_descriptor_ptr", first + 4},
        {"audiofx_descriptor_len", first + 5},
    };

    if (spec.abiVersion)
    {
        exports.push_back({"audiofx_abi_version", first + 6});
    }

    std::optional<uint32_t> startFunction;

    if (!spec.startOps.empty())
    {
        functions.push_back({firstType + 1, spec.startOps});
        startFunction = first + 7;
    }

    std::vector<DataSegmentDef> data;

    if (!spec.descriptor.empty())
    {
        data.push_back({0, spec.descriptor});
    }

    ByteVector idBytes;

    for (const auto& id : spec.ids)
    {
        idBytes.insert(idBytes.end(), id.begin(), id.end());
    }

    if (!idBytes.empty())
    {
        data.push_back({kIdOffset, idBytes});
    }

    return MakeModule(types, spec.imports, spec.globals, functions, exports, true, data, spec.memoryMinPages,
                      startFunction);
}

constexpr uint32_t kParamHandleImport = 0;
constexpr uint32_t kParamValueImport = 1;
constexpr uint32_t kTempoImport = 2;

// A module that imports the param readers and keeps one handle per id in globals 0..N-1, looked up
// in prepare as the authoring guide says.
GuestSpec ParamGuest(ByteVector descriptor, std::vector<std::string> ids)
{
    GuestSpec spec;
    spec.importTypes = {
        {{WasmValueType::I32, WasmValueType::I32}, {WasmValueType::I32}},
        {{WasmValueType::I32}, {WasmValueType::F32}},
        {{}, {WasmValueType::F32}},
    };
    spec.imports = {
        {"host", "param_handle", 0},
        {"host", "param_value", 1},
        {"host", "tempo_bpm", 2},
    };
    spec.descriptor = std::move(descriptor);
    spec.ids = std::move(ids);
    spec.prepareOps.clear();

    uint32_t offset = kIdOffset;

    for (uint32_t index = 0; index < spec.ids.size(); ++index)
    {
        const auto length = static_cast<int32_t>(spec.ids[index].size());
        spec.globals.push_back({WasmValueType::I32, true, I32Const(-1)});
        AppendOp(spec.prepareOps, I32Const(static_cast<int32_t>(offset)));
        AppendOp(spec.prepareOps, I32Const(length));
        AppendOp(spec.prepareOps, Call(kParamHandleImport));
        AppendOp(spec.prepareOps, GlobalSet(index));
        offset += static_cast<uint32_t>(length);
    }

    AppendOp(spec.prepareOps, I32Const(0));
    return spec;
}

// param_value(the handle for ids[index]).
ByteVector ReadParam(uint32_t index)
{
    ByteVector ops = GlobalGet(index);
    AppendOp(ops, Call(kParamValueImport));
    return ops;
}

// Scales each channel by the 'gain' param.
ByteVector MakeGainModule()
{
    auto spec = ParamGuest(BuildDescriptorBlob({
                               {"effect.name", "WASM Gain"},
                               {"effect.version", "1.2.3"},
                               {"effect.category", "utility"},
                               {"effect.description", "Simple gain module with self-described guest metadata."},
                               {"effect.thumbnailBase64", "dGVzdA=="},
                               {"param.gain.title", "Gain"},
                               {"param.gain.default", "0.5"},
                               {"param.gain.min", "0.0"},
                               {"param.gain.max", "2.0"},
                               {"param.gain.unit", "amount"},
                           }),
                           {"gain"});

    AppendOp(spec.processOps, LocalGet(0));
    AppendOp(spec.processOps, ReadParam(0));
    AppendU8(spec.processOps, 0x94);
    AppendOp(spec.processOps, LocalGet(1));
    AppendOp(spec.processOps, ReadParam(0));
    AppendU8(spec.processOps, 0x94);
    return MakeGuestModule(spec);
}

// Both outputs are (left + right) / 2 scaled by 'gain'.
ByteVector MakeStereoAverageModule()
{
    auto spec = ParamGuest(BuildDescriptorBlob({
                               {"effect.name", "Stereo Average"},
                               {"effect.category", "utility"},
                               {"param.gain.title", "Output Gain"},
                               {"param.gain.default", "1.0"},
                               {"param.gain.min", "0.0"},
                               {"param.gain.max", "2.0"},
                           }),
                           {"gain"});

    for (int channel = 0; channel < 2; ++channel)
    {
        AppendOp(spec.processOps, LocalGet(0));
        AppendOp(spec.processOps, LocalGet(1));
        AppendU8(spec.processOps, 0x92);
        AppendOp(spec.processOps, F32Const(0.5f));
        AppendU8(spec.processOps, 0x94);
        AppendOp(spec.processOps, ReadParam(0));
        AppendU8(spec.processOps, 0x94);
    }

    return MakeGuestModule(spec);
}

// Outputs (param 'a', param 'b'), whatever order the descriptor declares them in.
ByteVector MakePairModule(bool reversed)
{
    std::vector<std::pair<std::string, std::string>> entries = {
        {"param.a.default", "0.1"},
        {"param.b.default", "0.9"},
    };

    if (reversed)
    {
        std::swap(entries[0], entries[1]);
    }

    auto spec = ParamGuest(BuildDescriptorBlob(entries), {"a", "b"});
    AppendOp(spec.processOps, ReadParam(0));
    AppendOp(spec.processOps, ReadParam(1));
    return MakeGuestModule(spec);
}

// Outputs (param 'last', tempo_bpm()).
ByteVector MakeTempoModule()
{
    auto spec = ParamGuest(BuildDescriptorBlob({
                               {"effect.name", "Tempo"},
                               {"param.first.default", "0.1"},
                               {"param.last.default", "0.75"},
                           }),
                           {"first", "last"});

    AppendOp(spec.processOps, ReadParam(1));
    AppendOp(spec.processOps, Call(kTempoImport));
    return MakeGuestModule(spec);
}

// Reads params at 0 and 1, the way ABI 1 numbered them, without asking param_handle.
ByteVector MakeHardCodedHandleModule()
{
    auto spec = ParamGuest(BuildDescriptorBlob({{"param.gain.default", "1.0"}}), {});

    AppendOp(spec.processOps, I32Const(0));
    AppendOp(spec.processOps, Call(kParamValueImport));
    AppendOp(spec.processOps, I32Const(1));
    AppendOp(spec.processOps, Call(kParamValueImport));
    return MakeGuestModule(spec);
}

// Prepare succeeds only if param_handle answers -1 to an undeclared id, an id outside memory, an
// empty id and an over-long one. Process then outputs 0.125 on both channels.
ByteVector MakeUnknownIdModule()
{
    auto spec = ParamGuest(BuildDescriptorBlob({{"param.gain.default", "1.0"}}), {"missing"});
    spec.globals.clear();
    spec.prepareOps.clear();

    const std::vector<std::pair<int32_t, int32_t>> lookups = {
        {static_cast<int32_t>(kIdOffset), 7},
        {0x7fff0000, 4},
        {static_cast<int32_t>(kIdOffset), 0},
        {static_cast<int32_t>(kIdOffset), static_cast<int32_t>(WasmEffect::kMaxGuestIdLength) + 1},
    };

    for (std::size_t i = 0; i < lookups.size(); ++i)
    {
        AppendOp(spec.prepareOps, I32Const(lookups[i].first));
        AppendOp(spec.prepareOps, I32Const(lookups[i].second));
        AppendOp(spec.prepareOps, Call(kParamHandleImport));
        AppendOp(spec.prepareOps, I32Const(1));
        AppendU8(spec.prepareOps, 0x6a);

        if (i > 0)
        {
            AppendU8(spec.prepareOps, 0x72);
        }
    }

    AppendOp(spec.processOps, F32Const(0.125f));
    AppendOp(spec.processOps, F32Const(0.125f));
    return MakeGuestModule(spec);
}

constexpr uint32_t kResourceHandleImport = 0;
constexpr uint32_t kResourceSizeImport = 1;
constexpr uint32_t kResourceReadImport = 2;
// Where the resource fixtures copy what they read; one page of memory ends at kMemoryEnd.
constexpr uint32_t kResourceDataOffset = 8192;
constexpr int32_t kMemoryEnd = 65536;

// A module that imports the resource readers and keeps one handle per id in globals 0..N-1, looked
// up in prepare. Its prepare ends with the handle lookups: the caller appends the status.
GuestSpec ResourceGuest(ByteVector descriptor, std::vector<std::string> ids)
{
    GuestSpec spec;
    spec.importTypes = {
        {{WasmValueType::I32, WasmValueType::I32}, {WasmValueType::I32}},
        {{WasmValueType::I32}, {WasmValueType::I32}},
        {{WasmValueType::I32, WasmValueType::I32, WasmValueType::I32, WasmValueType::I32}, {WasmValueType::I32}},
    };
    spec.imports = {
        {"host", "resource_handle", 0},
        {"host", "resource_size", 1},
        {"host", "resource_read", 2},
    };
    spec.descriptor = std::move(descriptor);
    spec.ids = std::move(ids);
    spec.prepareOps.clear();

    uint32_t offset = kIdOffset;

    for (uint32_t index = 0; index < spec.ids.size(); ++index)
    {
        const auto length = static_cast<int32_t>(spec.ids[index].size());
        spec.globals.push_back({WasmValueType::I32, true, I32Const(-1)});
        AppendOp(spec.prepareOps, I32Const(static_cast<int32_t>(offset)));
        AppendOp(spec.prepareOps, I32Const(length));
        AppendOp(spec.prepareOps, Call(kResourceHandleImport));
        AppendOp(spec.prepareOps, GlobalSet(index));
        offset += static_cast<uint32_t>(length);
    }

    return spec;
}

// resource_read(the handle in global `handleGlobal`, offset, destination, length).
ByteVector ReadResource(uint32_t handleGlobal, int32_t offset, int32_t destination, int32_t length)
{
    ByteVector ops = GlobalGet(handleGlobal);
    AppendOp(ops, I32Const(offset));
    AppendOp(ops, I32Const(destination));
    AppendOp(ops, I32Const(length));
    AppendOp(ops, Call(kResourceReadImport));
    return ops;
}

ByteVector ResourceSize(uint32_t handleGlobal)
{
    ByteVector ops = GlobalGet(handleGlobal);
    AppendOp(ops, Call(kResourceSizeImport));
    return ops;
}

// i32.load8_u at a fixed address.
ByteVector LoadByte(int32_t address)
{
    ByteVector ops = I32Const(address);
    AppendOp(ops, {0x2d, 0x00, 0x00});
    return ops;
}

// Copies the first byte of resources 'a' and 'b' in prepare and outputs (a / 64, b / 64), whatever
// order the descriptor declares them in.
ByteVector MakeResourcePairModule(bool reversed)
{
    std::vector<std::pair<std::string, std::string>> entries = {
        {"resource.a.title", "A"},
        {"resource.b.title", "B"},
    };

    if (reversed)
    {
        std::swap(entries[0], entries[1]);
    }

    auto spec = ResourceGuest(BuildDescriptorBlob(entries), {"a", "b"});

    for (uint32_t index = 0; index < 2; ++index)
    {
        AppendOp(spec.prepareOps, ReadResource(index, 0, static_cast<int32_t>(kResourceDataOffset + 16 * index), 16));
        AppendU8(spec.prepareOps, 0x1a);
    }

    AppendOp(spec.prepareOps, I32Const(0));

    for (uint32_t index = 0; index < 2; ++index)
    {
        AppendOp(spec.processOps, LoadByte(static_cast<int32_t>(kResourceDataOffset + 16 * index)));
        AppendU8(spec.processOps, 0xb3);
        AppendOp(spec.processOps, F32Const(64.0f));
        AppendU8(spec.processOps, 0x95);
    }

    return MakeGuestModule(spec);
}

// Prepare checks the resource readers' edges, setting bit i of its status when check i fails, so
// a failure names itself in lastError. 'data' is given the bytes 1, 2, 3, 4; 'empty' no file.
// Process outputs 0.125 on both channels.
ByteVector MakeResourceEdgeModule()
{
    auto spec = ResourceGuest(BuildDescriptorBlob({
                                  {"resource.data.title", "Data"},
                                  {"resource.empty.title", "Empty"},
                              }),
                              {"data", "empty"});
    constexpr auto kDst = static_cast<int32_t>(kResourceDataOffset);

    const auto handleOf = [](int32_t pointer, int32_t length) {
        ByteVector ops = I32Const(pointer);
        AppendOp(ops, I32Const(length));
        AppendOp(ops, Call(kResourceHandleImport));
        return ops;
    };

    const std::vector<std::pair<ByteVector, int32_t>> checks = {
        {ResourceSize(0), 4},
        {ReadResource(0, 2, kDst, 100), 2},         // stops at the end of the resource
        {ReadResource(0, 4, kDst + 32, 1), 0},      // offset at the end
        {ReadResource(0, -1, kDst + 32, 1), 0},     // negative offset
        {ReadResource(0, 0, kMemoryEnd - 1, 4), 1}, // stops at the end of guest memory
        {ReadResource(0, 0, kMemoryEnd, 1), 0},     // destination past memory
        {ReadResource(0, 0, kDst + 32, -4), 0},     // negative length
        {ResourceSize(1), 0},                       // declared, no file chosen
        {ReadResource(1, 0, kDst + 32, 4), 0},
        {handleOf(static_cast<int32_t>(kIdOffset), 3), -1}, // "dat": undeclared
        {handleOf(0x7fff0000, 4), -1},                      // id outside memory
        {handleOf(static_cast<int32_t>(kIdOffset), static_cast<int32_t>(WasmEffect::kMaxGuestIdLength) + 1), -1},
        {LoadByte(kDst), 3}, // what the second check copied
        {LoadByte(kDst + 1), 4},
        {LoadByte(kMemoryEnd - 1), 1},
    };

    AppendOp(spec.prepareOps, I32Const(0));

    for (std::size_t i = 0; i < checks.size(); ++i)
    {
        AppendOp(spec.prepareOps, checks[i].first);
        AppendOp(spec.prepareOps, I32Const(checks[i].second));
        AppendU8(spec.prepareOps, 0x47);
        AppendOp(spec.prepareOps, I32Const(1 << i));
        AppendU8(spec.prepareOps, 0x6c);
        AppendU8(spec.prepareOps, 0x72);
    }

    AppendOp(spec.processOps, F32Const(0.125f));
    AppendOp(spec.processOps, F32Const(0.125f));
    return MakeGuestModule(spec);
}

// Asks resource_size about handles 0 and 1 instead of asking resource_handle.
ByteVector MakeHardCodedResourceHandleModule()
{
    auto spec = ResourceGuest(BuildDescriptorBlob({{"resource.data.title", "Data"}}), {});
    AppendOp(spec.prepareOps, I32Const(0));

    for (int32_t handle = 0; handle < 2; ++handle)
    {
        AppendOp(spec.processOps, I32Const(handle));
        AppendOp(spec.processOps, Call(kResourceSizeImport));
        AppendU8(spec.processOps, 0xb3);
    }

    return MakeGuestModule(spec);
}

ByteVector MakeStatefulBiasModule()
{
    GuestSpec spec;
    spec.globals = {{WasmValueType::F32, true, F32Const(0.0f)}};

    AppendOp(spec.resetOps, F32Const(0.0f));
    AppendOp(spec.resetOps, GlobalSet(0));

    AppendOp(spec.processOps, GlobalGet(0));
    AppendOp(spec.processOps, F32Const(0.01f));
    AppendU8(spec.processOps, 0x92);
    AppendOp(spec.processOps, GlobalSet(0));

    for (uint32_t channel = 0; channel < 2; ++channel)
    {
        AppendOp(spec.processOps, LocalGet(channel));
        AppendOp(spec.processOps, GlobalGet(0));
        AppendU8(spec.processOps, 0x92);
    }

    return MakeGuestModule(spec);
}

ByteVector MakeDescriptorModule(const ByteVector& descriptorBlob)
{
    GuestSpec spec;
    spec.descriptor = descriptorBlob;
    return MakeGuestModule(spec);
}

bool WriteBytes(const std::filesystem::path& path, const ByteVector& bytes)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);

    if (!stream.is_open())
    {
        return false;
    }

    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return stream.good();
}

bool NearlyEqual(float actual, float expected, float tolerance = kTolerance)
{
    return std::fabs(actual - expected) <= tolerance;
}

std::unique_ptr<EffectProcessor> CreateWasmHost()
{
    return EffectRegistry::Instance().Create(EffectGuids::kWasmHost);
}

std::unique_ptr<EffectProcessor> LoadModuleFile(const std::filesystem::path& modulePath, bool& loaded)
{
    auto effect = CreateWasmHost();
    loaded = false;

    if (!effect)
    {
        return nullptr;
    }

    ResourceRef moduleRef;
    moduleRef.resourceType = "wasm";
    moduleRef.filePath = modulePath.string();
    loaded = effect->LoadResources({moduleRef}, {modulePath});
    return effect;
}

// Writes the module, loads it into a fresh effect and prepares it; null if any step fails.
std::unique_ptr<EffectProcessor> LoadPrepared(const TempDir& tempDir, const char* fileName, const ByteVector& module)
{
    const auto modulePath = tempDir.root / fileName;

    if (!WriteBytes(modulePath, module))
    {
        std::cerr << "Failed to write " << fileName << ".\n";
        return nullptr;
    }

    bool loaded = false;
    auto effect = LoadModuleFile(modulePath, loaded);

    if (!effect || !loaded)
    {
        std::cerr << fileName << " failed to load: " << (effect ? effect->GetConfig("lastError") : std::string{})
                  << "\n";
        return nullptr;
    }

    effect->Prepare(kSampleRate, kBlockSize);
    return effect;
}

// One block of the given constant input; returns the first output sample of each channel.
std::pair<float, float> ProcessBlock(EffectProcessor& effect, float left, float right)
{
    std::array<float, kBlockSize> leftIn{};
    std::array<float, kBlockSize> rightIn{};
    std::array<float, kBlockSize> leftOut{};
    std::array<float, kBlockSize> rightOut{};
    leftIn.fill(left);
    rightIn.fill(right);
    float* inputs[] = {leftIn.data(), rightIn.data()};
    float* outputs[] = {leftOut.data(), rightOut.data()};
    effect.Process(inputs, outputs, kBlockSize);
    return {leftOut[0], rightOut[0]};
}

bool TestRegistryMetadata()
{
    const auto info = EffectRegistry::Instance().GetTypeInfo(EffectGuids::kWasmHost);

    if (!info)
    {
        std::cerr << "WASM host effect was not registered.\n";
        return false;
    }

    if (!info->requiresResource || !info->requiresTempo || info->exposedResources.size() != 1 ||
        info->parameters.size() != 3)
    {
        std::cerr << "WASM host metadata is missing required resource or slot information.\n";
        return false;
    }

    for (const auto& param : info->parameters)
    {
        if (!WasmEffect::IsHostParamId(param.id))
        {
            std::cerr << "Host param '" << param.id << "' is missing from WasmEffect::kHostParamIds.\n";
            return false;
        }
    }

    return true;
}

bool TestDescriptorInspection(const TempDir& tempDir)
{
    const auto modulePath = tempDir.root / "gain_descriptor.wasm";

    if (!WriteBytes(modulePath, MakeGainModule()))
    {
        std::cerr << "Failed to write descriptor-enabled gain module.\n";
        return false;
    }

    std::string error;
    const auto descriptor = WasmEffect::InspectModuleFile(modulePath, &error);

    if (!descriptor)
    {
        std::cerr << "Failed to inspect WASM descriptor: " << error << "\n";
        return false;
    }

    if (descriptor->displayName != "WASM Gain" || descriptor->version != "1.2.3" ||
        descriptor->thumbnailDataUrl != "data:image/png;base64,dGVzdA==" || descriptor->parameters.size() != 1)
    {
        std::cerr << "Descriptor inspection returned unexpected metadata.\n";
        return false;
    }

    const auto& gain = descriptor->parameters.front().definition;

    if (gain.id != "gain" || gain.displayName != "Gain" || gain.defaultValue != 0.5 || gain.maxValue != 2.0 ||
        gain.unit != "amount")
    {
        std::cerr << "The 'gain' param's fields were not read from param.gain.*.\n";
        return false;
    }

    return true;
}

bool TestDirectGainModule(const TempDir& tempDir)
{
    auto effect = LoadPrepared(tempDir, "gain.wasm", MakeGainModule());

    if (!effect)
    {
        return false;
    }

    // The descriptor's default first, then a value set after the load.
    if (const auto [left, right] = ProcessBlock(*effect, 1.0f, -0.5f);
        !NearlyEqual(left, 0.5f) || !NearlyEqual(right, -0.25f))
    {
        std::cerr << "Gain module did not read its default through its handle: " << left << ", " << right << "\n";
        return false;
    }

    effect->SetParam("gain", 2.0);

    if (const auto [left, right] = ProcessBlock(*effect, 1.0f, -0.5f);
        !NearlyEqual(left, 2.0f) || !NearlyEqual(right, -1.0f))
    {
        std::cerr << "Gain module did not follow SetParam: " << left << ", " << right << "\n";
        return false;
    }

    // A rebuild keeps the value.
    effect->Prepare(kSampleRate, kBlockSize);

    if (const auto [left, right] = ProcessBlock(*effect, 1.0f, -0.5f); !NearlyEqual(left, 2.0f))
    {
        std::cerr << "Gain lost its value across Prepare: " << left << "\n";
        return false;
    }

    // An id the module didn't declare is not kept once a descriptor has loaded.
    effect->SetParam("notDeclared", 1.0);

    if (effect->GetParam("notDeclared") != 0.0 || effect->GetParam("gain") != 2.0)
    {
        std::cerr << "SetParam kept a value for an id the module never declared.\n";
        return false;
    }

    return true;
}

// The tempo has its own import; it never shares a numbering with params again.
bool TestTempoImport(const TempDir& tempDir)
{
    auto effect = LoadPrepared(tempDir, "tempo.wasm", MakeTempoModule());

    if (!effect)
    {
        return false;
    }

    if (const auto [left, right] = ProcessBlock(*effect, 0.0f, 0.0f);
        !NearlyEqual(left, 0.75f) || !NearlyEqual(right, 120.0f))
    {
        std::cerr << "Param default or tempo read back wrong: " << left << ", " << right << "\n";
        return false;
    }

    effect->SetParam("last", 0.25);
    effect->SetParam("bpm", 90.0);

    if (const auto [left, right] = ProcessBlock(*effect, 0.0f, 0.0f);
        !NearlyEqual(left, 0.25f) || !NearlyEqual(right, 90.0f) || effect->GetParam("last") != 0.25)
    {
        std::cerr << "Param or tempo did not follow SetParam: " << left << ", " << right << "\n";
        return false;
    }

    return true;
}

// A new version of a module that declares the same ids in another order reads the same values:
// nothing depends on where a param sits in the descriptor.
bool TestReorderedDescriptorKeepsValuesById(const TempDir& tempDir)
{
    const auto firstPath = tempDir.root / "pair.wasm";
    const auto reorderedPath = tempDir.root / "pair_reordered.wasm";

    if (!WriteBytes(firstPath, MakePairModule(false)) || !WriteBytes(reorderedPath, MakePairModule(true)))
    {
        std::cerr << "Failed to write the pair modules.\n";
        return false;
    }

    bool loaded = false;
    auto effect = LoadModuleFile(firstPath, loaded);

    if (!effect || !loaded)
    {
        std::cerr << "The pair module failed to load.\n";
        return false;
    }

    effect->SetParam("a", 0.2);
    effect->SetParam("b", 0.7);

    ResourceRef moduleRef;
    moduleRef.resourceType = "wasm";
    moduleRef.filePath = reorderedPath.string();

    if (!effect->LoadResources({moduleRef}, {reorderedPath}))
    {
        std::cerr << "The reordered pair module failed to load.\n";
        return false;
    }

    effect->Prepare(kSampleRate, kBlockSize);

    if (const auto [left, right] = ProcessBlock(*effect, 0.0f, 0.0f);
        !NearlyEqual(left, 0.2f) || !NearlyEqual(right, 0.7f))
    {
        std::cerr << "Reordering the descriptor moved values between params: " << left << ", " << right << "\n";
        return false;
    }

    return true;
}

// A module that reads 0 and 1 instead of asking param_handle traps, and the node passes dry.
bool TestHardCodedHandleTraps(const TempDir& tempDir)
{
    auto effect = LoadPrepared(tempDir, "hard_coded.wasm", MakeHardCodedHandleModule());

    if (!effect)
    {
        return false;
    }

    if (const auto [left, right] = ProcessBlock(*effect, 0.25f, -0.5f);
        !NearlyEqual(left, 0.25f) || !NearlyEqual(right, -0.5f))
    {
        std::cerr << "A hard-coded handle did not leave the node passing dry: " << left << ", " << right << "\n";
        return false;
    }

    if (effect->GetConfig("lastError").find("param_handle") == std::string::npos)
    {
        std::cerr << "A hard-coded handle's trap was not reported: " << effect->GetConfig("lastError") << "\n";
        return false;
    }

    return true;
}

bool TestUnknownIdsHaveNoHandle(const TempDir& tempDir)
{
    auto effect = LoadPrepared(tempDir, "unknown_id.wasm", MakeUnknownIdModule());

    if (!effect)
    {
        return false;
    }

    if (const auto [left, right] = ProcessBlock(*effect, 0.0f, 0.0f);
        !NearlyEqual(left, 0.125f) || !NearlyEqual(right, 0.125f))
    {
        std::cerr << "param_handle gave a handle for an undeclared, out-of-bounds, empty or over-long id: "
                  << effect->GetConfig("lastError") << "\n";
        return false;
    }

    return true;
}

// Each must be refused before it runs, with a message that says why.
bool TestOtherInterfacesAreRefused(const TempDir& tempDir)
{
    GuestSpec abi1;
    abi1.importTypes = {{{WasmValueType::I32}, {WasmValueType::F32}}};
    abi1.imports = {{"host", "read_param", 0}};
    abi1.abiVersion = std::nullopt;

    GuestSpec unversioned;
    unversioned.abiVersion = std::nullopt;

    GuestSpec futureAbi;
    futureAbi.abiVersion = 3;

    GuestSpec abi1Resources;
    abi1Resources.importTypes = {{{WasmValueType::I32, WasmValueType::I32}, {WasmValueType::I32}}};
    abi1Resources.imports = {{"host", "read_resource_byte", 0}};

    GuestSpec foreignImport;
    foreignImport.importTypes = {{{WasmValueType::F32}, {WasmValueType::F32}}};
    foreignImport.imports = {{"env", "sinf", 0}};

    const std::vector<std::tuple<const char*, GuestSpec, const char*>> cases = {
        {"abi1.wasm", abi1, "host.read_param"},
        {"abi1_resources.wasm", abi1Resources, "ABI 1 (it imports host.read_resource_byte)"},
        {"unversioned.wasm", unversioned, "audiofx_abi_version"},
        {"future_abi.wasm", futureAbi, "ABI 3"},
        {"foreign_import.wasm", foreignImport, "env.sinf"},
    };

    for (const auto& [fileName, spec, expected] : cases)
    {
        const auto modulePath = tempDir.root / fileName;

        if (!WriteBytes(modulePath, MakeGuestModule(spec)))
        {
            std::cerr << "Failed to write " << fileName << ".\n";
            return false;
        }

        bool loaded = false;
        auto effect = LoadModuleFile(modulePath, loaded);
        const std::string error = effect ? effect->GetConfig("lastError") : std::string{};

        if (loaded || error.find(expected) == std::string::npos)
        {
            std::cerr << fileName << " was not refused with a message naming '" << expected << "': " << error << "\n";
            return false;
        }
    }

    return true;
}

bool TestBadDescriptorsAreRefused(const TempDir& tempDir)
{
    std::vector<std::pair<std::string, std::string>> tooMany;

    for (std::size_t i = 0; i <= WasmEffect::kMaxGuestParams; ++i)
    {
        tooMany.push_back({"param.p" + std::to_string(i) + ".default", "0"});
    }

    std::vector<std::pair<std::string, std::string>> tooManyResources;

    for (std::size_t i = 0; i <= WasmEffect::kMaxGuestResources; ++i)
    {
        tooManyResources.push_back({"resource.r" + std::to_string(i) + ".type", "blob"});
    }

    const std::vector<std::pair<std::vector<std::pair<std::string, std::string>>, const char*>> cases = {
        {{{"param.0.id", "gain"}}, "numbers its param"},
        {{{"param.gain.slot", "0"}}, "no 'slot'"},
        {{{"param.gain.id", "gain"}}, "no 'id'"},
        {{{"param.mix.title", "Guest Mix"}}, "host's own controls"},
        {{{"param.bpm.title", "Guest Tempo"}}, "host's own controls"},
        {{{"param.9lives.title", "Lives"}}, "must be a letter"},
        {{{"param.gain", "1"}}, "names no field"},
        {tooMany, "the limit is 64"},
        {{{"resource.0.id", "impulse"}}, "numbers its resource"},
        {{{"resource.ir.slot", "1"}}, "no 'slot'"},
        {{{"resource.ir.id", "ir"}}, "no 'id'"},
        {{{"resource.module.title", "Another Module"}}, "module's own resource"},
        {{{"resource._ir.title", "Impulse"}}, "must be a letter"},
        {{{"resource.ir", "x"}}, "names no field"},
        {tooManyResources, "the limit is 16"},
    };

    for (const auto& [entries, expected] : cases)
    {
        const auto modulePath = tempDir.root / "bad_descriptor.wasm";

        if (!WriteBytes(modulePath, MakeDescriptorModule(BuildDescriptorBlob(entries))))
        {
            std::cerr << "Failed to write a bad-descriptor module.\n";
            return false;
        }

        std::string error;

        if (WasmEffect::InspectModuleFile(modulePath, &error) || error.find(expected) == std::string::npos)
        {
            std::cerr << "A descriptor starting '" << entries.front().first << "' was not refused with '" << expected
                      << "': " << error << "\n";
            return false;
        }
    }

    return true;
}

bool TestMonoInputToStereoGuestUsesDualMono(const TempDir& tempDir)
{
    auto effect = LoadPrepared(tempDir, "stereo_average_mono_input.wasm", MakeStereoAverageModule());

    if (!effect)
    {
        return false;
    }

    std::array<float, kBlockSize> leftIn{};
    std::array<float, kBlockSize> leftOut{};
    std::array<float, kBlockSize> rightOut{};
    leftIn[0] = 1.0f;

    float* inputs[] = {leftIn.data(), nullptr};
    float* outputs[] = {leftOut.data(), rightOut.data()};
    effect->Process(inputs, outputs, kBlockSize);

    if (!NearlyEqual(leftOut[0], 1.0f) || !NearlyEqual(rightOut[0], 1.0f))
    {
        std::cerr << "Mono input was not mirrored to dual-mono for stereo WASM guest processing.\n";
        return false;
    }

    return true;
}

// The node's params reach the module through the executor, by id.
bool TestSignalGraphStereoModule(const TempDir& tempDir)
{
    const auto modulePath = tempDir.root / "stereo_average.wasm";

    if (!WriteBytes(modulePath, MakeStereoAverageModule()))
    {
        std::cerr << "Failed to write stereo-average WASM module.\n";
        return false;
    }

    SignalGraph graph;
    graph.nodes.push_back(GraphNode{"__input__", kNodeTypeInput, "utility", "Input", true});

    GraphNode wasmNode;
    wasmNode.id = "wasm";
    wasmNode.type = EffectGuids::kWasmHost;
    wasmNode.category = "utility";
    wasmNode.label = "WASM Host";
    wasmNode.params["gain"] = 0.5;
    wasmNode.resources.resize(1);
    wasmNode.resources[0].resourceType = "wasm";
    wasmNode.resources[0].filePath = modulePath.string();
    graph.nodes.push_back(wasmNode);

    graph.nodes.push_back(GraphNode{"__output__", kNodeTypeOutput, "utility", "Output", true});

    graph.edges.push_back(GraphEdge{"__input__", "wasm", 0, 0, 1.0});
    graph.edges.push_back(GraphEdge{"wasm", "__output__", 0, 0, 1.0});

    ResourceLibrary resourceLibrary;
    SignalGraphExecutor executor;
    executor.SetResourceLibrary(&resourceLibrary);
    executor.SetGraph(graph);
    executor.Prepare(kSampleRate, kBlockSize);

    std::array<float, kBlockSize> leftIn{};
    std::array<float, kBlockSize> rightIn{};
    std::array<float, kBlockSize> leftOut{};
    std::array<float, kBlockSize> rightOut{};
    std::fill(leftIn.begin(), leftIn.end(), 1.0f);
    std::fill(rightIn.begin(), rightIn.end(), 0.0f);

    float* inputs[] = {leftIn.data(), rightIn.data()};
    float* outputs[] = {leftOut.data(), rightOut.data()};
    executor.Process(inputs, outputs, kBlockSize);

    if (!NearlyEqual(leftOut[0], 0.25f) || !NearlyEqual(rightOut[0], 0.25f))
    {
        std::cerr << "Signal graph stereo WASM node output mismatch: " << leftOut[0] << ", " << rightOut[0] << "\n";
        return false;
    }

    return true;
}

ResourceRef FileRef(const std::filesystem::path& path, const char* type, std::size_t slot)
{
    ResourceRef ref;
    ref.resourceType = type;
    ref.filePath = path.string();
    ref.metadata["resourceSlotIndex"] = std::to_string(slot);
    return ref;
}

// The pair modules and two one-byte files: 'a' holds 16 (0.25 out) and 'b' 48 (0.75 out).
struct ResourcePairFiles
{
    std::filesystem::path module;
    std::filesystem::path reversedModule;
    std::filesystem::path a;
    std::filesystem::path b;
};

std::optional<ResourcePairFiles> WriteResourcePairFiles(const TempDir& tempDir)
{
    ResourcePairFiles files{tempDir.root / "resource_pair.wasm", tempDir.root / "resource_pair_reversed.wasm",
                            tempDir.root / "a.bin", tempDir.root / "b.bin"};

    if (!WriteBytes(files.module, MakeResourcePairModule(false)) ||
        !WriteBytes(files.reversedModule, MakeResourcePairModule(true)) || !WriteBytes(files.a, {16u}) ||
        !WriteBytes(files.b, {48u}))
    {
        std::cerr << "Failed to write the resource-pair test files.\n";
        return std::nullopt;
    }

    return files;
}

// Loads `module` with `first` and `second` at node resources 1 and 2, after `resourceIds` if any.
std::optional<std::pair<float, float>> RunResourcePair(const std::filesystem::path& module,
                                                       const std::filesystem::path& first,
                                                       const std::filesystem::path& second,
                                                       const std::string& resourceIds = {})
{
    auto effect = CreateWasmHost();

    if (!resourceIds.empty())
    {
        effect->SetConfig(WasmEffect::kResourceIdsConfigKey, resourceIds);
    }

    if (!effect->LoadResources({FileRef(module, "wasm", 0), FileRef(first, "blob", 1), FileRef(second, "blob", 2)},
                               {module, first, second}))
    {
        std::cerr << "The resource-pair module failed to load: " << effect->GetConfig("lastError") << "\n";
        return std::nullopt;
    }

    effect->Prepare(kSampleRate, kBlockSize);
    return ProcessBlock(*effect, 0.0f, 0.0f);
}

bool TestResourcesById(const TempDir& tempDir)
{
    const auto files = WriteResourcePairFiles(tempDir);

    if (!files)
    {
        return false;
    }

    // With no ids saved, each file is the resource declared at its place.
    if (const auto out = RunResourcePair(files->module, files->a, files->b);
        !out || !NearlyEqual(out->first, 0.25f) || !NearlyEqual(out->second, 0.75f))
    {
        std::cerr << "Resources read back wrong by declaration order.\n";
        return false;
    }

    // Saved ids win over places: here the module declares b first, and the files are where a
    // module declaring a first put them.
    if (const auto out = RunResourcePair(files->reversedModule, files->a, files->b, R"(["","a","b"])");
        !out || !NearlyEqual(out->first, 0.25f) || !NearlyEqual(out->second, 0.75f))
    {
        std::cerr << "Saved resource ids did not decide which file a resource reads.\n";
        return false;
    }

    // An id the module doesn't declare takes no resource, and a bad one is ignored.
    if (const auto out = RunResourcePair(files->module, files->a, files->b, R"(["","gone","9bad"])");
        !out || !NearlyEqual(out->first, 0.0f) || !NearlyEqual(out->second, 0.75f))
    {
        std::cerr << "An undeclared or malformed saved resource id still took a file.\n";
        return false;
    }

    return true;
}

// The executor sets the node's config before it loads its resources, so saved ids reach the
// binding.
bool TestSignalGraphResourcesById(const TempDir& tempDir)
{
    const auto files = WriteResourcePairFiles(tempDir);

    if (!files)
    {
        return false;
    }

    SignalGraph graph;
    graph.nodes.push_back(GraphNode{"__input__", kNodeTypeInput, "utility", "Input", true});

    GraphNode wasmNode;
    wasmNode.id = "wasm";
    wasmNode.type = EffectGuids::kWasmHost;
    wasmNode.category = "utility";
    wasmNode.params["mix"] = 1.0;
    wasmNode.config[WasmEffect::kResourceIdsConfigKey] = R"(["","b","a"])";
    wasmNode.resources = {FileRef(files->module, "wasm", 0), FileRef(files->b, "blob", 1),
                          FileRef(files->a, "blob", 2)};

    // The executor stamps slots itself.
    for (auto& resource : wasmNode.resources)
    {
        resource.metadata.clear();
    }

    graph.nodes.push_back(wasmNode);
    graph.nodes.push_back(GraphNode{"__output__", kNodeTypeOutput, "utility", "Output", true});
    graph.edges.push_back(GraphEdge{"__input__", "wasm", 0, 0, 1.0});
    graph.edges.push_back(GraphEdge{"wasm", "__output__", 0, 0, 1.0});

    ResourceLibrary resourceLibrary;
    SignalGraphExecutor executor;
    executor.SetResourceLibrary(&resourceLibrary);
    executor.SetGraph(graph);
    executor.Prepare(kSampleRate, kBlockSize);

    std::array<float, kBlockSize> leftIn{};
    std::array<float, kBlockSize> rightIn{};
    std::array<float, kBlockSize> leftOut{};
    std::array<float, kBlockSize> rightOut{};
    float* inputs[] = {leftIn.data(), rightIn.data()};
    float* outputs[] = {leftOut.data(), rightOut.data()};
    executor.Process(inputs, outputs, kBlockSize);

    if (!NearlyEqual(leftOut[0], 0.25f) || !NearlyEqual(rightOut[0], 0.75f))
    {
        std::cerr << "The executor did not bind resources by their saved ids: " << leftOut[0] << ", " << rightOut[0]
                  << "\n";
        return false;
    }

    return true;
}

WasmModuleDescriptor DescriptorWithResources(std::vector<std::string> ids)
{
    WasmModuleDescriptor descriptor;

    for (auto& id : ids)
    {
        ExposedResource resource;
        resource.resourceId = std::move(id);
        descriptor.exposedResources.push_back(std::move(resource));
    }

    return descriptor;
}

ResourceRef NamedFile(const char* name)
{
    ResourceRef ref;
    ref.resourceType = "blob";
    ref.filePath = name;
    return ref;
}

// What the controller does to a node when a new version of its module is chosen or a preset loads.
bool TestRekeyNodeResources()
{
    GraphNode node;
    node.resources = {NamedFile("module.wasm"), NamedFile("a.bin"), NamedFile("b.bin")};

    // Chosen against a module declaring a, b; its new version declares b, a and then c.
    auto dropped = WasmEffect::RekeyNodeResources(node, DescriptorWithResources({"b", "a", "c"}),
                                                  DescriptorWithResources({"a", "b"}));

    if (!dropped.empty() || node.resources.size() != 3 || node.resources[0].filePath != "module.wasm" ||
        node.resources[1].filePath != "b.bin" || node.resources[2].filePath != "a.bin" ||
        node.config[WasmEffect::kResourceIdsConfigKey] != R"(["","b","a"])")
    {
        std::cerr << "Reordered resources did not keep their files by id.\n";
        return false;
    }

    // Saved ids now say which file is whose, whatever `placedBy` claims. The next version drops a.
    dropped = WasmEffect::RekeyNodeResources(node, DescriptorWithResources({"b"}), DescriptorWithResources({"x", "y"}));

    if (dropped != std::vector<std::string>{"a"} || node.resources.size() != 2 ||
        node.resources[1].filePath != "b.bin" || node.config[WasmEffect::kResourceIdsConfigKey] != R"(["","b"])")
    {
        std::cerr << "A resource the module no longer declares was not dropped, or b moved.\n";
        return false;
    }

    // A gap stays empty, the trailing empty place goes, and with no guest files the ids go too.
    node.resources = {NamedFile("module.wasm"), ResourceRef{}, NamedFile("b.bin")};
    node.config.erase(WasmEffect::kResourceIdsConfigKey);
    dropped = WasmEffect::RekeyNodeResources(node, DescriptorWithResources({"a", "b", "c"}),
                                             DescriptorWithResources({"a", "b", "c"}));

    if (!dropped.empty() || node.resources.size() != 3 || node.resources[1].IsValid() ||
        node.resources[2].filePath != "b.bin")
    {
        std::cerr << "A gap between chosen files did not stay where it was.\n";
        return false;
    }

    node.resources.resize(1);
    dropped = WasmEffect::RekeyNodeResources(node, DescriptorWithResources({"a"}), DescriptorWithResources({"a"}));

    if (!dropped.empty() || node.resources.size() != 1 || node.config.contains(WasmEffect::kResourceIdsConfigKey))
    {
        std::cerr << "A node with no guest files kept its saved resource ids.\n";
        return false;
    }

    return true;
}

bool TestResourceReadEdges(const TempDir& tempDir)
{
    const auto modulePath = tempDir.root / "resource_edges.wasm";
    const auto dataPath = tempDir.root / "edges.bin";

    if (!WriteBytes(modulePath, MakeResourceEdgeModule()) || !WriteBytes(dataPath, {1u, 2u, 3u, 4u}))
    {
        std::cerr << "Failed to write the resource-edge test files.\n";
        return false;
    }

    auto effect = CreateWasmHost();

    if (!effect->LoadResources({FileRef(modulePath, "wasm", 0), FileRef(dataPath, "blob", 1)}, {modulePath, dataPath}))
    {
        std::cerr << "The resource-edge module failed to load: " << effect->GetConfig("lastError") << "\n";
        return false;
    }

    effect->Prepare(kSampleRate, kBlockSize);

    if (const auto [left, right] = ProcessBlock(*effect, 0.0f, 0.0f);
        !NearlyEqual(left, 0.125f) || !NearlyEqual(right, 0.125f))
    {
        std::cerr << "A resource reader edge check failed (bit i of the status is check i): "
                  << effect->GetConfig("lastError") << "\n";
        return false;
    }

    return true;
}

bool TestHardCodedResourceHandleTraps(const TempDir& tempDir)
{
    auto effect = LoadPrepared(tempDir, "hard_coded_resource.wasm", MakeHardCodedResourceHandleModule());

    if (!effect)
    {
        return false;
    }

    if (const auto [left, right] = ProcessBlock(*effect, 0.25f, -0.5f);
        !NearlyEqual(left, 0.25f) || !NearlyEqual(right, -0.5f) ||
        effect->GetConfig("lastError").find("resource_handle") == std::string::npos)
    {
        std::cerr << "A hard-coded resource handle did not trap: " << effect->GetConfig("lastError") << "\n";
        return false;
    }

    return true;
}

bool TestResetStatefulModule(const TempDir& tempDir)
{
    auto effect = LoadPrepared(tempDir, "stateful_bias.wasm", MakeStatefulBiasModule());

    if (!effect)
    {
        return false;
    }

    const float firstRunValue = ProcessBlock(*effect, 0.0f, 0.0f).first;
    const float secondRunValue = ProcessBlock(*effect, 0.0f, 0.0f).first;
    effect->Reset();
    const float resetRunValue = ProcessBlock(*effect, 0.0f, 0.0f).first;

    if (!(secondRunValue > firstRunValue) || !NearlyEqual(resetRunValue, firstRunValue))
    {
        std::cerr << "Stateful module reset behavior mismatch.\n";
        return false;
    }

    return true;
}

// Modules that misbehave on purpose. A module is code from whoever shared it, so each of these
// must end in a trap or a refused load, never a hang or a crash.
enum class HostileKind
{
    LoopingProcess,
    RecursiveProcess,
    LoopingStart,
    HugeMemory,
};

ByteVector MakeHostileModule(HostileKind kind)
{
    // loop (br 0) end; unreachable — the unreachable makes the body valid for any result type.
    const ByteVector foreverOps = {0x03, 0x40, 0x0c, 0x00, 0x0b, 0x00};

    GuestSpec spec;

    switch (kind)
    {
    case HostileKind::LoopingProcess:
        spec.processOps = foreverOps;
        break;
    case HostileKind::RecursiveProcess:
        // Process is function 2 in a module with no imports.
        AppendOp(spec.processOps, LocalGet(0));
        AppendOp(spec.processOps, LocalGet(1));
        AppendOp(spec.processOps, Call(2));
        break;
    case HostileKind::LoopingStart:
        spec.startOps = foreverOps;
        break;
    case HostileKind::HugeMemory:
        // One page past the store's 256 MiB limit.
        spec.memoryMinPages = 4097;
        break;
    }

    return MakeGuestModule(spec);
}

double SecondsSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// The guest's process runs on the audio thread; one that never returns must be stopped by its
// deadline and the node must pass the input through dry from then on.
bool TestHostileProcessIsStopped(const TempDir& tempDir, HostileKind kind, const char* label)
{
    auto effect = LoadPrepared(tempDir, (std::string(label) + ".wasm").c_str(), MakeHostileModule(kind));

    if (!effect)
    {
        return false;
    }

    std::array<float, kBlockSize> leftIn{};
    std::array<float, kBlockSize> rightIn{};
    std::array<float, kBlockSize> leftOut{};
    std::array<float, kBlockSize> rightOut{};
    leftIn.fill(0.25f);
    rightIn.fill(-0.5f);
    float* inputs[] = {leftIn.data(), rightIn.data()};
    float* outputs[] = {leftOut.data(), rightOut.data()};

    const auto start = std::chrono::steady_clock::now();
    effect->Process(inputs, outputs, kBlockSize);
    effect->Process(inputs, outputs, kBlockSize);
    const double elapsed = SecondsSince(start);

    // The block budget is 0.2 s; allow for a slow CI machine, but not for a hang.
    if (elapsed > 2.0)
    {
        std::cerr << "The " << label << " module held Process for " << elapsed << " s.\n";
        return false;
    }

    if (!NearlyEqual(leftOut[kBlockSize - 1], 0.25f) || !NearlyEqual(rightOut[kBlockSize - 1], -0.5f))
    {
        std::cerr << "The " << label << " module did not leave the node passing dry.\n";
        return false;
    }

    if (effect->GetConfig("lastError").empty())
    {
        std::cerr << "The " << label << " module's trap was not reported.\n";
        return false;
    }

    return true;
}

bool TestLoopingStartFunctionIsStopped(const TempDir& tempDir)
{
    const auto modulePath = tempDir.root / "looping_start.wasm";

    if (!WriteBytes(modulePath, MakeHostileModule(HostileKind::LoopingStart)))
    {
        std::cerr << "Failed to write the looping-start module.\n";
        return false;
    }

    // Inspection runs on the message thread when a preset loads; the start function must not
    // freeze it. The setup budget is 5 s.
    const auto start = std::chrono::steady_clock::now();
    std::string error;
    const auto descriptor = WasmEffect::InspectModuleFile(modulePath, &error);
    const double elapsed = SecondsSince(start);

    if (descriptor || error.find("time limit") == std::string::npos)
    {
        std::cerr << "A looping start function was not stopped by its deadline: " << error << "\n";
        return false;
    }

    if (elapsed > 15.0)
    {
        std::cerr << "A looping start function held inspection for " << elapsed << " s.\n";
        return false;
    }

    return true;
}

bool TestHugeMemoryIsRefused(const TempDir& tempDir)
{
    const auto modulePath = tempDir.root / "huge_memory.wasm";

    if (!WriteBytes(modulePath, MakeHostileModule(HostileKind::HugeMemory)))
    {
        std::cerr << "Failed to write the huge-memory module.\n";
        return false;
    }

    bool loaded = false;
    auto effect = LoadModuleFile(modulePath, loaded);

    if (loaded)
    {
        std::cerr << "A module asking for more than the memory limit was instantiated.\n";
        return false;
    }

    return true;
}

bool TestNetworkPathsAreRefused(const TempDir& tempDir)
{
    const std::vector<std::pair<std::filesystem::path, bool>> cases = {
        {std::filesystem::path(L"\\\\server\\share\\fx.wasm"), true},
        {std::filesystem::path(L"//server/share/fx.wasm"), true},
        {std::filesystem::path(L"\\\\?\\UNC\\server\\share\\fx.wasm"), true},
        {std::filesystem::path(L"\\\\?\\C:\\fx.wasm"), false},
        {std::filesystem::path(L"C:\\fx.wasm"), false},
        {tempDir.root / "fx.wasm", false},
    };

    for (const auto& [path, network] : cases)
    {
        if (WasmEffect::IsNetworkPath(path) != network)
        {
            std::cerr << "IsNetworkPath got " << path.string() << " wrong.\n";
            return false;
        }
    }

    bool loaded = false;
    auto effect = LoadModuleFile(std::filesystem::path(L"\\\\server.invalid\\share\\fx.wasm"), loaded);

    if (loaded || !effect || effect->GetConfig("lastError").find("network") == std::string::npos)
    {
        std::cerr << "A module on a network share was not refused up front.\n";
        return false;
    }

    std::string error;

    if (WasmEffect::InspectModuleFile(std::filesystem::path(L"\\\\server.invalid\\share\\fx.wasm"), &error) ||
        error.find("network") == std::string::npos)
    {
        std::cerr << "Inspecting a module on a network share was not refused up front.\n";
        return false;
    }

    return true;
}

bool TestHostingGate(const TempDir& tempDir)
{
    const auto modulePath = tempDir.root / "gated_gain.wasm";

    if (!WriteBytes(modulePath, MakeGainModule()))
    {
        std::cerr << "Failed to write the gated gain module.\n";
        return false;
    }

    WasmEffect::SetHostingEnabled(false);
    bool loaded = false;
    auto effect = LoadModuleFile(modulePath, loaded);
    std::string error;
    const auto descriptor = WasmEffect::InspectModuleFile(modulePath, &error);
    WasmEffect::SetHostingEnabled(true);

    if (loaded || descriptor)
    {
        std::cerr << "A module loaded while Custom Effects were turned off.\n";
        return false;
    }

    auto enabledEffect = LoadModuleFile(modulePath, loaded);

    if (!loaded)
    {
        std::cerr << "The gain module did not load once Custom Effects were turned back on.\n";
        return false;
    }

    return true;
}

// A module's category and thumbnail end up in UI markup and in the Custom Effect library.
bool TestHostileDescriptorIsSanitised(const TempDir& tempDir)
{
    const auto modulePath = tempDir.root / "hostile_descriptor.wasm";
    const ByteVector blob = BuildDescriptorBlob({
        {"effect.name", "Looks Normal"},
        {"effect.category", "<iframe src=\"https://example.invalid\"></iframe>"},
        {"effect.thumbnailDataUrl", "https://example.invalid/pixel.png"},
        {"param.amount.title", "Amount"},
    });

    if (!WriteBytes(modulePath, MakeDescriptorModule(blob)))
    {
        std::cerr << "Failed to write the hostile-descriptor module.\n";
        return false;
    }

    std::string error;
    const auto descriptor = WasmEffect::InspectModuleFile(modulePath, &error);

    if (!descriptor)
    {
        std::cerr << "Failed to inspect the hostile-descriptor module: " << error << "\n";
        return false;
    }

    if (descriptor->category != "utility" || !descriptor->thumbnailDataUrl.empty())
    {
        std::cerr << "A hostile category or thumbnail URL survived descriptor parsing.\n";
        return false;
    }

    for (const auto& entry : descriptor->entries)
    {
        if ((entry.key == "effect.category" && entry.value != "utility") || entry.key.rfind("effect.thumbnail", 0) == 0)
        {
            std::cerr << "The stored descriptor entries still carry '" << entry.key << "' unsanitised.\n";
            return false;
        }
    }

    return true;
}
} // namespace
} // namespace guitarfx

int main()
{
    guitarfx::RegisterAllEffects();
    // The app sets this from the Custom Effects feature setting.
    guitarfx::WasmEffect::SetHostingEnabled(true);

    guitarfx::TempDir tempDir;
    bool allPassed = true;
    allPassed = guitarfx::TestRegistryMetadata() && allPassed;
    allPassed = guitarfx::TestDescriptorInspection(tempDir) && allPassed;
    allPassed = guitarfx::TestDirectGainModule(tempDir) && allPassed;
    allPassed = guitarfx::TestTempoImport(tempDir) && allPassed;
    allPassed = guitarfx::TestReorderedDescriptorKeepsValuesById(tempDir) && allPassed;
    allPassed = guitarfx::TestHardCodedHandleTraps(tempDir) && allPassed;
    allPassed = guitarfx::TestUnknownIdsHaveNoHandle(tempDir) && allPassed;
    allPassed = guitarfx::TestOtherInterfacesAreRefused(tempDir) && allPassed;
    allPassed = guitarfx::TestBadDescriptorsAreRefused(tempDir) && allPassed;
    allPassed = guitarfx::TestMonoInputToStereoGuestUsesDualMono(tempDir) && allPassed;
    allPassed = guitarfx::TestSignalGraphStereoModule(tempDir) && allPassed;
    allPassed = guitarfx::TestResourcesById(tempDir) && allPassed;
    allPassed = guitarfx::TestSignalGraphResourcesById(tempDir) && allPassed;
    allPassed = guitarfx::TestRekeyNodeResources() && allPassed;
    allPassed = guitarfx::TestResourceReadEdges(tempDir) && allPassed;
    allPassed = guitarfx::TestHardCodedResourceHandleTraps(tempDir) && allPassed;
    allPassed = guitarfx::TestResetStatefulModule(tempDir) && allPassed;
    allPassed =
        guitarfx::TestHostileProcessIsStopped(tempDir, guitarfx::HostileKind::LoopingProcess, "looping_process") &&
        allPassed;
    allPassed =
        guitarfx::TestHostileProcessIsStopped(tempDir, guitarfx::HostileKind::RecursiveProcess, "recursive_process") &&
        allPassed;
    allPassed = guitarfx::TestLoopingStartFunctionIsStopped(tempDir) && allPassed;
    allPassed = guitarfx::TestHugeMemoryIsRefused(tempDir) && allPassed;
    allPassed = guitarfx::TestNetworkPathsAreRefused(tempDir) && allPassed;
    allPassed = guitarfx::TestHostingGate(tempDir) && allPassed;
    allPassed = guitarfx::TestHostileDescriptorIsSanitised(tempDir) && allPassed;

    if (!allPassed)
    {
        std::cerr << "WasmEffectHostTests failed.\n";
        return 1;
    }

    std::cout << "WasmEffectHostTests passed.\n";
    return 0;
}
