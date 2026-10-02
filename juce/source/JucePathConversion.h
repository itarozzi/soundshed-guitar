#pragma once

#include <juce_core/juce_core.h>

#include "util/PathEncoding.h"

#include <filesystem>

// Where juce::File and std::filesystem::path meet. Both sides hold the path as
// Unicode, so it crosses in UTF-8.
//
// Never cross with getFullPathName().toStdString() into a path, or path::string()
// into a juce::String: on Windows the narrow side of std::filesystem::path is the
// ANSI code page, not UTF-8. A folder called "Müzik" then resolves to "MÃ¼zik",
// which does not exist, and a name the code page cannot spell at all throws.
namespace soundshed
{
    inline std::filesystem::path toStdPath (const juce::File& file)
    {
        return guitarfx::util::PathFromUtf8 (file.getFullPathName().toStdString());
    }

    /** `path` must be absolute, as juce::File requires. */
    inline juce::File toJuceFile (const std::filesystem::path& path)
    {
        return juce::File (juce::String (guitarfx::util::PathToUtf8 (path)));
    }

    /** For logs and messages only: the path as text, never as something to open. */
    inline juce::String toJuceString (const std::filesystem::path& path)
    {
        return juce::String (guitarfx::util::PathToUtf8 (path));
    }
} // namespace soundshed
