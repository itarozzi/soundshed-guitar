#pragma once

// A looper project on disk (docs/plans/looper.md, "Looper projects"):
//
//   <riff library>/loops/<projectId>/
//       project.json
//       layers/<layerKey>.di.wav    32-bit float, 1 or 2 channels, the recorded span only
//       layers/<layerKey>.wet.wav   32-bit float, stereo, the span and its tail
//
// The session folder (`loops/.session/`) has the same layout and is written as layers finish, so a
// crash loses at most the layer in progress; on the next launch it is moved to `loops/.recover/`
// and offered back. Plain file helpers, apart from ExtractLayer, which reads an engine slot's audio.

#include "dsp/looper/LooperEngine.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace guitarfx::looper_files
{
using Channels = std::vector<std::vector<float>>;

inline constexpr const char* kLoopsFolder = "loops";
inline constexpr const char* kSessionFolder = ".session";
inline constexpr const char* kRecoverFolder = ".recover";
inline constexpr const char* kProjectFile = "project.json";
inline constexpr int kProjectVersion = 1;

[[nodiscard]] std::filesystem::path DiPath(const std::filesystem::path& project, const std::string& layerKey);
[[nodiscard]] std::filesystem::path WetPath(const std::filesystem::path& project, const std::string& layerKey);

/// Both of a layer's files, written atomically. False on any failure.
bool WriteLayer(const std::filesystem::path& project, const std::string& layerKey, const Channels& di,
                const Channels& wet, int sampleRate);
bool WriteJson(const std::filesystem::path& file, const nlohmann::json& json);
[[nodiscard]] std::optional<nlohmann::json> ReadJson(const std::filesystem::path& file);
/// A WAV's channels at `sampleRate`, resampled when it was saved at another.
[[nodiscard]] std::optional<Channels> ReadWav(const std::filesystem::path& file, double sampleRate);

/// A finished layer as it is saved: the DI over its span, the wet sound over its span and tail.
struct LayerCopy
{
    Channels di;
    Channels wet;
};

/// Reads `slot`'s audio, which nothing writes once the layer is final.
[[nodiscard]] LayerCopy ExtractLayer(const looper::SlotInfo& slot, std::int64_t length);

/// Makes `to` exactly the project at `from` for the layers `keys`: project.json, and each layer's
/// files copied across. Layer files `to` held for other keys go. Written beside `to` and renamed
/// over it, so a failed save leaves the old project as it was.
bool PublishProject(const std::filesystem::path& from, const std::filesystem::path& to,
                    const std::vector<std::string>& keys, const nlohmann::json& project);

/// A random 16-hex-digit key, for a layer or a project.
[[nodiscard]] std::string NewKey();
/// UTC, ISO 8601.
[[nodiscard]] std::string NowIso();
} // namespace guitarfx::looper_files
