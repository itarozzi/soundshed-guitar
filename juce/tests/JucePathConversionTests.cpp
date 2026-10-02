// The juce::File <-> std::filesystem::path crossing (JucePathConversion.h).
//
// Every file and folder the user picks crosses here, and so does the profile folder.
// The old crossing, std::filesystem::path (getFullPathName().toStdString()), read
// UTF-8 in the ANSI code page on Windows: a picked "Müzik" folder became "MÃ¼zik"
// and failed is_directory, and the resource browser stayed on "No folder selected".
// These names are created on disk and must be found again from either side.

#include "JucePathConversion.h"

#include <juce_core/juce_core.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

namespace
{
    constexpr auto kFailCode = 1;

    int Fail (const std::string& message)
    {
        std::cerr << "[JucePathConversionTests] " << message << std::endl;
        return kFailCode;
    }

    // Names in UTF-8, as JUCE hands them over: the reported folder, Turkish letters the
    // Western code page cannot spell, and a script no single-byte code page holds.
    const char* const kNames[] = {
        "M\xC3\xBCzik",
        "\xC5\x9F"
        "ark\xC4\xB1lar",
        "\xD7\xA4\xD7\x93\xD7\x9C \xE9\x9F\xB3\xE6\xA5\xBD",
    };
}

int main()
{
    const auto scratch = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("SoundshedJucePathConversionTests-" + juce::Uuid().toString());

    int result = 0;

    for (const auto* name : kNames)
    {
        const auto folder = scratch.getChildFile (juce::String::fromUTF8 (name));

        if (!folder.createDirectory())
        {
            result = Fail ("could not create the test folder '" + std::string (name) + "'");
            break;
        }

        // JUCE to std: the path names the folder JUCE made, and keeps its name.
        const auto stdPath = soundshed::toStdPath (folder);
        std::error_code ec;

        if (!std::filesystem::is_directory (stdPath, ec)
            || guitarfx::util::PathToUtf8 (stdPath.filename()) != name)
        {
            result = Fail ("toStdPath lost the folder '" + std::string (name) + "', got '"
                           + guitarfx::util::PathToUtf8 (stdPath) + "'");
            break;
        }

        // std to JUCE: a path built on the std side, as the core builds its profile paths.
        const auto child = stdPath / "child.txt";

        if (!soundshed::toJuceFile (child).replaceWithText ("x")
            || !std::filesystem::exists (child, ec)
            || soundshed::toJuceFile (stdPath) != folder)
        {
            result = Fail ("toJuceFile did not reach the folder '" + std::string (name) + "'");
            break;
        }
    }

    // An empty path stays empty: the editor hands an unresolved resource root through.
    if (result == 0 && soundshed::toJuceFile ({}) != juce::File())
        result = Fail ("an empty path should become an empty juce::File");

    scratch.deleteRecursively();

    if (result == 0)
        std::cout << "[JucePathConversionTests] all passed" << std::endl;

    return result;
}
