// Looper re-tone through the controller: a layer's DI rendered offline through the current rig
// matches what was recorded live, swaps in, and the recorded tone comes back; and a riff take
// brought in as a layer, rendered through the rig.

#include "LooperControllerSession.h"
#include "controller/LooperProjectFiles.h"
#include "dsp/effects/BuiltinEffects.h"

#include <filesystem>
#include <numeric>

namespace fs = std::filesystem;
using namespace guitarfx;
using namespace looper_session;

namespace
{
/// The correlation of two signals and the level of `b` against `a` in dB, over [from, to).
std::pair<double, double> Compare(const std::vector<float>& a, const std::vector<float>& b, std::size_t from,
                                  std::size_t to)
{
    double ab = 0.0, aa = 0.0, bb = 0.0;

    for (std::size_t i = from; i < std::min({to, a.size(), b.size()}); ++i)
    {
        ab += static_cast<double>(a[i]) * b[i];
        aa += static_cast<double>(a[i]) * a[i];
        bb += static_cast<double>(b[i]) * b[i];
    }

    const double correlation = aa > 0.0 && bb > 0.0 ? ab / std::sqrt(aa * bb) : 0.0;
    const double levelDb = aa > 0.0 && bb > 0.0 ? 10.0 * std::log10(bb / aa) : -999.0;
    return {correlation, levelDb};
}

void TestRetone(const fs::path& sandbox)
{
    LooperHost host(sandbox, std::this_thread::get_id(), kSampleRate, kBlock);
    PluginController controller(host);
    controller.Initialize();
    controller.Prepare(kSampleRate, kBlock);
    host.Pump();
    Session s{host, controller};
    s.Run(0.05);
    s.Send({{"type", "setLooper"}, {"enabled", true}, {"maxSeconds", 30}});
    s.Run(0.05);
    s.Send({{"type", "getRiffLibrary"}});
    s.Run(0.01);
    const fs::path session =
        fs::path(host.Last("riffLibraryState")["library"].value("path", std::string{})) / "loops" / ".session";

    s.playing = true;
    s.Command("recPlayDub");
    s.Run(1.0);
    s.Command("recPlayDub");
    s.playing = false;
    s.Run(0.5);
    s.Settle();

    auto layers = host.Last("looperLayers")["layers"];
    Check(layers.is_array() && layers.size() == 1 && layers[0].value("wetSource", std::string{}) == "live",
          "one layer, recorded live");
    const auto project = looper_files::ReadJson(session / "project.json");
    const std::string key =
        project && !(*project)["layers"].empty() ? (*project)["layers"][0].value("id", std::string{}) : "";
    const auto live = looper_files::ReadWav(looper_files::WetPath(session, key), kSampleRate);
    Check(live.has_value() && !live->empty(), "the live wet sound is in the session folder");

    s.Send({{"type", "retoneLooperLayer"}, {"layerIds", "all"}});
    s.Settle(2500);
    const auto state = s.State();
    Check(!state.contains("notice"), "the re-tone was accepted: " + state.value("notice", std::string{}) + " " +
                                         state.value("noticeDetail", std::string{}));
    layers = host.Last("looperLayers")["layers"];
    Check(layers.size() == 1 && layers[0].value("wetSource", std::string{}) == "reamp" &&
              layers[0].value("canRevert", false) && !layers[0].contains("retone"),
          "the layer plays its rendered tone, and can go back: " + layers.dump().substr(0, 200));

    // The render through the same rig is what was played, once the loop's own tail is past.
    const auto rendered = looper_files::ReadWav(looper_files::WetPath(session, key), kSampleRate);
    Check(rendered.has_value() && !rendered->empty(), "the rendered wet sound replaced it in the session folder");

    if (live && rendered && !live->empty() && !rendered->empty())
    {
        const std::size_t length = (*live)[0].size();
        const auto [correlation, levelDb] = Compare((*live)[0], (*rendered)[0], length / 4, length);
        Check(correlation > 0.99 && std::abs(levelDb) < 0.5, "the re-amp matches the live take: correlation " +
                                                                 std::to_string(correlation) + ", level " +
                                                                 std::to_string(levelDb) + " dB");
    }

    s.Send({{"type", "retoneLooperLayer"},
            {"layerIds", nlohmann::json::array({layers[0].value("id", std::string{})})},
            {"revert", true}});
    s.Settle(300);
    layers = host.Last("looperLayers")["layers"];
    Check(layers.size() == 1 && layers[0].value("wetSource", std::string{}) == "live" &&
              !layers[0].value("canRevert", true),
          "the recorded tone comes back");

    // A riff take as a layer: this layer's DI saved as a riff, then brought back in.
    s.Send({{"type", "exportLooperLayer"}, {"layerId", layers[0].value("id", std::string{})}, {"as", "riff"}});
    s.Run(0.05);
    s.Send({{"type", "saveRiffTake"}, {"title", "Looped riff"}});
    s.Run(0.05);
    const std::string takeId = host.Last("riffSaved").value("takeId", std::string{});
    Check(!takeId.empty(), "the layer saved as a riff");
    s.Send({{"type", "addRiffAsLooperLayer"}, {"takeId", takeId}});
    s.Settle(2500);
    layers = host.Last("looperLayers")["layers"];
    Check(layers.size() == 2 && layers[0].value("title", std::string{}) == "Looped riff" &&
              layers[0].value("wetSource", std::string{}) == "reamp" &&
              layers[0].value("riffId", std::string{}) == takeId,
          "a riff becomes a layer, rendered through the rig: " + layers.dump().substr(0, 240));
    Check(s.State().value("canUndo", false), "and it can be undone");
}
} // namespace

int main()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-looper-retone-tests";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    test::SetSettingsEnvRoot(sandbox);
    RegisterAllEffects();

    TestRetone(sandbox);
    fs::remove_all(sandbox, ec);

    if (gFailures > 0)
    {
        std::cerr << gFailures << " check(s) failed" << std::endl;
        return 1;
    }

    std::cout << "LooperRetoneTests passed" << std::endl;
    return 0;
}
