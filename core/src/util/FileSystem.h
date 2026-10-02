#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace guitarfx
{
class FileSystem
{
  public:
    /// Overrides the platform root directory for the whole process.
    ///
    /// Most platforms derive the root from the environment, but some have no
    /// environment to derive it from — on Android the sandbox path is only
    /// known to the Java layer, so the platform shim sets it once at startup,
    /// before any controller has been constructed. Passing an empty path
    /// restores the environment-derived default.
    static void SetPlatformRootOverride(const std::filesystem::path& root);

    /// An environment variable read as a path; empty when it is unset or empty.
    ///
    /// On Windows this reads the wide environment. The narrow one is in the ANSI code
    /// page, which cannot spell every user name, so a profile under such a name would
    /// resolve to a folder that does not exist.
    [[nodiscard]] static std::filesystem::path ReadEnvironmentPath(const char* name);

    [[nodiscard]] std::filesystem::path ResolvePlatformRootDirectory() const;
    [[nodiscard]] std::filesystem::path ResolveDataDirectory() const;
    [[nodiscard]] std::filesystem::path ResolveDataV1Directory() const;
    [[nodiscard]] std::filesystem::path ResolvePresetDirectory() const;
    [[nodiscard]] std::filesystem::path ResolveCacheDirectory() const;
    [[nodiscard]] std::filesystem::path ResolveSettingsDirectory() const;
    [[nodiscard]] std::filesystem::path ResolveSettingsFile() const;
    [[nodiscard]] std::optional<std::filesystem::path> EnsureDirectory(const std::filesystem::path& dir) const;
};
} // namespace guitarfx
