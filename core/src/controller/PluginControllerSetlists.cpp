#include "PluginController.h"

#include "controller/internal/ControllerUtils.h"

#include <algorithm>
#include <string>

// Setlist edits, asked for by name (docs/plans/native-ui.md, rule C3), with the rules of the web
// UI's presets/setlists.ts: a bank number belongs to one setlist at most; a new setlist is the
// active one; deleting one makes the next (else the previous) active; moving or removing a slot
// keeps the cursor on the slot it was on. setlists.json is {setlists: [{id, name, bank, slots:
// [{presetId}]}], activeSetlistId, cursorIndex, bankSize}; each edit is answered by "setlists"
// and "setlistCursorChanged". A slot command names its setlist, or works on the active one.

namespace guitarfx
{
namespace
{
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

nlohmann::json* FindSetlist(nlohmann::json& document, const std::string& id)
{
    for (auto& setlist : document["setlists"])
    {
        if (setlist.is_object() && setlist.value("id", std::string()) == id)
        {
            return &setlist;
        }
    }

    return nullptr;
}

/// The bank as stored: a non-negative whole number, or null for none.
nlohmann::json BankValue(const nlohmann::json& payload)
{
    const auto it = payload.find("bank");

    if (it == payload.end() || !it->is_number())
    {
        return nullptr;
    }

    const auto bank = it->get<double>();

    if (bank < 0.0)
    {
        return nullptr;
    }

    return static_cast<int>(bank);
}

bool BankTaken(const nlohmann::json& document, const nlohmann::json& bank, const std::string& exceptId)
{
    if (bank.is_null())
    {
        return false;
    }

    for (const auto& setlist : document["setlists"])
    {
        if (setlist.is_object() && setlist.value("id", std::string()) != exceptId && setlist.contains("bank") &&
            setlist["bank"] == bank)
        {
            return true;
        }
    }

    return false;
}
} // namespace

nlohmann::json PluginController::LoadSetlistsDocument()
{
    auto document = LoadUiStorageJson("setlists.json", nlohmann::json::object());

    if (!document.is_object())
    {
        document = nlohmann::json::object();
    }

    if (!document.contains("setlists") || !document["setlists"].is_array())
    {
        document["setlists"] = nlohmann::json::array();
    }

    if (!document.contains("activeSetlistId") || !document["activeSetlistId"].is_string())
    {
        document["activeSetlistId"] = "";
    }

    if (!document.contains("cursorIndex") || !document["cursorIndex"].is_number_integer())
    {
        document["cursorIndex"] = 0;
    }

    return document;
}

void PluginController::SaveSetlistsDocument(const nlohmann::json& document)
{
    mSetlistCursorIndex = document.value("cursorIndex", 0);
    SaveUiStorageJson("setlists.json", document);
    HandleGetSetlistsRequest();

    nlohmann::json cursor;
    cursor["type"] = "setlistCursorChanged";
    cursor["activeSetlistId"] = document.value("activeSetlistId", std::string());
    cursor["cursorIndex"] = mSetlistCursorIndex.load();
    SendMessageToUI(cursor.dump());
}

nlohmann::json* PluginController::SetlistToEdit(nlohmann::json& document, const nlohmann::json& payload)
{
    auto id = payload.value("setlistId", std::string());

    if (id.empty())
    {
        id = document.value("activeSetlistId", std::string());
    }

    return id.empty() ? nullptr : FindSetlist(document, id);
}

void PluginController::HandleCreateSetlistRequest(const nlohmann::json& payload)
{
    const auto name = Trimmed(payload.value("name", std::string()));

    if (name.empty())
    {
        return;
    }

    auto document = LoadSetlistsDocument();
    const auto bank = BankValue(payload);

    if (BankTaken(document, bank, {}))
    {
        ReportErrorToUI("Bank already used", "Only one setlist can use a bank number.");
        return;
    }

    // A UI may name it (the web UI's pads edit the new setlist at once); else the engine does.
    auto id = payload.value("id", std::string());

    if (id.empty() || FindSetlist(document, id) != nullptr)
    {
        id = controller_detail::GenerateGuidV4String();
    }

    document["setlists"].push_back({{"id", id}, {"name", name}, {"bank", bank}, {"slots", nlohmann::json::array()}});
    document["activeSetlistId"] = id;
    document["cursorIndex"] = 0;
    SaveSetlistsDocument(document);
}

void PluginController::HandleUpdateSetlistRequest(const nlohmann::json& payload)
{
    auto document = LoadSetlistsDocument();
    auto* setlist = SetlistToEdit(document, payload);

    if (setlist == nullptr)
    {
        return;
    }

    const auto name = payload.contains("name") ? Trimmed(payload.value("name", std::string()))
                                               : setlist->value("name", std::string());

    if (name.empty())
    {
        return;
    }

    const auto bank = payload.contains("bank") ? BankValue(payload) : setlist->value("bank", nlohmann::json());

    if (BankTaken(document, bank, setlist->value("id", std::string())))
    {
        ReportErrorToUI("Bank already used", "Only one setlist can use a bank number.");
        return;
    }

    (*setlist)["name"] = name;
    (*setlist)["bank"] = bank;
    SaveSetlistsDocument(document);
}

void PluginController::HandleDeleteSetlistRequest(const nlohmann::json& payload)
{
    auto document = LoadSetlistsDocument();
    auto* setlist = SetlistToEdit(document, payload);

    if (setlist == nullptr)
    {
        return;
    }

    const auto id = setlist->value("id", std::string());
    auto& list = document["setlists"];
    std::size_t index = 0;

    while (index < list.size() && list[index].value("id", std::string()) != id)
    {
        ++index;
    }

    list.erase(list.begin() + static_cast<std::ptrdiff_t>(index));

    if (document.value("activeSetlistId", std::string()) == id)
    {
        // The next one, else the one before, as the web UI's deleteActiveSetlist.
        const nlohmann::json* next = index < list.size() ? &list[index] : (index > 0 ? &list[index - 1] : nullptr);
        document["activeSetlistId"] = next != nullptr ? next->value("id", std::string()) : std::string();
        document["cursorIndex"] = 0;
    }

    SaveSetlistsDocument(document);
}

void PluginController::HandleAddSetlistSlotRequest(const nlohmann::json& payload)
{
    const auto presetId = payload.value("presetId", std::string());
    auto document = LoadSetlistsDocument();
    auto* setlist = SetlistToEdit(document, payload);

    if (setlist == nullptr || presetId.empty())
    {
        return;
    }

    if (!(*setlist)["slots"].is_array())
    {
        (*setlist)["slots"] = nlohmann::json::array();
    }

    (*setlist)["slots"].push_back({{"presetId", presetId}});
    SaveSetlistsDocument(document);
}

void PluginController::HandleSetSetlistSlotRequest(const nlohmann::json& payload)
{
    // A pad assigned past the end fills the gap with empty slots; an empty id clears the slot.
    const int index = payload.value("index", -1);
    const auto presetId = payload.value("presetId", std::string());
    auto document = LoadSetlistsDocument();
    auto* setlist = SetlistToEdit(document, payload);

    if (setlist == nullptr || index < 0 || index > 1023)
    {
        return;
    }

    auto& slots = (*setlist)["slots"];

    if (!slots.is_array())
    {
        slots = nlohmann::json::array();
    }

    if (presetId.empty() && static_cast<std::size_t>(index) >= slots.size())
    {
        return;
    }

    while (slots.size() <= static_cast<std::size_t>(index))
    {
        slots.push_back({{"presetId", ""}});
    }

    slots[static_cast<std::size_t>(index)] = {{"presetId", presetId}};
    SaveSetlistsDocument(document);
}

void PluginController::HandleMoveSetlistSlotRequest(const nlohmann::json& payload)
{
    const int from = payload.value("fromIndex", -1);
    const int to = payload.value("toIndex", -1);
    auto document = LoadSetlistsDocument();
    auto* setlist = SetlistToEdit(document, payload);

    if (setlist == nullptr || !(*setlist)["slots"].is_array())
    {
        return;
    }

    auto& slots = (*setlist)["slots"];
    const int count = static_cast<int>(slots.size());

    if (from < 0 || to < 0 || from >= count || to >= count || from == to)
    {
        return;
    }

    auto slot = slots[static_cast<std::size_t>(from)];
    slots.erase(slots.begin() + from);
    slots.insert(slots.begin() + to, std::move(slot));

    // The cursor stays on the slot it was on: a footswitch "next" after the move still lands on
    // the slot after the one playing.
    if (setlist->value("id", std::string()) == document.value("activeSetlistId", std::string()))
    {
        int cursor = document.value("cursorIndex", 0);

        if (cursor == from)
        {
            cursor = to;
        }
        else if (from < cursor && cursor <= to)
        {
            --cursor;
        }
        else if (to <= cursor && cursor < from)
        {
            ++cursor;
        }

        document["cursorIndex"] = cursor;
    }

    SaveSetlistsDocument(document);
}

void PluginController::HandleRemoveSetlistSlotRequest(const nlohmann::json& payload)
{
    const int index = payload.value("index", -1);
    auto document = LoadSetlistsDocument();
    auto* setlist = SetlistToEdit(document, payload);

    if (setlist == nullptr || !(*setlist)["slots"].is_array())
    {
        return;
    }

    auto& slots = (*setlist)["slots"];

    if (index < 0 || index >= static_cast<int>(slots.size()))
    {
        return;
    }

    slots.erase(slots.begin() + index);

    if (setlist->value("id", std::string()) == document.value("activeSetlistId", std::string()))
    {
        int cursor = document.value("cursorIndex", 0);

        if (index < cursor)
        {
            --cursor;
        }
        else if (index == cursor)
        {
            cursor = std::max(0, std::min(cursor, static_cast<int>(slots.size()) - 1));
        }

        document["cursorIndex"] = cursor;
    }

    SaveSetlistsDocument(document);
}
} // namespace guitarfx
