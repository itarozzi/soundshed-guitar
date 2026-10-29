/**
 * PresetInspectTests.cpp - The save dialog's preset inspector (core/src/uiclient/PresetInspect.h):
 * validation and the clean-up pass, held to what the web UI's presets/validate.ts reports for
 * the same preset.
 */

#include "uiclient/PresetBrowse.h"
#include "uiclient/PresetInspect.h"

#include <algorithm>
#include <iostream>
#include <string>

namespace uic = guitarfx::uiclient;
using nlohmann::json;

namespace
{
int gFailures = 0;

void Expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << what << std::endl;
        ++gFailures;
    }
}

bool Contains(const std::vector<std::string>& lines, const std::string& line)
{
    return std::find(lines.begin(), lines.end(), line) != lines.end();
}

void TestValidate()
{
    Expect(uic::ValidatePresetJson(json::object()) == std::vector<std::string>{"No preset loaded."}, "no preset");

    const json good = {{"id", "p1"},
                       {"name", "Clean"},
                       {"graph", {{"nodes", {{{"id", "a"}, {"type", "gain"}}}}, {"edges", json::array()}}}};
    Expect(uic::ValidatePresetJson(good).empty(), "a sound preset has no issues");

    json bad = good;
    bad["zebra"] = 1;
    bad["alpha"] = 2;
    bad["name"] = "  ";
    bad["graph"]["nodes"].push_back({{"id", ""}});
    bad["graph"]["edges"].push_back({{"from", "a"}});
    bad["scenes"] = "nope";
    const auto issues = uic::ValidatePresetJson(bad);
    Expect(Contains(issues, "Unknown fields: alpha, zebra"), "unknown fields, sorted");
    Expect(Contains(issues, "Missing or invalid preset name."), "a blank name");
    Expect(Contains(issues, "Graph node #2 is missing a valid id."), "a node without an id");
    Expect(Contains(issues, "Graph node #2 is missing a valid type."), "a node without a type");
    Expect(Contains(issues, "Graph edge #1 is missing valid endpoints."), "an edge without an end");
    Expect(Contains(issues, "Preset scenes must be an array when present."), "scenes that are not a list");
}

void TestCleanup()
{
    const json preset = {
        {"id", "p1"},
        {"name", "Clean"},
        {"description", " "},
        {"tags", json::array()},
        {"global", {{"inputTrim", 0}}},
        {"stray", true},
        {"irId", ""},
        {"graph",
         {{"nodes",
           {{{"id", "amp"}, {"type", "nam"}, {"resources", {{{"id", "m1"}, {"type", "model"}}}}},
            {{"id", "global_eq"},
             {"type", "eq_parametric"},
             {"enabled", false},
             {"params",
              {{"lowGain", 0.0},
               {"lowFreq", 100.0},
               {"lowMidGain", 0.0},
               {"lowMidFreq", 400.0},
               {"lowMidQ", 1.0},
               {"highMidGain", 0.0},
               {"highMidFreq", 2000.0},
               {"highMidQ", 1.0},
               {"highGain", 0.0},
               {"highFreq", 8000.0}}}},
            {{"id", "__output__"}, {"type", "output"}}}},
          {"edges", {{{"from", "__input__"}, {"to", "global_eq"}}, {{"from", "global_eq"}, {"to", "__output__"}}}}}}};

    const auto result = uic::CleanupPresetJson(preset);
    const auto& c = result.cleaned;
    Expect(!c.contains("stray") && !c.contains("global") && !c.contains("description") && !c.contains("tags") &&
               !c.contains("irId"),
           "unknown, legacy and empty fields go");
    Expect(c["graph"]["nodes"][0]["resources"][0] == json({{"resourceId", "m1"}, {"resourceType", "model"}}),
           "a resource ref's legacy aliases fold in");
    Expect(result.normalizedAliases == 2, "two aliases folded");
    Expect(result.removedGlobalEq && c["graph"]["nodes"].size() == 2, "the default global EQ is taken out");
    Expect(c["graph"]["edges"].size() == 1 && c["graph"]["edges"][0]["from"] == "__input__" &&
               c["graph"]["edges"][0]["to"] == "__output__",
           "and the input is linked straight to the output");

    const auto report = uic::CleanupReport(result);
    Expect(report.size() == 3 && report[1] == "Normalized resource ref aliases: 2" &&
               report[2] == "Removed default global EQ node.",
           "the report's lines");
    Expect(report[0].rfind("Removed fields: audioFxModelId, customIrPath, customModelPath, description, global,", 0) ==
               0,
           "removed fields, sorted, the absent model and IR keys among them as in the web UI");

    uic::PresetCleanup nothing;
    Expect(uic::CleanupReport(nothing) == std::vector<std::string>{"No unused fields removed."}, "nothing removed");
}

void TestInspectable()
{
    guitarfx::Preset preset;
    preset.id = "p1";
    preset.name = "Clean";
    const auto j = uic::InspectablePresetJson(preset);
    Expect(j.value("id", "") == "p1" && j.value("name", "") == "Clean", "the engine's serialisation");
    Expect(!j.contains("globalSignalChain"), "without the global chain");
}
} // namespace

void TestPeakHold()
{
    uic::PeakHold hold;
    Expect(!hold.Peak(), "no reading yet");
    hold.Push(-30.0, 0.0);
    hold.Push(-40.0, 2.0);
    Expect(hold.Peak() == -30.0, "a quieter reading leaves the hold");
    hold.Push(-12.0, 5.0);
    Expect(hold.Peak() == -12.0, "a louder one raises it");
    hold.Push(-50.0, 10.5);
    Expect(hold.Peak() == -50.0, "a window ten seconds old starts again");
    Expect(uic::FormatPeakDb(-12.34) == "-12.3 dBFS", "a peak's text");
    Expect(uic::FormatPeakDb(std::nullopt) == "\xE2\x80\x94", "no peak is a dash");
}

void TestUserPresetIds()
{
    Expect(uic::IsUserPresetId("1717568989143"), "a timestamp is a user preset");
    Expect(uic::IsUserPresetId("user-black-album"), "so is a user- id");
    Expect(uic::IsUserPresetId("4b0f9c1e-2d3a-4c5b-8e9f-0a1b2c3d4e5f"), "and a UUID");
    Expect(!uic::IsUserPresetId("4b0f9c1e-2d3a-6c5b-8e9f-0a1b2c3d4e5f"), "not a UUID of another version");
    Expect(!uic::IsUserPresetId("Preset-Pack-Vol.-1__black-album"), "a pack's preset is not");
    Expect(!uic::IsUserPresetId(""), "nor is no preset");
}

void TestFolders()
{
    std::vector<guitarfx::uiclient::PresetFolder> folders(2);
    folders[0].id = "b";
    folders[0].name = "beta";
    folders[1].id = "a";
    folders[1].name = "Alpha";
    folders[1].children.resize(1);
    folders[1].children[0].id = "a1";
    folders[1].children[0].name = "Leads";
    folders[1].children[0].presetIds = {"p1"};
    const auto flat = uic::FlattenPresetFolders(folders);
    Expect(flat.size() == 3 && flat[0].first->id == "a" && flat[1].first->id == "a1" && flat[1].second == 1 &&
               flat[2].first->id == "b",
           "folders by name, ignoring case, each followed by its own");
    Expect(uic::PresetFolderPath(folders, "p1") == "Alpha > Leads", "a preset's folders, outermost first");
    Expect(uic::PresetFolderPath(folders, "p2").empty(), "none for a preset in no folder");
}

int main()
{
    TestUserPresetIds();
    TestFolders();
    TestPeakHold();
    TestValidate();
    TestCleanup();
    TestInspectable();

    if (gFailures > 0)
    {
        std::cerr << gFailures << " failure(s)" << std::endl;
        return 1;
    }

    std::cout << "PresetInspectTests passed" << std::endl;
    return 0;
}
