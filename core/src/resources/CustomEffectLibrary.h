#pragma once

#include "storage/JsonStore.h"
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace guitarfx
{
struct CustomEffectLibraryEntry
{
    std::string id;
    std::string name;
    std::string category;
    std::string description;
    std::string baseEffectType;
    std::string moduleResourceType;
    std::string moduleResourceId;
    std::string latestRevisionId;
    std::string thumbnailDataUrl;
    std::vector<std::string> tags;
    std::map<std::string, double> defaultParams;
    nlohmann::json descriptorSummary = nlohmann::json::object();
    std::string origin;
    std::string createdAt;
    std::string updatedAt;

    [[nodiscard]] bool IsValid() const;
};

// A Custom Effect's category and thumbnail come from its module (the guest descriptor), which is
// code from whoever wrote it, and both end up in UI markup. A category is a short plain word or it
// is "utility"; a thumbnail is an inline raster image or nothing, so it can never make the app
// fetch a URL.
[[nodiscard]] std::string SanitizeCustomEffectCategory(const std::string& category);
[[nodiscard]] std::string SanitizeCustomEffectThumbnail(const std::string& dataUrl);

[[nodiscard]] nlohmann::json SerializeCustomEffectLibraryEntry(const CustomEffectLibraryEntry& entry);
[[nodiscard]] std::optional<CustomEffectLibraryEntry> DeserializeCustomEffectLibraryEntry(const nlohmann::json& json,
                                                                                          std::string* error = nullptr);

class CustomEffectLibrary
{
  public:
    void Clear();
    void UpsertEntry(const CustomEffectLibraryEntry& entry);
    [[nodiscard]] bool RemoveEntry(const std::string& id);
    [[nodiscard]] const CustomEffectLibraryEntry* GetEntry(const std::string& id) const;

    [[nodiscard]] const std::vector<CustomEffectLibraryEntry>& GetAllEntries() const
    {
        return mEntries;
    }

    // Store-backed persistence: one row per entry, keyed by entry id.
    void LoadFromStore(storage::JsonStore& store);
    bool SaveToStore(storage::JsonStore& store) const;

    // Legacy file persistence, retained for import/export and the migration.
    void LoadFromFile(const std::filesystem::path& path);
    void SaveToFile(const std::filesystem::path& path) const;

  private:
    std::vector<CustomEffectLibraryEntry> mEntries;
};
} // namespace guitarfx