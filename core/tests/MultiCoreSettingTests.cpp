// The multi-core processing setting: on by default in the standalone, off in a plugin, each host
// reading its own key from the shared store; and with it off, no helper thread runs any DSP.

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "IPluginHost.h"
#include "PluginController.h"
#include "dsp/RealtimeParallel.h"

namespace fs = std::filesystem;

namespace
{
class TestHost final : public guitarfx::IPluginHost
{
  public:
    TestHost(fs::path root, bool standalone) : mRoot(std::move(root)), mStandalone(standalone)
    {
    }

    void SendMessageToUI(const std::string&) override
    {
    }

    void BrowseFileAsync(guitarfx::BrowseFileType, const std::string&,
                         std::function<void(const guitarfx::BrowseFileResult&)> callback) override
    {
        callback(guitarfx::BrowseFileResult{});
    }

    void SaveFileAsync(guitarfx::BrowseFileType, const std::string&, const std::string&,
                       std::function<void(const guitarfx::BrowseFileResult&)> callback) override
    {
        callback(guitarfx::BrowseFileResult{});
    }

    void RunOnMainThread(std::function<void()> fn) override
    {
        fn();
    }

    [[nodiscard]] fs::path GetUserDataPath() const override
    {
        return mRoot;
    }

    [[nodiscard]] fs::path GetBundledAssetsPath() const override
    {
        return mRoot;
    }

    [[nodiscard]] double GetSampleRate() const override
    {
        return 48000.0;
    }

    [[nodiscard]] int GetBlockSize() const override
    {
        return 256;
    }

    [[nodiscard]] bool IsStandalone() const override
    {
        return mStandalone;
    }

  private:
    fs::path mRoot;
    bool mStandalone;
};

int gFailures = 0;

void Expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << what << std::endl;
        ++gFailures;
    }
}

/// A fresh profile per test, keyed on this run so a concurrent run of the same exe never shares one.
fs::path Sandbox(const std::string& name)
{
    static const auto run = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 100000000);
    const auto root = fs::temp_directory_path() / ("guitarfx-multicore-tests-" + run) / name;
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
#ifdef _WIN32
    _putenv_s("APPDATA", root.string().c_str());
#else
    setenv("HOME", root.string().c_str(), 1);
#endif
    return root;
}

void SetSetting(guitarfx::PluginController& controller, const std::string& key, bool value)
{
    controller.HandleUIMessage(nlohmann::json{{"type", "setSetting"}, {"key", key}, {"value", value}}.dump());
}

void TestDefaults()
{
    {
        TestHost host(Sandbox("standalone-default"), true);
        guitarfx::PluginController controller(host);
        controller.Initialize();
        Expect(controller.GetMixer().IsMultiThreadedProcessingEnabled() == guitarfx::rtparallel::kParallelDspSupported,
               "the standalone defaults to multi-core");
    }

    {
        TestHost host(Sandbox("plugin-default"), false);
        guitarfx::PluginController controller(host);
        controller.Initialize();
        Expect(!controller.GetMixer().IsMultiThreadedProcessingEnabled(), "a plugin defaults to single-core");
    }
}

void TestEachHostReadsItsOwnKey()
{
    // One store, as on a user's machine: the plugin key moves plugins only.
    const auto root = Sandbox("own-key");

    {
        TestHost host(root, false);
        guitarfx::PluginController controller(host);
        controller.Initialize();
        SetSetting(controller, "audio.processing.multiCore.standalone", false);
        Expect(!controller.GetMixer().IsMultiThreadedProcessingEnabled(), "a plugin ignores the standalone key");
        SetSetting(controller, "audio.processing.multiCore.plugin", true);
        Expect(controller.GetMixer().IsMultiThreadedProcessingEnabled() == guitarfx::rtparallel::kParallelDspSupported,
               "a plugin turns multi-core on from its own key");
    }

    {
        // The store now says plugin on, standalone off.
        TestHost host(root, true);
        guitarfx::PluginController controller(host);
        controller.Initialize();
        Expect(!controller.GetMixer().IsMultiThreadedProcessingEnabled(),
               "the standalone reads its own stored value, not the plugin's");
    }

    {
        TestHost host(root, false);
        guitarfx::PluginController controller(host);
        controller.Initialize();
        Expect(controller.GetMixer().IsMultiThreadedProcessingEnabled() == guitarfx::rtparallel::kParallelDspSupported,
               "a new plugin instance starts from the stored plugin value");
    }
}

void TestRefusedScopeKeepsTasksOnTheCaller()
{
    guitarfx::rtparallel::RealtimeTaskPool pool;
    pool.Start(3);

    if (!pool.HasWorkers())
    {
        return; // a platform without helpers has nothing to refuse
    }

    struct Context
    {
        std::thread::id caller = std::this_thread::get_id();
        std::atomic<int> elsewhere{0};
        std::atomic<int> ran{0};
    } context;

    const auto task = [](void* raw, int) {
        auto& ctx = *static_cast<Context*>(raw);

        if (std::this_thread::get_id() != ctx.caller)
        {
            ctx.elsewhere.fetch_add(1);
        }

        // Long enough that awake helpers would claim some of the tasks.
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        ctx.ran.fetch_add(1);
    };

    {
        const guitarfx::rtparallel::ScopedSharedHelpersRefused singleThreaded;
        pool.Run(task, &context, 32);
    }

    Expect(context.ran.load() == 32, "every task ran with helpers refused");
    Expect(context.elsewhere.load() == 0, "no task ran on a helper with helpers refused");
    pool.Stop();
}
} // namespace

int main()
{
    TestDefaults();
    TestEachHostReadsItsOwnKey();
    TestRefusedScopeKeepsTasksOnTheCaller();

    if (gFailures != 0)
    {
        std::cerr << gFailures << " multi-core setting check(s) failed" << std::endl;
        return 1;
    }

    std::cout << "Multi-core setting tests passed" << std::endl;
    return 0;
}
