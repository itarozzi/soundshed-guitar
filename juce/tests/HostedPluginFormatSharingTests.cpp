// The plugin formats are shared by every Plugin Host (JuceHostedPluginEffect::SharedPluginFormats).
// Setting them up costs 100-250 ms (JUCE's LV2 format loads every installed bundle and writes its
// spec bundles to a temp folder), and while each node owned a set, every Plugin Host paid it on
// every preset load. These pin the sharing: set up on the first load, not at construction; one set
// for every live Plugin Host, set up once; and released with the last Plugin Host.
//
// Loads a plugin path that does not exist, which sets the formats up and then fails cleanly, so
// this runs on any machine.

#include "JuceHostedPluginEffect.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

namespace
{
int Fail(const std::string& message)
{
    std::cerr << "[HostedPluginFormatSharingTests] " << message << std::endl;
    return 1;
}

std::filesystem::path MissingPluginPath()
{
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("soundshed-format-sharing-test-missing.vst3");
    return std::filesystem::path(file.getFullPathName().toWideCharPointer());
}

/// Constructing a Plugin Host must not set the formats up: nodes are created for every preset
/// load, including ones whose plugin is never loaded.
bool TestFormatsAreNotSetUpAtConstruction()
{
    guitarfx::JuceHostedPluginEffect effect;

    if (effect.GetPluginFormatsForTesting().getNumFormats() != 0)
    {
        Fail("a new Plugin Host already had formats set up");
        return false;
    }

    return true;
}

/// Two live Plugin Hosts use one set of formats, set up by the first load and not again.
bool TestLivePluginHostsShareOneSetOfFormats()
{
    auto first = std::make_unique<guitarfx::JuceHostedPluginEffect>();

    if (first->LoadResource(MissingPluginPath()))
    {
        Fail("a plugin path that does not exist loaded");
        return false;
    }

    const int formatCount = first->GetPluginFormatsForTesting().getNumFormats();

    if (formatCount == 0)
    {
        Fail("a load did not set the formats up");
        return false;
    }

    guitarfx::JuceHostedPluginEffect second;

    if (&second.GetPluginFormatsForTesting() != &first->GetPluginFormatsForTesting())
    {
        Fail("a second Plugin Host got formats of its own");
        return false;
    }

    second.LoadResource(MissingPluginPath());

    if (second.GetPluginFormatsForTesting().getNumFormats() != formatCount)
    {
        Fail("a second load set the formats up again: " + std::to_string(formatCount) + " formats became "
             + std::to_string(second.GetPluginFormatsForTesting().getNumFormats()));
        return false;
    }

    // The second outliving the first keeps the set.
    first.reset();

    if (second.GetPluginFormatsForTesting().getNumFormats() != formatCount)
    {
        Fail("the formats went away while a Plugin Host still used them");
        return false;
    }

    return true;
}

/// The set goes with the last Plugin Host, so the next one starts from nothing again.
bool TestFormatsAreReleasedWithTheLastPluginHost()
{
    {
        guitarfx::JuceHostedPluginEffect effect;
        effect.LoadResource(MissingPluginPath());

        if (effect.GetPluginFormatsForTesting().getNumFormats() == 0)
        {
            Fail("a load did not set the formats up");
            return false;
        }
    }

    guitarfx::JuceHostedPluginEffect next;

    if (next.GetPluginFormatsForTesting().getNumFormats() != 0)
    {
        Fail("the formats outlived the last Plugin Host");
        return false;
    }

    return true;
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    int passed = 0;
    int failed = 0;

    const auto run = [&](const std::string& name, bool ok) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << "\n";
        if (ok) ++passed; else ++failed;
    };

    run("Formats are not set up at construction", TestFormatsAreNotSetUpAtConstruction());
    run("Live Plugin Hosts share one set of formats", TestLivePluginHostsShareOneSetOfFormats());
    run("Formats are released with the last Plugin Host", TestFormatsAreReleasedWithTheLastPluginHost());

    std::cout << "\nHosted plugin format sharing tests: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
