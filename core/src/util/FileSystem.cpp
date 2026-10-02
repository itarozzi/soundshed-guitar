#include "FileSystem.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace guitarfx
{
namespace
{
std::mutex& PlatformRootMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::filesystem::path& PlatformRootOverride()
{
    static std::filesystem::path root;
    return root;
}
} // namespace

void FileSystem::SetPlatformRootOverride(const std::filesystem::path& root)
{
    const std::lock_guard<std::mutex> lock(PlatformRootMutex());
    PlatformRootOverride() = root;
}

std::filesystem::path FileSystem::ReadEnvironmentPath(const char* name)
{
#ifdef _WIN32
    // Variable names are ASCII, so widening them byte by byte is exact.
    const std::wstring wideName(name, name + std::strlen(name));
    wchar_t* value = nullptr;
    std::size_t length = 0;

    if (_wdupenv_s(&value, &length, wideName.c_str()) != 0 || value == nullptr)
    {
        return {};
    }

    std::filesystem::path result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::filesystem::path(value) : std::filesystem::path{};
#endif
}

std::filesystem::path FileSystem::ResolvePlatformRootDirectory() const
{
    {
        const std::lock_guard<std::mutex> lock(PlatformRootMutex());

        if (const auto& configured = PlatformRootOverride(); !configured.empty())
        {
            return configured;
        }
    }

#ifdef _WIN32

    if (const auto appData = ReadEnvironmentPath("APPDATA"); !appData.empty())
    {
        return appData / "Soundshed Guitar";
    }

#elif defined(__APPLE__)

    if (const auto home = ReadEnvironmentPath("HOME"); !home.empty())
    {
        return home / "Library" / "Soundshed Guitar";
    }

#else

    if (const auto home = ReadEnvironmentPath("HOME"); !home.empty())
    {
        return home / ".config" / "Soundshed Guitar";
    }

#endif

    return std::filesystem::path{"settings"};
}

std::filesystem::path FileSystem::ResolveDataDirectory() const
{
    return ResolvePlatformRootDirectory() / "data";
}

std::filesystem::path FileSystem::ResolveDataV1Directory() const
{
    return ResolveDataDirectory() / "v1";
}

std::filesystem::path FileSystem::ResolvePresetDirectory() const
{
    return ResolveSettingsDirectory() / "presets";
}

std::filesystem::path FileSystem::ResolveCacheDirectory() const
{
    return ResolveSettingsDirectory() / "cache";
}

std::filesystem::path FileSystem::ResolveSettingsDirectory() const
{
    return ResolveDataV1Directory();
}

std::filesystem::path FileSystem::ResolveSettingsFile() const
{
    return ResolveSettingsDirectory() / "settings" / "app.json";
}

std::optional<std::filesystem::path> FileSystem::EnsureDirectory(const std::filesystem::path& dir) const
{
    std::error_code ec;

    if (std::filesystem::create_directories(dir, ec) || std::filesystem::exists(dir))
    {
        return dir;
    }

    return std::nullopt;
}
} // namespace guitarfx
