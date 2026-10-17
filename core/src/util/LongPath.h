#pragma once

#include <filesystem>
#include <string>
#include <system_error>

namespace guitarfx::util
{
/// The form of `path` to hand the operating system when opening, testing or moving a file.
///
/// Windows refuses a path of MAX_PATH (260) characters or more, as though the file were not there,
/// unless the process opted into long paths or the path carries the `\\?\` extended-length prefix.
/// Opting in takes a longPathAware manifest, which a plug-in cannot declare for its host, and a
/// registry switch that is off by default. So a long absolute path comes back with the prefix
/// (`\\?\UNC\` for a share), its separators made backslashes and its `.` and `..` segments
/// resolved, since the prefix also turns that normalisation off. A shorter path, a relative one,
/// one already prefixed, and every path on other platforms come back unchanged.
///
/// Only for the call that touches the file: never store, compare or show the result.
[[nodiscard]] inline std::filesystem::path LongPathSafe(const std::filesystem::path& path)
{
#ifdef _WIN32
    // 248 rather than 260: CreateDirectory stops at MAX_PATH less 12, for an 8.3 name.
    constexpr std::size_t kLongestPlainPath = 247;
    const std::wstring& native = path.native();

    if (native.size() <= kLongestPlainPath || native.rfind(L"\\\\?\\", 0) == 0 || native.rfind(L"\\\\.\\", 0) == 0 ||
        native.rfind(L"//?/", 0) == 0 || !path.is_absolute())
    {
        return path;
    }

    const std::wstring normal = path.lexically_normal().native();

    if (normal.rfind(L"\\\\", 0) == 0)
    {
        return std::filesystem::path(L"\\\\?\\UNC\\" + normal.substr(2));
    }

    return std::filesystem::path(L"\\\\?\\" + normal);
#else
    return path;
#endif
}

/// std::filesystem::exists through LongPathSafe; false on any error.
[[nodiscard]] inline bool PathExists(const std::filesystem::path& path) noexcept
{
    try
    {
        std::error_code ec;
        return std::filesystem::exists(LongPathSafe(path), ec);
    }
    catch (...)
    {
        return false;
    }
}
} // namespace guitarfx::util
