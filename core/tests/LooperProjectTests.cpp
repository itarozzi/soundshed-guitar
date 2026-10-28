// Looper projects through the controller: the session folder written as layers finish, save and
// the looper-projects index, the unsaved marker, load back, rename and delete, a layer exported as
// a riff take, and a session recovered after the app went away with a loop in it.

#include "LooperControllerSession.h"
#include "controller/LooperProjectFiles.h"
#include "dsp/effects/BuiltinEffects.h"

#include <filesystem>

namespace fs = std::filesystem;
using namespace guitarfx;
using namespace looper_session;

namespace
{
struct App
{
    LooperHost host;
    PluginController controller;
    Session session;

    explicit App(const fs::path& sandbox)
        : host(sandbox, std::this_thread::get_id(), kSampleRate, kBlock), controller(host), session{host, controller}
    {
        controller.Initialize();
        controller.Prepare(kSampleRate, kBlock);
        host.Pump();
        session.Run(0.05);
    }

    fs::path Loops()
    {
        session.Send({{"type", "getRiffLibrary"}});
        session.Run(0.01);
        return fs::path(host.Last("riffLibraryState")["library"].value("path", std::string{})) / "loops";
    }

    /// A half-second loop of the tone and one overdub pass on it, left playing.
    void RecordTwoLayers()
    {
        session.playing = true;
        session.Send({{"type", "looperCommand"}, {"command", "recPlayDub"}});
        session.Run(0.5);
        session.Send({{"type", "looperCommand"}, {"command", "recPlayDub"}});
        session.Run(0.1);
        session.Send({{"type", "looperCommand"}, {"command", "overdub"}});
        session.Run(0.3);
        session.Send({{"type", "looperCommand"}, {"command", "play"}});
        session.playing = false;
        session.Run(0.3);
        session.Settle();
    }
};

void TestSaveLoadAndTheIndex(const fs::path& sandbox)
{
    App app(sandbox);
    Session& s = app.session;
    s.Send({{"type", "setLooper"}, {"enabled", true}, {"maxSeconds", 30}});
    s.Run(0.05);
    const fs::path loops = app.Loops();
    app.RecordTwoLayers();

    const auto session = looper_files::ReadJson(loops / ".session" / "project.json");
    Check(session && (*session)["layers"].size() == 2, "the session folder follows the loop as layers finish");

    if (session && (*session)["layers"].size() == 2)
    {
        const std::string key = (*session)["layers"][0].value("id", std::string{});
        Check(fs::exists(looper_files::DiPath(loops / ".session", key)) &&
                  fs::exists(looper_files::WetPath(loops / ".session", key)),
              "with each layer's DI and wet files");
    }

    Check(s.State().value("dirty", false), "a new loop is unsaved");
    s.Send({{"type", "saveLooperProject"}, {"title", "Test Loop"}});
    s.Settle();
    const auto saved = app.host.Last("looperProjectSaved");
    const std::string id = saved.value("projectId", std::string{});
    Check(!id.empty() && saved.value("title", std::string{}) == "Test Loop", "saving answers with the project");
    Check(!s.State().value("dirty", true) && s.State().value("projectTitle", std::string{}) == "Test Loop",
          "and it is no longer unsaved");
    const auto project = looper_files::ReadJson(loops / id / "project.json");
    Check(project && (*project)["layers"].size() == 2 && project->value("title", std::string{}) == "Test Loop",
          "the project is in the riff library's loops folder");
    const auto list = app.host.Last("looperProjects")["projects"];
    Check(list.is_array() && list.size() == 1 && list[0].value("id", std::string{}) == id &&
              list[0].value("layerCount", 0) == 2,
          "and in the looper-projects index");

    const auto layers = app.host.Last("looperLayers")["layers"];
    s.Send({{"type", "setLooperLayer"}, {"layerId", layers[0].value("id", std::string{})}, {"level", 0.5}});
    s.Run(0.05);
    Check(s.State().value("dirty", false), "a mix change marks it unsaved");

    s.Send({{"type", "looperCommand"}, {"command", "clear"}});
    s.Run(0.2);
    Check(s.State().value("layerCount", -1) == 0, "cleared");
    s.Send({{"type", "loadLooperProject"}, {"projectId", id}});
    s.Settle();
    auto state = s.State();
    Check(state.value("layerCount", 0) == 2 && std::abs(state.value("lengthSec", 0.0) - 0.5) < 0.02 &&
              state.value("state", std::string{}) == "stopped",
          "loading brings the loop back, stopped: " + state.dump());
    Check(!state.value("dirty", true) && state.value("projectId", std::string{}) == id,
          "as the saved project, not unsaved");
    const auto loaded = app.host.Last("looperLayers")["layers"];
    Check(loaded.is_array() && loaded.size() == 2 && loaded[1].value("title", std::string{}) == "Layer 1",
          "with the layers' names");

    // A layer's DI as a riff take, waiting to be saved in the riff library.
    s.Send({{"type", "exportLooperLayer"}, {"layerId", loaded[1].value("id", std::string{})}, {"as", "riff"}});
    s.Run(0.05);
    const auto riff = app.host.Last("riffCaptureStopped");
    Check(riff.value("source", std::string{}) == "looper" && riff.value("hasAudio", false) &&
              std::abs(static_cast<double>(riff.value("capturedSamples", 0)) - 0.5 * kSampleRate) < 1000.0,
          "a layer exports as a riff take: " + riff.dump().substr(0, 160));

    s.Send({{"type", "renameLooperProject"}, {"projectId", id}, {"title", "Renamed"}});
    s.Settle(300);
    Check(app.host.Last("looperProjects")["projects"][0].value("title", std::string{}) == "Renamed",
          "a project can be renamed");
    Check(looper_files::ReadJson(loops / id / "project.json")->value("title", std::string{}) == "Renamed",
          "on disk too");
    s.Send({{"type", "deleteLooperProject"}, {"projectId", id}});
    s.Settle(300);
    Check(app.host.Last("looperProjects")["projects"].empty() && !fs::exists(loops / id), "and deleted");
}

void TestRecovery(const fs::path& sandbox)
{
    {
        App app(sandbox);
        app.session.Send({{"type", "looperCommand"}, {"command", "clear"}});
        app.session.Run(0.2);
        app.RecordTwoLayers();
        // Gone with a loop in it, never saved.
    }

    App app(sandbox);
    Session& s = app.session;
    s.Send({{"type", "getLooperProjects"}});
    s.Run(0.05);
    Check(app.host.Last("looperProjects").value("recoverable", false), "the last session is offered back");
    s.Send({{"type", "recoverLooperSession"}});
    s.Settle();
    const auto state = s.State();
    Check(state.value("layerCount", 0) == 2, "and recovered: " + state.dump());
    Check(state.value("dirty", false) && state.value("projectId", std::string{"x"}).empty(), "as an unsaved loop");
    Check(!state.value("recoverable", true), "once, not again");
}
} // namespace

int main()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-looper-project-tests";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    test::SetSettingsEnvRoot(sandbox);
    RegisterAllEffects();

    TestSaveLoadAndTheIndex(sandbox);
    TestRecovery(sandbox);
    fs::remove_all(sandbox, ec);

    if (gFailures > 0)
    {
        std::cerr << gFailures << " check(s) failed" << std::endl;
        return 1;
    }

    std::cout << "LooperProjectTests passed" << std::endl;
    return 0;
}
