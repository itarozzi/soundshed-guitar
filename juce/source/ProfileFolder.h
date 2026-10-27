#pragma once

#include <juce_core/juce_core.h>

namespace soundshed
{
/// The one profile every Soundshed Guitar product shares: settings, soundshed.db, the
/// resource library, riffs and logs. Deliberately not derived from the product name, so
/// Soundshed Guitar and Soundshed Nano Q (through core's FileSystem) read and write the same
/// data.
inline constexpr const char* kProfileFolderName = "Soundshed Guitar";

inline juce::File profileFolder()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile (kProfileFolderName);
}
} // namespace soundshed
