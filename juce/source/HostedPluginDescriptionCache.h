#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "util/FileSystem.h"
#include "util/PathEncoding.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace guitarfx
{
    /**
     * What scanning each VST3 file found, kept so a load can go straight to creating the
     * instance.
     *
     * A VST3 without a moduleinfo.json (most of them) can only be described by loading its
     * binary and creating and initialising a component of every class in it, which JUCE then
     * throws away and unloads before the real instance is made. Measured on 2026-10-23 that
     * scan was 80-95% of a first load for the heavy amp sims (Guitar Rig 7: 7.8 s of 8.4 s;
     * BIAS FX 2: 7.2 s of 7.6 s), and still up to ~1.9 s when the binary was already loaded.
     *
     * Entries are keyed by the plugin path and checked against a fingerprint of the files
     * that make it up (names, sizes, modification times), so updating a plugin in place
     * rescans it. Kept in memory and in the cache folder, so a restart keeps them. Not
     * thread-safe: JuceHostedPluginEffect uses it under its shared formats lock.
     */
    class HostedPluginDescriptionCache
    {
    public:
        /// The descriptions a scan of `plugin` found last time, or none if it has not been
        /// scanned, or has changed since.
        [[nodiscard]] std::vector<juce::PluginDescription> Find (const std::filesystem::path& plugin)
        {
            LoadIfNeeded();
            const auto fingerprint = Fingerprint (plugin);
            if (fingerprint.empty())
                return {};

            const auto* entry = FindEntry (plugin);
            if (entry == nullptr || entry->getStringAttribute ("fingerprint").toStdString() != fingerprint)
                return {};

            std::vector<juce::PluginDescription> descriptions;
            for (const auto* child : entry->getChildIterator())
            {
                juce::PluginDescription description;
                if (description.loadFromXml (*child))
                    descriptions.push_back (description);
            }
            return descriptions;
        }

        /// Records what a scan of `plugin` found, and writes the cache out.
        void Store (const std::filesystem::path& plugin, const juce::OwnedArray<juce::PluginDescription>& found)
        {
            LoadIfNeeded();
            const auto fingerprint = Fingerprint (plugin);
            if (fingerprint.empty() || found.isEmpty())
                return;

            RemoveEntry (plugin);
            auto* entry = mEntries.createNewChildElement ("ENTRY");
            entry->setAttribute ("path", juce::String::fromUTF8 (util::PathToUtf8 (plugin).c_str()));
            entry->setAttribute ("fingerprint", juce::String (fingerprint));
            for (const auto* description : found)
            {
                if (description != nullptr)
                    entry->addChildElement (description->createXml().release());
            }
            Save();
        }

        /// Drops what is known about `plugin`: a cached description it could not be created from.
        void Forget (const std::filesystem::path& plugin)
        {
            LoadIfNeeded();
            if (RemoveEntry (plugin))
                Save();
        }

    private:
        static std::filesystem::path CacheFile()
        {
            return FileSystem().ResolveCacheDirectory() / "hosted-plugin-descriptions.xml";
        }

        // The plugin's own files: a single-file plugin itself, or, for a bundle, the binaries
        // in its platform folders and its module info. Empty when none can be read, which
        // leaves the plugin uncached rather than trusting a stale entry.
        static std::string Fingerprint (const std::filesystem::path& plugin)
        {
            std::error_code ec;
            std::vector<std::filesystem::path> files;

            if (std::filesystem::is_regular_file (plugin, ec))
            {
                files.push_back (plugin);
            }
            else if (std::filesystem::is_directory (plugin, ec))
            {
                const auto contents = plugin / "Contents";
                for (const auto* name : { "moduleinfo.json", "Info.plist" })
                    files.push_back (contents / name);
                files.push_back (contents / "Resources" / "moduleinfo.json");

                for (std::filesystem::directory_iterator it (contents, ec), end; !ec && it != end; it.increment (ec))
                {
                    const auto folder = it->path().filename().string();
                    const bool binaries = folder == "MacOS" || folder.ends_with ("-win") || folder.ends_with ("-linux");
                    if (!binaries || !it->is_directory (ec))
                        continue;

                    std::error_code inner;
                    for (std::filesystem::directory_iterator file (it->path(), inner), fileEnd; !inner && file != fileEnd;
                         file.increment (inner))
                        files.push_back (file->path());
                }
            }

            std::sort (files.begin(), files.end());
            std::string fingerprint;
            bool anyBinary = false;
            for (const auto& file : files)
            {
                std::error_code fileEc;
                if (!std::filesystem::is_regular_file (file, fileEc))
                    continue;

                const auto size = std::filesystem::file_size (file, fileEc);
                const auto written = std::filesystem::last_write_time (file, fileEc);
                if (fileEc)
                    return {};

                const auto ticks = std::chrono::duration_cast<std::chrono::milliseconds> (written.time_since_epoch()).count();
                fingerprint += util::PathToUtf8 (file.filename()) + ":" + std::to_string (size) + ":" + std::to_string (ticks) + ";";
                anyBinary = anyBinary || file.extension() != ".json";
            }

            return anyBinary ? fingerprint : std::string {};
        }

        juce::XmlElement* FindEntry (const std::filesystem::path& plugin)
        {
            const auto key = juce::String::fromUTF8 (util::PathToUtf8 (plugin).c_str());
            for (auto* entry : mEntries.getChildWithTagNameIterator ("ENTRY"))
            {
                if (entry->getStringAttribute ("path") == key)
                    return entry;
            }
            return nullptr;
        }

        bool RemoveEntry (const std::filesystem::path& plugin)
        {
            if (auto* entry = FindEntry (plugin))
            {
                mEntries.removeChildElement (entry, true);
                return true;
            }
            return false;
        }

        void LoadIfNeeded()
        {
            if (mLoaded)
                return;

            mLoaded = true;
            const juce::File file (CacheFile().wstring().c_str());
            if (auto xml = juce::parseXMLIfTagMatches (file, "HOSTEDPLUGINDESCRIPTIONS"))
                mEntries = *xml;
        }

        // Replaced whole through a temporary, so a second process reading it never sees half a file.
        void Save() const
        {
            const juce::File file (CacheFile().wstring().c_str());
            if (!file.getParentDirectory().createDirectory())
                return;

            const auto temporary = file.getSiblingFile (file.getFileName() + ".tmp");
            if (mEntries.writeTo (temporary))
                temporary.moveFileTo (file);
        }

        juce::XmlElement mEntries { "HOSTEDPLUGINDESCRIPTIONS" };
        bool mLoaded = false;
    };
} // namespace guitarfx
