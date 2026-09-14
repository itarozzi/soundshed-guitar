// The WebView2 profile helpers (WebView2UserData.h): the stable folder they resolve,
// and the sweep of the per-launch profiles older builds left under %TEMP%.
//
// The sweep's contract is the interesting part: a profile another process still has
// open must survive, and everything else must go. A live profile is modelled by
// holding a file inside it open, which on Windows makes the rename the sweep relies
// on fail exactly as a running browser would.

#include "WebView2UserData.h"

#include <juce_core/juce_core.h>

#include <iostream>
#include <memory>
#include <string>

namespace
{
    constexpr auto kFailCode = 1;

    int Fail (const std::string& message)
    {
        std::cerr << "[WebView2UserDataTests] " << message << std::endl;
        return kFailCode;
    }

    // A private stand-in for %TEMP% so the test never touches the real legacy folders.
    juce::File makeScratchTempDir()
    {
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("SoundshedWebView2UserDataTests-" + juce::Uuid().toString());
        dir.createDirectory();
        return dir;
    }

    // The shape a per-launch profile has on disk: a nested folder with files in it.
    juce::File makeLegacyProfile (const juce::File& root, const juce::String& name)
    {
        const auto profile = root.getChildFile (name);
        const auto inner = profile.getChildFile ("EBWebView");
        inner.createDirectory();
        inner.getChildFile ("lockfile").replaceWithText ("held by the browser process");
        inner.getChildFile ("Local State").replaceWithText ("{}");
        return profile;
    }

    int TestResolvesOneStableFolderPerArgumentSet()
    {
        std::cout << "Test: ResolvesOneStableFolderPerArgumentSet... ";

        const juce::File localAppData ("C:\\Users\\someone\\AppData\\Local");

        const auto plain = guitarfx::webview2::resolveUserDataFolder (localAppData, {});
        const auto expected = localAppData.getChildFile ("Soundshed Guitar").getChildFile ("WebView2");

        if (plain != expected)
            return Fail ("default folder is " + plain.getFullPathName().toStdString());

        // Whitespace-only arguments are what an empty environment variable looks like.
        if (guitarfx::webview2::resolveUserDataFolder (localAppData, "  ") != expected)
            return Fail ("blank arguments should resolve to the default folder");

        const auto debug = guitarfx::webview2::resolveUserDataFolder (localAppData, "--remote-debugging-port=9333");
        const auto debugAgain = guitarfx::webview2::resolveUserDataFolder (localAppData, "--remote-debugging-port=9333");
        const auto otherPort = guitarfx::webview2::resolveUserDataFolder (localAppData, "--remote-debugging-port=9355");

        if (debug == plain)
            return Fail ("extra browser arguments must not share the default profile");

        if (debug != debugAgain)
            return Fail ("the same arguments must resolve to the same folder every time");

        if (debug == otherPort)
            return Fail ("different arguments must resolve to different folders");

        if (debug.getParentDirectory() != expected.getParentDirectory()
            || !debug.getFileName().startsWith ("WebView2-"))
            return Fail ("argument-keyed folder should be a WebView2- sibling, got " + debug.getFullPathName().toStdString());

        std::cout << "PASS" << std::endl;
        return 0;
    }

    int TestSweepRemovesIdleProfilesAndKeepsLiveOnes()
    {
        std::cout << "Test: SweepRemovesIdleProfilesAndKeepsLiveOnes... ";

        const auto tempDir = makeScratchTempDir();
        const auto root = tempDir.getChildFile (guitarfx::webview2::kLegacyProfileRoot);
        root.createDirectory();

        const auto idleA = makeLegacyProfile (root, "1784192733001");
        const auto idleB = makeLegacyProfile (root, "1789349473782");
        const auto live = makeLegacyProfile (root, "1789352698253");
        const auto probe = makeLegacyProfile (tempDir, guitarfx::webview2::kLegacyProbeFolder);

        // A running browser keeps files inside its profile open.
        auto heldOpen = std::make_unique<juce::FileOutputStream> (live.getChildFile ("EBWebView").getChildFile ("lockfile"));

        if (!heldOpen->openedOk())
            return Fail ("could not hold the live profile's lock file open");

        const auto first = guitarfx::webview2::sweepLegacyProfileFolders (tempDir);

        if (idleA.exists() || idleB.exists())
            return Fail ("idle profiles should be gone after the first sweep");

        if (probe.exists())
            return Fail ("the probe profile should be gone after the first sweep");

#if JUCE_WINDOWS
        if (!live.isDirectory() || !live.getChildFile ("EBWebView").getChildFile ("Local State").existsAsFile())
            return Fail ("a profile with an open file must be left intact");

        if (!root.isDirectory())
            return Fail ("the legacy root must stay while a profile in it is still in use");

        if (first.removed != 3 || first.skipped != 1)
            return Fail ("first sweep counted removed=" + std::to_string (first.removed)
                         + " skipped=" + std::to_string (first.skipped) + ", expected 3 and 1");
#else
        // Elsewhere an open file does not block a rename, so the live profile goes too;
        // the Windows-only behaviour is what the sweep is for, and this branch just
        // keeps the test honest if the target is ever built on another platform.
        if (first.removed != 4 || first.skipped != 0)
            return Fail ("first sweep counted removed=" + std::to_string (first.removed)
                         + " skipped=" + std::to_string (first.skipped) + ", expected 4 and 0");
#endif

        // The browser process exits; the next pass picks the profile up.
        heldOpen.reset();
        const auto second = guitarfx::webview2::sweepLegacyProfileFolders (tempDir);

        if (live.exists())
            return Fail ("the released profile should be gone after the second sweep");

        if (root.exists())
            return Fail ("the legacy root should be removed once it is empty");

        if (tempDir.getNumberOfChildFiles (juce::File::findFilesAndDirectories) != 0)
            return Fail ("the sweep left something behind in the temp dir");

#if JUCE_WINDOWS
        if (second.removed != 1 || second.skipped != 0)
            return Fail ("second sweep counted removed=" + std::to_string (second.removed)
                         + " skipped=" + std::to_string (second.skipped) + ", expected 1 and 0");
#else
        if (second.removed != 0 || second.skipped != 0)
            return Fail ("second sweep should have found nothing to do");
#endif

        tempDir.deleteRecursively();
        std::cout << "PASS" << std::endl;
        return 0;
    }

    int TestSweepResumesAnInterruptedPass()
    {
        std::cout << "Test: SweepResumesAnInterruptedPass... ";

        const auto tempDir = makeScratchTempDir();
        const auto root = tempDir.getChildFile (guitarfx::webview2::kLegacyProfileRoot);
        root.createDirectory();

        // A pass that renamed a profile and was killed before deleting it leaves the
        // ".sweep" twin behind.
        const auto parked = makeLegacyProfile (root, "1784192733001.sweep");

        const auto result = guitarfx::webview2::sweepLegacyProfileFolders (tempDir);

        if (parked.exists() || root.exists())
            return Fail ("a parked profile from an interrupted pass should be deleted");

        if (result.removed != 1 || result.skipped != 0)
            return Fail ("counted removed=" + std::to_string (result.removed)
                         + " skipped=" + std::to_string (result.skipped) + ", expected 1 and 0");

        tempDir.deleteRecursively();
        std::cout << "PASS" << std::endl;
        return 0;
    }

    int TestSweepIsANoOpWithoutLegacyFolders()
    {
        std::cout << "Test: SweepIsANoOpWithoutLegacyFolders... ";

        const auto tempDir = makeScratchTempDir();
        const auto unrelated = tempDir.getChildFile ("something-else");
        unrelated.createDirectory();

        const auto result = guitarfx::webview2::sweepLegacyProfileFolders (tempDir);

        if (result.removed != 0 || result.skipped != 0)
            return Fail ("nothing to sweep should count nothing");

        if (!unrelated.isDirectory())
            return Fail ("the sweep must not touch unrelated folders");

        tempDir.deleteRecursively();
        std::cout << "PASS" << std::endl;
        return 0;
    }
}

int main()
{
    int failures = 0;
    failures += TestResolvesOneStableFolderPerArgumentSet();
    failures += TestSweepRemovesIdleProfilesAndKeepsLiveOnes();
    failures += TestSweepResumesAnInterruptedPass();
    failures += TestSweepIsANoOpWithoutLegacyFolders();

    if (failures != 0)
    {
        std::cerr << "[WebView2UserDataTests] " << failures << " test(s) failed" << std::endl;
        return kFailCode;
    }

    std::cout << "[WebView2UserDataTests] all tests passed" << std::endl;
    return 0;
}
