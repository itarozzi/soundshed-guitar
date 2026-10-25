// Looper project files. See LooperProjectFiles.h.

#include "controller/LooperProjectFiles.h"

#include "util/FileIO.h"
#include "util/PathEncoding.h"
#include "util/Wav.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>

namespace guitarfx::looper_files
{
namespace fs = std::filesystem;

fs::path DiPath(const fs::path& project, const std::string& layerKey)
{
    return project / "layers" / (layerKey + ".di.wav");
}

fs::path WetPath(const fs::path& project, const std::string& layerKey)
{
    return project / "layers" / (layerKey + ".wet.wav");
}

namespace
{
bool WriteChannels(const fs::path& file, const Channels& channels, int sampleRate)
{
    std::vector<const std::vector<float>*> pointers;

    for (const auto& channel : channels)
    {
        pointers.push_back(&channel);
    }

    const auto bytes = util::Encode32BitFloatWav(pointers, sampleRate);
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    return !bytes.empty() && util::WriteFileAtomic(file, bytes);
}
} // namespace

bool WriteLayer(const fs::path& project, const std::string& layerKey, const Channels& di, const Channels& wet,
                int sampleRate)
{
    return WriteChannels(DiPath(project, layerKey), di, sampleRate) &&
           WriteChannels(WetPath(project, layerKey), wet, sampleRate);
}

bool WriteJson(const fs::path& file, const nlohmann::json& json)
{
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    return util::WriteTextFileAtomic(file, json.dump(2));
}

std::optional<nlohmann::json> ReadJson(const fs::path& file)
{
    const auto bytes = util::ReadFileBytes(file);

    if (bytes.empty())
    {
        return std::nullopt;
    }

    auto json = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);

    if (json.is_discarded() || !json.is_object())
    {
        return std::nullopt;
    }

    return json;
}

std::optional<Channels> ReadWav(const fs::path& file, double sampleRate)
{
    const auto decoded = util::DecodePcmWav(util::ReadFileBytes(file));

    if (!decoded || decoded->channelSamples.empty())
    {
        return std::nullopt;
    }

    if (decoded->sampleRate == sampleRate)
    {
        Channels channels;

        for (const auto& source : decoded->channelSamples)
        {
            auto& channel = channels.emplace_back(source.size());

            for (std::size_t i = 0; i < source.size(); ++i)
            {
                channel[i] = static_cast<float>(source[i]);
            }
        }

        return channels;
    }

    return util::ConvertToSampleRate(*decoded, sampleRate);
}

/// A finished layer as it is saved: the DI over its span, the wet sound over its span and tail.
LayerCopy ExtractLayer(const looper::SlotInfo& slot, std::int64_t length)
{
    const looper::LayerAudio& audio = *slot.audio;
    const std::int64_t span = slot.firstLayer ? length : slot.spanLength;
    const std::int64_t wet = slot.firstLayer ? length : std::min(length, slot.spanLength + slot.ringOut);
    const int diChannels = std::clamp(slot.diChannels, 1, audio.di[1] ? 2 : 1);
    LayerCopy copy;
    copy.di.assign(static_cast<std::size_t>(diChannels), std::vector<float>(static_cast<std::size_t>(span)));
    copy.wet.assign(2, std::vector<float>(static_cast<std::size_t>(wet)));

    for (std::int64_t k = 0; k < wet; ++k)
    {
        const std::int64_t pos = (slot.spanStart + k) % length;
        const auto index = static_cast<std::size_t>((slot.head + pos) % audio.capacity);
        const auto out = static_cast<std::size_t>(k);
        copy.wet[0][out] = audio.wet[0][index];
        copy.wet[1][out] = audio.wet[1][index];

        if (k < span)
        {
            for (int c = 0; c < diChannels; ++c)
            {
                copy.di[static_cast<std::size_t>(c)][out] = audio.di[c][index];
            }
        }
    }

    return copy;
}


bool PublishProject(const fs::path& from, const fs::path& to, const std::vector<std::string>& keys,
                    const nlohmann::json& project)
{
    std::error_code ec;
    const fs::path staging = to.parent_path() / (to.filename().string() + ".saving");
    fs::remove_all(staging, ec);
    fs::create_directories(staging / "layers", ec);

    if (ec)
    {
        return false;
    }

    for (const auto& key : keys)
    {
        for (const auto& path : {DiPath(from, key), WetPath(from, key)})
        {
            fs::copy_file(path, staging / "layers" / path.filename(), fs::copy_options::overwrite_existing, ec);

            if (ec)
            {
                fs::remove_all(staging, ec);
                return false;
            }
        }
    }

    if (!WriteJson(staging / kProjectFile, project))
    {
        fs::remove_all(staging, ec);
        return false;
    }

    const fs::path old = to.parent_path() / (to.filename().string() + ".old");
    fs::remove_all(old, ec);

    if (fs::exists(to))
    {
        fs::rename(to, old, ec);

        if (ec)
        {
            fs::remove_all(staging, ec);
            return false;
        }
    }

    fs::rename(staging, to, ec);

    if (ec)
    {
        std::error_code restore;
        fs::rename(old, to, restore);
        return false;
    }

    fs::remove_all(old, ec);
    return true;
}

std::string NewKey()
{
    static thread_local std::mt19937_64 generator{std::random_device{}()};
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << generator();
    return out.str();
}

std::string NowIso()
{
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}
} // namespace guitarfx::looper_files
