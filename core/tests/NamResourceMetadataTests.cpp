/**
 * NamResourceMetadataTests.cpp — Reading a .nam model's own metadata for the library.
 *
 * A .nam file is one JSON document. An A2 model is a SlimmableContainer whose submodels
 * are whole WaveNet models with their own "architecture", "metadata", "version" and
 * "sample_rate", and trainers write the top-level members in any order: metadata before
 * the architecture, after the weights, config first. These check that:
 *  - only the top-level members are read, in every key order seen in the wild, with the
 *    weights longer than any read buffer;
 *  - an A2 file is classified A2 whether it is a SlimmableContainer or an unwrapped A2
 *    WaveNet, and an A1 WaveNet A1;
 *  - strings holding braces, quotes and key names, and values straddling the read buffer,
 *    do not confuse the reader, and a truncated file keeps what came before the break;
 *  - the testdata A2 captures read as A2 with their calibration levels;
 *  - enrichment lets the file's structure replace a stale row, keeps the Tone3000 API's
 *    architecture version and a user's edit, and leaves a row alone when the file is gone;
 *  - startup re-reads every model once for a library filled by the older reader.
 */

#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "MessageThreadTestHost.h"
#include "PluginController.h"
#include "controller/internal/NamResourceMetadata.h"
#include "resources/ResourceLibrary.h"
#include "storage/JsonStore.h"
#include "util/PathEncoding.h"

namespace fs = std::filesystem;
using namespace guitarfx;
using namespace guitarfx::controller_detail;
using ordered_json = nlohmann::ordered_json;

namespace
{
class QuietHost final : public test::PumpedTestHost
{
  public:
    explicit QuietHost(const fs::path& root) : PumpedTestHost(root, std::this_thread::get_id(), 48000.0, 512)
    {
    }

    void SendMessageToUI(const std::string&) override
    {
    }

    void RunOnMainThread(std::function<void()> fn) override
    {
        fn();
    }
};

fs::path FreshSandbox(const std::string& name)
{
    // Per-process, so a parallel ctest run of another build cannot delete it mid-test.
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-nam-metadata-tests" /
                             (name + "-" + std::to_string(std::random_device{}()));
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    test::SetSettingsEnvRoot(sandbox);
    return sandbox;
}

bool Expect(bool ok, const std::string& what)
{
    if (!ok)
    {
        std::cerr << "  " << what << "\n";
    }

    return ok;
}

bool ExpectEq(const std::string& actual, const std::string& expected, const std::string& what)
{
    return Expect(actual == expected, what + ": got \"" + actual + "\", expected \"" + expected + "\"");
}

NamFileMetadata Read(const std::string& text)
{
    std::istringstream in(text);
    return ReadNamMetadata(in);
}

/// More numbers than one 64 KB read holds, so anything after them is past the old limit.
ordered_json Weights(std::size_t count = 40000)
{
    ordered_json weights = ordered_json::array();

    for (std::size_t i = 0; i < count; ++i)
    {
        weights.push_back(std::sin(static_cast<double>(i)) * 0.0123456789);
    }

    return weights;
}

ordered_json A1Layers()
{
    return ordered_json::array({ordered_json{{"input_size", 1},
                                             {"condition_size", 1},
                                             {"head_size", 8},
                                             {"channels", 16},
                                             {"kernel_size", 3},
                                             {"dilations", {1, 2, 4, 8}},
                                             {"activation", "Tanh"},
                                             {"gated", false},
                                             {"head_bias", false}}});
}

/// The layer layout the A2 trainer writes, unwrapped or inside a SlimmableContainer.
ordered_json A2Layers()
{
    return ordered_json::array({ordered_json{{"input_size", 1},
                                             {"condition_size", 1},
                                             {"head", {{"out_channels", 1}, {"kernel_size", 16}, {"bias", true}}},
                                             {"channels", 8},
                                             {"kernel_sizes", {3, 3, 3}},
                                             {"dilations", {1, 2, 4}},
                                             {"activation", {"Tanh", "Tanh", "Tanh"}},
                                             {"bottleneck", 8},
                                             {"head1x1", {{"active", false}, {"out_channels", 1}, {"groups", 1}}},
                                             {"layer1x1", {{"active", true}, {"groups", 1}}}}});
}

/// A nested submodel whose own metadata and version must never be taken for the file's.
ordered_json Submodel(double maxValue)
{
    return ordered_json{{"max_value", maxValue},
                        {"model",
                         {{"version", "0.6.9"},
                          {"metadata",
                           {{"date", {{"year", 1999}, {"month", 1}, {"day", 1}}},
                            {"name", "submodel"},
                            {"input_level_dbu", -99},
                            {"output_level_dbu", -98}}},
                          {"architecture", "WaveNet"},
                          {"config", {{"layers", A2Layers()}, {"head", nullptr}, {"head_scale", 0.01}}},
                          {"weights", Weights(2000)},
                          {"sample_rate", 44100}}}};
}

ordered_json TopMetadata()
{
    return ordered_json{{"date", {{"year", 2026}, {"month", 5}, {"day", 7}, {"hour", 21}}},
                        {"loudness", -19.25},
                        {"name", "Top {\"architecture\": \"WaveNet\"} \\ name"},
                        {"modeled_by", "Someone"},
                        {"gear_type", "amp_cab"},
                        {"gear_make", "Make"},
                        {"gear_model", "Model"},
                        {"tone_type", "hi_gain"},
                        {"input_level_dbu", 18.995},
                        {"output_level_dbu", -3.5e-1},
                        {"training", {{"settings", {{"ignore_checks", false}}}, {"final_loss", 0.0125}}}};
}

/// A model whose members appear in `order`, each by its .nam key name.
std::string Document(const std::vector<std::string>& order, const std::string& architecture, const ordered_json& config)
{
    ordered_json doc = ordered_json::object();

    for (const auto& key : order)
    {
        if (key == "version")
        {
            doc["version"] = "0.7.0";
        }
        else if (key == "metadata")
        {
            doc["metadata"] = TopMetadata();
        }
        else if (key == "architecture")
        {
            doc["architecture"] = architecture;
        }
        else if (key == "config")
        {
            doc["config"] = config;
        }
        else if (key == "weights")
        {
            doc["weights"] = Weights();
        }
        else if (key == "sample_rate")
        {
            doc["sample_rate"] = 48000;
        }
    }

    return doc.dump(2);
}

ordered_json SlimmableConfig()
{
    return ordered_json{{"submodels", {Submodel(0.5), Submodel(1.0)}}};
}

ordered_json WaveNetConfig(const ordered_json& layers)
{
    return ordered_json{{"layers", layers}, {"head", nullptr}, {"head_scale", 0.02}};
}

bool ExpectTopLevel(const NamFileMetadata& meta, const std::string& what)
{
    bool ok = true;
    ok &= ExpectEq(meta.fileVersion, "0.7.0", what + " version");
    ok &= ExpectEq(meta.sampleRate, "48000", what + " sample rate");
    ok &= ExpectEq(meta.namName, "Top {\"architecture\": \"WaveNet\"} \\ name", what + " name");
    ok &= ExpectEq(meta.modeledBy, "Someone", what + " modeled_by");
    ok &= ExpectEq(meta.gearType, "amp_cab", what + " gear_type");
    ok &= ExpectEq(meta.gearMake, "Make", what + " gear_make");
    ok &= ExpectEq(meta.gearModel, "Model", what + " gear_model");
    ok &= ExpectEq(meta.toneType, "hi_gain", what + " tone_type");
    ok &= ExpectEq(meta.inputLevelDbu, "18.995", what + " input level");
    ok &= ExpectEq(meta.outputLevelDbu, "-0.35", what + " output level");
    ok &= ExpectEq(meta.modelDate, "2026-05-07", what + " date");
    ok &= ExpectEq(meta.trainingFinalLoss, "0.0125", what + " final loss");
    return ok;
}

const std::vector<std::vector<std::string>> kKeyOrders = {
    // The three orders trainers write A2 files in, from a survey of 1,634 files.
    {"version", "metadata", "architecture", "config", "weights", "sample_rate"},
    {"version", "architecture", "config", "weights", "sample_rate", "metadata"},
    {"config", "version", "weights", "metadata", "sample_rate", "architecture"},
    // And the rest of the hard cases: everything after the weights, metadata last.
    {"weights", "config", "sample_rate", "architecture", "version", "metadata"},
};

std::string OrderName(const std::vector<std::string>& order)
{
    std::string name;

    for (const auto& key : order)
    {
        name += (name.empty() ? "" : ",") + key;
    }

    return name;
}

bool TestSlimmableInEveryKeyOrder()
{
    bool ok = true;

    for (const auto& order : kKeyOrders)
    {
        const auto meta = Read(Document(order, "SlimmableContainer", SlimmableConfig()));
        const std::string what = "SlimmableContainer [" + OrderName(order) + "]";
        ok &= ExpectEq(meta.architecture, "SlimmableContainer", what + " architecture");
        ok &= ExpectEq(meta.architectureVersion, "A2", what + " architecture version");
        ok &= ExpectTopLevel(meta, what);
    }

    return ok;
}

bool TestWaveNetLayoutsInEveryKeyOrder()
{
    bool ok = true;

    for (const auto& order : kKeyOrders)
    {
        const std::string name = OrderName(order);
        const auto a2 = Read(Document(order, "WaveNet", WaveNetConfig(A2Layers())));
        ok &= ExpectEq(a2.architecture, "WaveNet", "unwrapped A2 [" + name + "] architecture");
        ok &= ExpectEq(a2.architectureVersion, "A2", "unwrapped A2 [" + name + "] architecture version");
        ok &= ExpectTopLevel(a2, "unwrapped A2 [" + name + "]");

        const auto a1 = Read(Document(order, "WaveNet", WaveNetConfig(A1Layers())));
        ok &= ExpectEq(a1.architectureVersion, "A1", "A1 WaveNet [" + name + "] architecture version");
        ok &= ExpectTopLevel(a1, "A1 WaveNet [" + name + "]");
    }

    const auto lstm = Read(R"({"version":"0.5.4","architecture":"LSTM","config":{"num_layers":1,"hidden_size":16},
                              "weights":[0.1,0.2],"sample_rate":48000})");
    ok &= ExpectEq(lstm.architecture, "LSTM", "LSTM architecture");
    ok &= ExpectEq(lstm.architectureVersion, "", "LSTM architecture version");
    ok &= ExpectEq(ClassifyNamArchitectureVersion("ConvNet", false), "", "ConvNet architecture version");
    return ok;
}

bool TestMissingAndOddValues()
{
    bool ok = true;

    // null calibration is no calibration, and NaN (which Python's json writes) is stepped over.
    const auto meta = Read(R"({"metadata":{"input_level_dbu":null,"output_level_dbu":null,"loudness":NaN,
                               "name":"Édith \"Le Son\""},"architecture":"WaveNet","version":"0.5.4",
                               "config":{"layers":[]},"weights":[1e-3,-2.5E+2,3],"sample_rate":44100.0})");
    ok &= ExpectEq(meta.inputLevelDbu, "", "null input level");
    ok &= ExpectEq(meta.outputLevelDbu, "", "null output level");
    ok &= ExpectEq(meta.namName,
                   "\xC3\x89"
                   "dith \"Le Son\"",
                   "escaped name");
    ok &= ExpectEq(meta.architecture, "WaveNet", "architecture after NaN");
    ok &= ExpectEq(meta.sampleRate, "44100.0", "sample rate");

    const auto notAModel = Read(R"({"patchList":[],"device":"x","liveSetData":{},"version":"1.0.0"})");
    ok &= ExpectEq(notAModel.architecture, "", "a JSON file that is not a model");
    ok &= ExpectEq(notAModel.architectureVersion, "", "a JSON file that is not a model, version");

    ok &= ExpectEq(Read("").architecture, "", "an empty file");
    ok &= ExpectEq(Read("[1,2,3]").architecture, "", "a top-level array");
    return ok;
}

bool TestValuesStraddlingTheReadBuffer()
{
    // A padding string pushes each later token across the 64 KB buffer edge in turn, so a
    // key, an escape, a number and a closing brace each get split between two reads.
    bool ok = true;
    const std::string tail =
        R"(","metadata":{"name":"a\"b\\c","input_level_dbu":-12.75,"date":{"year":2025,"month":12,"day":3}},)"
        R"("architecture":"SlimmableContainer","sample_rate":48000})";

    for (std::size_t pad = 65536 + 2 - tail.size(); pad <= 65536 + 2; ++pad)
    {
        const std::string text = R"({"version":"0.7.0","notes":")" + std::string(pad - 30, 'x') + tail;
        const auto meta = Read(text);
        const std::string what = "padding " + std::to_string(pad);
        ok &= ExpectEq(meta.namName, "a\"b\\c", what + " name");
        ok &= ExpectEq(meta.inputLevelDbu, "-12.75", what + " input level");
        ok &= ExpectEq(meta.modelDate, "2025-12-03", what + " date");
        ok &= ExpectEq(meta.architectureVersion, "A2", what + " architecture version");
        ok &= ExpectEq(meta.sampleRate, "48000", what + " sample rate");

        if (!ok)
        {
            break;
        }
    }

    return ok;
}

bool TestTruncatedFileKeepsWhatCameFirst()
{
    const std::string full = Document(kKeyOrders[0], "SlimmableContainer", SlimmableConfig());
    const auto meta = Read(full.substr(0, full.size() / 2));
    bool ok = true;
    ok &= ExpectEq(meta.architecture, "SlimmableContainer", "truncated architecture");
    ok &= ExpectEq(meta.architectureVersion, "A2", "truncated architecture version");
    ok &= ExpectEq(meta.inputLevelDbu, "18.995", "truncated input level");
    ok &= ExpectEq(meta.sampleRate, "", "truncated sample rate (after the break)");
    return ok;
}

fs::path A2AssetsDir()
{
    return fs::path(GUITARFX_TEST_RESOURCES_DIR) / "assets" / "amps" / "Guitar" / "A2";
}

/// The testdata capture whose top-level metadata comes after its weights.
fs::path MetadataLastAsset()
{
    for (const auto& entry : fs::directory_iterator(A2AssetsDir()))
    {
        if (entry.path().filename().u8string().rfind(u8"A2 -wth space", 0) == 0)
        {
            return entry.path();
        }
    }

    return {};
}

bool TestTestdataCaptures()
{
    bool ok = true;
    int a2Files = 0;

    for (const auto& entry : fs::directory_iterator(A2AssetsDir()))
    {
        if (entry.path().extension() != ".nam")
        {
            continue;
        }

        ++a2Files;
        const auto meta = TryExtractNamMetadata(entry.path());
        const std::string what = util::PathToUtf8(entry.path().filename());
        ok &= ExpectEq(meta.architecture, "SlimmableContainer", what + " architecture");
        ok &= ExpectEq(meta.architectureVersion, "A2", what + " architecture version");
        ok &= Expect(!meta.inputLevelDbu.empty(), what + " has no input level");
        // Written as 48000 by some trainers and 48000.0 by others.
        ok &= Expect(!meta.sampleRate.empty() && std::stod(meta.sampleRate) == 48000.0,
                     what + " sample rate: got \"" + meta.sampleRate + "\"");
    }

    ok &= Expect(a2Files >= 4, "found only " + std::to_string(a2Files) + " A2 testdata files");

    const auto metadataLast = TryExtractNamMetadata(MetadataLastAsset());
    ok &= ExpectEq(metadataLast.inputLevelDbu, "14.76", "metadata-after-weights capture input level");
    ok &= ExpectEq(metadataLast.modelDate.substr(0, 4), "2026", "metadata-after-weights capture date");

    const fs::path a1 = fs::path(GUITARFX_TEST_RESOURCES_DIR) / "assets" / "amps" / "Guitar" / "TimR" /
                        "JCM800 2203 1985" / "JCM800 Hi P6 B8 M4 T7 G10.nam";
    const auto a1Meta = TryExtractNamMetadata(a1);
    ok &= ExpectEq(a1Meta.architecture, "WaveNet", "A1 capture architecture");
    ok &= ExpectEq(a1Meta.architectureVersion, "A1", "A1 capture architecture version");
    ok &= ExpectEq(a1Meta.fileVersion, "0.5.0", "A1 capture version");
    return ok;
}

std::string MetadataValue(const LibraryResource& resource, const std::string& key)
{
    const auto it = resource.metadata.find(key);
    return it == resource.metadata.end() ? std::string{"(none)"} : it->second;
}

LibraryResource StaleRow(const std::string& id, const fs::path& file)
{
    // What the older reader stored for an A2 capture whose metadata it missed, plus the
    // "A1" the UI's own guess then persisted.
    LibraryResource resource;
    resource.type = "nam";
    resource.id = id;
    resource.name = id;
    resource.category = "amp";
    resource.filePath = file;
    resource.metadata = {{"provider", "local"},
                         {"namFileVersion", "0.7.0"},
                         {"architecture", "WaveNet"},
                         {"architectureVersion", "A1"},
                         {"modelDate", "1999-01-01"}};
    return resource;
}

bool TestEnrichment()
{
    const fs::path sandbox = FreshSandbox("enrich");
    const fs::path a2 = sandbox / "a2.nam";
    const fs::path a1 = sandbox / "a1.nam";
    std::ofstream(a2, std::ios::binary) << Document(kKeyOrders[2], "SlimmableContainer", SlimmableConfig());
    std::ofstream(a1, std::ios::binary) << Document(kKeyOrders[0], "WaveNet", WaveNetConfig(A1Layers()));
    bool ok = true;

    // A stale row: structure and the missing calibration come from the file; the date the
    // older reader took from a submodel stands until a full refresh, like any user edit.
    auto stale = StaleRow("stale", a2);
    EnrichNamResourceMetadata(stale, a2);
    ok &= ExpectEq(MetadataValue(stale, "architecture"), "SlimmableContainer", "stale architecture");
    ok &= ExpectEq(MetadataValue(stale, "architectureVersion"), "A2", "stale architecture version");
    ok &= ExpectEq(MetadataValue(stale, "inputLevelDbu"), "18.995", "stale input level");
    ok &= ExpectEq(MetadataValue(stale, "outputLevelDbu"), "-0.35", "stale output level");
    ok &= ExpectEq(MetadataValue(stale, "gear_type"), "amp_cab", "stale gear type");
    ok &= ExpectEq(MetadataValue(stale, "modelDate"), "1999-01-01", "a stored date replaced without a refresh");

    auto refreshed = StaleRow("refreshed", a2);
    EnrichNamResourceMetadata(refreshed, a2, NamMetadataRefresh::FromFile);
    ok &= ExpectEq(MetadataValue(refreshed, "modelDate"), "2026-05-07", "refreshed date");
    ok &= ExpectEq(MetadataValue(refreshed, "provider"), "local", "refresh dropped an unrelated key");

    // The Tone3000 API's own architecture version stands; an empty one is filled in.
    for (const std::string apiValue : {"1", "2", "custom"})
    {
        auto tone3000 = StaleRow("tone3000", a2);
        tone3000.metadata["provider"] = "tone3000";
        tone3000.metadata["architectureVersion"] = apiValue;
        EnrichNamResourceMetadata(tone3000, a2, NamMetadataRefresh::FromFile);
        ok &= ExpectEq(MetadataValue(tone3000, "architectureVersion"), apiValue, "Tone3000 API value " + apiValue);
    }

    auto tone3000Empty = StaleRow("tone3000-empty", a1);
    tone3000Empty.metadata["provider"] = "tone3000";
    tone3000Empty.metadata["architectureVersion"] = "";
    EnrichNamResourceMetadata(tone3000Empty, a1);
    ok &= ExpectEq(MetadataValue(tone3000Empty, "architectureVersion"), "A1", "empty Tone3000 value");

    // An A2 guess on an A1 file is corrected too, not only the other way round.
    auto wrongA2 = StaleRow("wrong-a2", a1);
    wrongA2.metadata["architectureVersion"] = "A2";
    EnrichNamResourceMetadata(wrongA2, a1);
    ok &= ExpectEq(MetadataValue(wrongA2, "architectureVersion"), "A1", "an A2 guess on an A1 file");

    // A user's edit to a descriptive field stands.
    auto edited = StaleRow("edited", a2);
    edited.metadata["namName"] = "My name";
    edited.metadata["inputLevelDbu"] = "10";
    EnrichNamResourceMetadata(edited, a2);
    ok &= ExpectEq(MetadataValue(edited, "namName"), "My name", "a user's name");
    ok &= ExpectEq(MetadataValue(edited, "inputLevelDbu"), "10", "a user's input level");

    // A missing file says nothing, so the row is left exactly as it was.
    auto gone = StaleRow("gone", sandbox / "gone.nam");
    const auto before = gone.metadata;
    EnrichNamResourceMetadata(gone, gone.filePath, NamMetadataRefresh::FromFile);
    ok &= Expect(gone.metadata == before, "a row whose file is gone was changed");
    return ok;
}

bool TestStartupRereadsOnce()
{
    const fs::path sandbox = FreshSandbox("startup");
    // Copied to a plain name: the asset's own name is not what this checks.
    const fs::path capture = sandbox / "metadata-last.nam";
    std::error_code copyError;
    fs::copy_file(MetadataLastAsset(), capture, copyError);

    {
        QuietHost host(sandbox);
        PluginController controller(host);
        controller.Initialize();
    }

    fs::path dbPath;

    for (const auto& entry : fs::recursive_directory_iterator(sandbox))
    {
        if (entry.path().filename() == "soundshed.db")
        {
            dbPath = entry.path();
        }
    }

    if (!Expect(!dbPath.empty(), "no document store was created") ||
        !Expect(!copyError, "could not copy the A2 asset: " + copyError.message()))
    {
        return false;
    }

    const auto writeRows = [&](const char* revision) {
        storage::JsonStore store;
        std::string error;

        if (!Expect(store.Open(dbPath, error), "could not open the store: " + error))
        {
            return false;
        }

        auto tone3000 = StaleRow("tone3000", capture);
        tone3000.metadata["provider"] = "tone3000";
        tone3000.metadata["architectureVersion"] = "1";
        ResourceLibrary::PutInStore(store, StaleRow("local", capture), sandbox / "elsewhere");
        ResourceLibrary::PutInStore(store, tone3000, sandbox / "elsewhere");
        return store.SetMeta(kNamMetadataRevisionKey, revision);
    };

    // A library the older reader filled: no revision stamp yet.
    if (!writeRows("1"))
    {
        return false;
    }

    bool ok = true;
    {
        QuietHost host(sandbox);
        PluginController controller(host);
        controller.Initialize();
        const auto local = controller.GetResourceLibrary().LookupResource("nam", "local");
        const auto tone3000 = controller.GetResourceLibrary().LookupResource("nam", "tone3000");

        if (!Expect(local && tone3000, "the seeded rows did not load"))
        {
            return false;
        }

        ok &= ExpectEq(MetadataValue(*local, "architecture"), "SlimmableContainer", "re-read architecture");
        ok &= ExpectEq(MetadataValue(*local, "architectureVersion"), "A2", "re-read architecture version");
        ok &= ExpectEq(MetadataValue(*local, "inputLevelDbu"), "14.76", "re-read input level");
        ok &= ExpectEq(MetadataValue(*local, "modelDate").substr(0, 4), "2026", "re-read date");
        ok &= ExpectEq(MetadataValue(*tone3000, "architectureVersion"), "1", "re-read kept Tone3000's value");
    }

    // The pass is saved: a fresh store handle sees the corrected row and the stamp.
    {
        storage::JsonStore store;
        std::string error;
        ok &= Expect(store.Open(dbPath, error), "could not reopen the store: " + error);
        ok &=
            ExpectEq(store.GetMeta(kNamMetadataRevisionKey).value_or("(none)"), kNamMetadataRevision, "revision stamp");
    }

    // Once stamped, a launch reads no model again: a stale row written now stays stale.
    if (!writeRows(kNamMetadataRevision))
    {
        return false;
    }

    QuietHost host(sandbox);
    PluginController controller(host);
    controller.Initialize();
    const auto local = controller.GetResourceLibrary().LookupResource("nam", "local");
    ok &= Expect(local && MetadataValue(*local, "architectureVersion") == "A1",
                 "a stamped library re-read its models at launch");
    return ok;
}
} // namespace

int main()
{
    int passed = 0;
    int failed = 0;

    const auto run = [&](const char* name, bool ok) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << "\n";
        (ok ? passed : failed)++;
    };

    run("SlimmableContainer reads top-level members in every key order", TestSlimmableInEveryKeyOrder());
    run("WaveNet layouts classify A1/A2 in every key order", TestWaveNetLayoutsInEveryKeyOrder());
    run("Null, NaN, escapes and non-models", TestMissingAndOddValues());
    run("Values straddling the read buffer", TestValuesStraddlingTheReadBuffer());
    run("A truncated file keeps what came first", TestTruncatedFileKeepsWhatCameFirst());
    run("Testdata captures", TestTestdataCaptures());
    run("Enrichment: the file's structure wins, edits and Tone3000 stand", TestEnrichment());
    run("Startup re-reads every model once", TestStartupRereadsOnce());

    std::cout << "\nNAM resource metadata tests: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
