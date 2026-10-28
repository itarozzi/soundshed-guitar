// The plugin formats are shared by every Plugin Host (JuceHostedPluginEffect::SharedPluginFormats).
// Setting them up costs 100-250 ms (JUCE's LV2 format loads every installed bundle and writes its
// spec bundles to a temp folder), and while each node owned a set, every Plugin Host paid it on
// every preset load. These pin the sharing: set up on the first load, not at construction; one set
// for every live Plugin Host, set up once; and released with the last Plugin Host.
//
// Loads a plugin path that does not exist, which sets the formats up and then fails cleanly, so
// this runs on any machine.
//
// Also the description cache the formats carry (HostedPluginDescriptionCache), against fake
// plugin files in a sandboxed settings folder: what a VST3 scan found is found again, after a
// restart too, until the plugin's files change.

#include "JuceHostedPluginEffect.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
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
        Fail("a second load set the formats up again: " + std::to_string(formatCount) + " formats became " +
             std::to_string(second.GetPluginFormatsForTesting().getNumFormats()));
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

// ── The description cache ───────────────────────────────────────────────────────────────

/// A sandbox for the cache file, which lives under the settings directory.
std::filesystem::path UseCacheSandbox()
{
    const auto root = std::filesystem::temp_directory_path() / "guitarfx-hosted-plugin-tests" / "description-cache";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
#ifdef _WIN32
    _wputenv_s(L"APPDATA", root.wstring().c_str());
#else
    setenv("HOME", root.string().c_str(), 1);
#endif
    return root;
}

void WriteFile(const std::filesystem::path& file, const std::string& contents)
{
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary | std::ios::trunc) << contents;
}

juce::OwnedArray<juce::PluginDescription> FakeScan(const std::filesystem::path& plugin)
{
    juce::OwnedArray<juce::PluginDescription> found;
    auto* description = found.add(new juce::PluginDescription());
    description->name = "Fake";
    description->pluginFormatName = "VST3";
    description->fileOrIdentifier = juce::String(plugin.wstring().c_str());
    description->uniqueId = 1234;
    return found;
}

/// A scanned plugin is found again, by this process and by the next one (a fresh cache reads
/// the file), until the plugin changes; then it is scanned again.
bool TestDescriptionCacheRemembersUntilThePluginChanges()
{
    const auto root = UseCacheSandbox();
    const auto plugin = root / "plugins" / "Fake.vst3";
    WriteFile(plugin, "binary-v1");

    guitarfx::HostedPluginDescriptionCache cache;

    if (!cache.Find(plugin).empty())
    {
        Fail("an empty cache found a plugin");
        return false;
    }

    cache.Store(plugin, FakeScan(plugin));
    const auto found = cache.Find(plugin);

    if (found.size() != 1 || found.front().name != "Fake" || found.front().uniqueId != 1234)
    {
        Fail("a stored description was not found again");
        return false;
    }

    guitarfx::HostedPluginDescriptionCache restarted;

    if (restarted.Find(plugin).size() != 1)
    {
        Fail("a stored description did not survive a restart");
        return false;
    }

    WriteFile(plugin, "binary-v2, updated in place");

    if (!restarted.Find(plugin).empty())
    {
        Fail("a plugin that changed was still taken from the cache");
        return false;
    }

    cache.Store(plugin, FakeScan(plugin));
    cache.Forget(plugin);

    if (!cache.Find(plugin).empty() || !guitarfx::HostedPluginDescriptionCache().Find(plugin).empty())
    {
        Fail("a forgotten description was still found");
        return false;
    }

    return true;
}

/// A bundle is told apart by the binaries inside it, which an update replaces while the bundle
/// folder itself stays put.
bool TestDescriptionCacheWatchesABundlesBinaries()
{
    const auto root = UseCacheSandbox();
    const auto bundle = root / "plugins" / "Bundle.vst3";
    const auto binary = bundle / "Contents" / "x86_64-win" / "Bundle.vst3";
    WriteFile(binary, "binary-v1");
    WriteFile(bundle / "Contents" / "Resources" / "Snapshots" / "image.png", "not part of the fingerprint");

    guitarfx::HostedPluginDescriptionCache cache;
    cache.Store(bundle, FakeScan(bundle));

    if (cache.Find(bundle).size() != 1)
    {
        Fail("a stored bundle was not found again");
        return false;
    }

    WriteFile(binary, "binary-v2, a longer build");

    if (!cache.Find(bundle).empty())
    {
        Fail("a bundle whose binary changed was still taken from the cache");
        return false;
    }

    // A folder with no binary in it is no plugin to remember.
    const auto empty = root / "plugins" / "Empty.vst3";
    std::filesystem::create_directories(empty / "Contents");
    cache.Store(empty, FakeScan(empty));

    if (!cache.Find(empty).empty())
    {
        Fail("a bundle with no binaries was cached");
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

        if (ok)
        {
            ++passed;
        }
        else
        {
            ++failed;
        }
    };

    run("Formats are not set up at construction", TestFormatsAreNotSetUpAtConstruction());
    run("Live Plugin Hosts share one set of formats", TestLivePluginHostsShareOneSetOfFormats());
    run("Formats are released with the last Plugin Host", TestFormatsAreReleasedWithTheLastPluginHost());
    run("Description cache remembers until the plugin changes", TestDescriptionCacheRemembersUntilThePluginChanges());
    run("Description cache watches a bundle's binaries", TestDescriptionCacheWatchesABundlesBinaries());

    std::cout << "\nHosted plugin format sharing tests: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
