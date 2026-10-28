#include "PluginController.h"

#include "controller/internal/ControllerUtils.h"

#include <algorithm>
#include <string>

// Preset folder edits, asked for by name (docs/plans/native-ui.md, rule C3): every UI used to
// edit its own copy of the folder tree and send it back whole (setPresetFolders), so two UIs
// could write it differently. The tree is preset-folders.json, or an archive session's own:
// {folders: [{id, name, children: [...], presetIds: [...]}], activeFolderId}. Edits work on the
// stored document, so the factory-archive folders a broadcast leaves out are kept.

namespace guitarfx
{
namespace
{
constexpr const char* kAllFolderId = "__all__";

bool IsVirtualFolderId(const std::string& id)
{
    return id.empty() || id == kAllFolderId || id == "__favorites__" || id == "__recents__";
}

nlohmann::json* FindFolder(nlohmann::json& list, const std::string& id)
{
    if (!list.is_array())
    {
        return nullptr;
    }

    for (auto& folder : list)
    {
        if (!folder.is_object())
        {
            continue;
        }

        if (folder.value("id", std::string()) == id)
        {
            return &folder;
        }

        if (auto children = folder.find("children"); children != folder.end())
        {
            if (auto* found = FindFolder(*children, id))
            {
                return found;
            }
        }
    }

    return nullptr;
}

/// The list that holds the folder `id`: the top level, or a folder's children.
nlohmann::json* FindParentList(nlohmann::json& list, const std::string& id)
{
    if (!list.is_array())
    {
        return nullptr;
    }

    for (auto& folder : list)
    {
        if (!folder.is_object())
        {
            continue;
        }

        if (folder.value("id", std::string()) == id)
        {
            return &list;
        }

        if (auto children = folder.find("children"); children != folder.end())
        {
            if (auto* found = FindParentList(*children, id))
            {
                return found;
            }
        }
    }

    return nullptr;
}

/// Takes the folder `id` out of the tree and returns it (null JSON if absent).
nlohmann::json TakeFolder(nlohmann::json& list, const std::string& id)
{
    auto* parent = FindParentList(list, id);

    if (parent == nullptr)
    {
        return nullptr;
    }

    for (auto it = parent->begin(); it != parent->end(); ++it)
    {
        if (it->is_object() && it->value("id", std::string()) == id)
        {
            auto folder = std::move(*it);
            parent->erase(it);
            return folder;
        }
    }

    return nullptr;
}

void RemovePresetEverywhere(nlohmann::json& list, const std::string& presetId)
{
    if (!list.is_array())
    {
        return;
    }

    for (auto& folder : list)
    {
        if (!folder.is_object())
        {
            continue;
        }

        if (auto ids = folder.find("presetIds"); ids != folder.end() && ids->is_array())
        {
            ids->erase(std::remove(ids->begin(), ids->end(), nlohmann::json(presetId)), ids->end());
        }

        if (auto children = folder.find("children"); children != folder.end())
        {
            RemovePresetEverywhere(*children, presetId);
        }
    }
}

std::string Trimmed(std::string text)
{
    const auto first = text.find_first_not_of(" \t\r\n");

    if (first == std::string::npos)
    {
        return {};
    }

    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}
} // namespace

nlohmann::json PluginController::LoadPresetFoldersDocument()
{
    auto document = mPresetArchiveSession ? mPresetArchiveSession->folders
                                          : LoadUiStorageJson("preset-folders.json", nlohmann::json::object());

    if (!document.is_object())
    {
        document = nlohmann::json::object();
    }

    if (!document.contains("folders") || !document["folders"].is_array())
    {
        document["folders"] = nlohmann::json::array();
    }

    if (!document.contains("activeFolderId") || !document["activeFolderId"].is_string())
    {
        document["activeFolderId"] = kAllFolderId;
    }

    return document;
}

void PluginController::SavePresetFoldersDocument(const nlohmann::json& document)
{
    // A session's folders are its own, and go with it.
    if (mPresetArchiveSession)
    {
        mPresetArchiveSession->folders = document;
    }
    else
    {
        SaveUiStorageJson("preset-folders.json", document);
    }

    HandleGetPresetFoldersRequest();
}

void PluginController::HandleCreatePresetFolderRequest(const nlohmann::json& payload)
{
    const auto name = Trimmed(payload.value("name", std::string()));

    if (name.empty())
    {
        return;
    }

    auto document = LoadPresetFoldersDocument();
    nlohmann::json folder = {{"id", controller_detail::GenerateGuidV4String()},
                             {"name", name},
                             {"children", nlohmann::json::array()},
                             {"presetIds", nlohmann::json::array()}};
    const auto id = folder["id"].get<std::string>();

    const auto parentId = payload.value("parentId", std::string());
    auto* parent = IsVirtualFolderId(parentId) ? nullptr : FindFolder(document["folders"], parentId);

    if (parent != nullptr)
    {
        if (!(*parent)["children"].is_array())
        {
            (*parent)["children"] = nlohmann::json::array();
        }

        (*parent)["children"].push_back(std::move(folder));
    }
    else
    {
        document["folders"].push_back(std::move(folder));
    }

    // The new folder is the one shown, as the web UI's createFolder does.
    document["activeFolderId"] = id;
    SavePresetFoldersDocument(document);
}

void PluginController::HandleRenamePresetFolderRequest(const nlohmann::json& payload)
{
    const auto name = Trimmed(payload.value("name", std::string()));

    if (name.empty())
    {
        return;
    }

    auto document = LoadPresetFoldersDocument();
    auto* folder = FindFolder(document["folders"], payload.value("folderId", std::string()));

    if (folder == nullptr || folder->value("name", std::string()) == name)
    {
        return;
    }

    (*folder)["name"] = name;
    SavePresetFoldersDocument(document);
}

void PluginController::HandleMovePresetFolderRequest(const nlohmann::json& payload)
{
    const auto folderId = payload.value("folderId", std::string());
    const auto parentId = payload.value("parentId", std::string());

    if (IsVirtualFolderId(folderId) || parentId == folderId || parentId == "__favorites__" || parentId == "__recents__")
    {
        return;
    }

    auto document = LoadPresetFoldersDocument();
    auto& folders = document["folders"];
    auto* moving = FindFolder(folders, folderId);

    if (moving == nullptr)
    {
        return;
    }

    // Not into itself or anything under it.
    if (auto children = moving->find("children");
        !IsVirtualFolderId(parentId) && children != moving->end() && FindFolder(*children, parentId) != nullptr)
    {
        return;
    }

    if (!IsVirtualFolderId(parentId) && FindFolder(folders, parentId) == nullptr)
    {
        return;
    }

    auto folder = TakeFolder(folders, folderId);

    if (folder.is_null())
    {
        return;
    }

    if (auto* parent = IsVirtualFolderId(parentId) ? nullptr : FindFolder(folders, parentId))
    {
        if (!(*parent)["children"].is_array())
        {
            (*parent)["children"] = nlohmann::json::array();
        }

        (*parent)["children"].push_back(std::move(folder));
    }
    else
    {
        folders.push_back(std::move(folder));
    }

    SavePresetFoldersDocument(document);
}

void PluginController::HandleDeletePresetFolderRequest(const nlohmann::json& payload)
{
    const auto folderId = payload.value("folderId", std::string());

    if (IsVirtualFolderId(folderId))
    {
        return;
    }

    auto document = LoadPresetFoldersDocument();

    if (TakeFolder(document["folders"], folderId).is_null())
    {
        return;
    }

    // Its presets stay in the library (All), and the list goes back to All, as in the web UI.
    document["activeFolderId"] = kAllFolderId;
    SavePresetFoldersDocument(document);
}

void PluginController::HandleMovePresetToFolderRequest(const nlohmann::json& payload)
{
    const auto presetId = payload.value("presetId", std::string());
    const auto folderId = payload.value("folderId", std::string());

    if (presetId.empty() || folderId == "__favorites__" || folderId == "__recents__")
    {
        return;
    }

    auto document = LoadPresetFoldersDocument();
    auto& folders = document["folders"];
    // A preset is in one folder at most: it leaves the others, and All takes it out of all.
    auto* target = IsVirtualFolderId(folderId) ? nullptr : FindFolder(folders, folderId);

    if (!IsVirtualFolderId(folderId) && target == nullptr)
    {
        return;
    }

    RemovePresetEverywhere(folders, presetId);

    if (target != nullptr)
    {
        if (!(*target)["presetIds"].is_array())
        {
            (*target)["presetIds"] = nlohmann::json::array();
        }

        (*target)["presetIds"].push_back(presetId);
    }

    SavePresetFoldersDocument(document);
}

void PluginController::HandleSelectPresetFolderRequest(const nlohmann::json& payload)
{
    const auto folderId = payload.value("folderId", std::string(kAllFolderId));
    auto document = LoadPresetFoldersDocument();

    if (!IsVirtualFolderId(folderId) && FindFolder(document["folders"], folderId) == nullptr)
    {
        return;
    }

    if (document["activeFolderId"] == folderId)
    {
        return;
    }

    document["activeFolderId"] = folderId.empty() ? std::string(kAllFolderId) : folderId;
    SavePresetFoldersDocument(document);
}

void PluginController::RemovePresetFromFolders(const std::string& presetId)
{
    auto document = LoadPresetFoldersDocument();
    const auto before = document.dump();
    RemovePresetEverywhere(document["folders"], presetId);

    if (document.dump() != before)
    {
        SavePresetFoldersDocument(document);
    }
}
} // namespace guitarfx
