#pragma once

// The iOS app's CLAP host: the smallest one Nano Q's plugin needs, around the plugin that is
// linked into the app (the clap_entry nanoq/src/clap_entry.cpp defines). It does what
// clap-wrapper's standalone does on the desktop, with the audio left to AudioIO:
//
//   main thread   Load, Activate, Deactivate, AttachView, ResizeView, Unload; on_main_thread
//                 whenever the plugin asks for a callback
//   audio thread  Process, from the RemoteIO render callback
//
// See docs/plans/nano-q-ios.md.

#include <clap/clap.h>

#include <atomic>
#include <cstdint>
#include <memory>

namespace nanoq::ios
{
class ClapHost
{
public:
    ClapHost();
    ~ClapHost();

    ClapHost(const ClapHost&) = delete;
    ClapHost& operator=(const ClapHost&) = delete;

    /// Initialises the entry, creates the plugin and inits it.
    bool Load();
    void Unload();

    /// The plugin's processing state follows the audio unit's: activated before the unit
    /// starts, deactivated once it has stopped, so Process never runs on an inactive plugin.
    bool Activate(double sampleRate, uint32_t maxFrames);
    void Deactivate();
    bool IsActive() const
    {
        return mActive;
    }

    /// Audio thread. `in` and `out` are two channels of `frames` samples each; `in` may be
    /// null (no input), and is then silence.
    void Process(const float* const* in, float* const* out, uint32_t frames);

    /// The plugin's state (clap.state), kept in a file between runs as clap-wrapper's desktop
    /// standalone keeps it: the engine's own data is in its profile, and this is the rest of
    /// what a host would save in a project (the active preset, the host parameters).
    bool SaveState(const char* path);
    bool LoadState(const char* path);

    /// Creates the editor and puts its view inside `parent` (a UIView*), filling it.
    bool AttachView(void* parent, uint32_t width, uint32_t height);
    void ResizeView(uint32_t width, uint32_t height);
    void DetachView();

private:
    static const void* GetExtension(const clap_host_t* host, const char* id);
    static void RequestRestart(const clap_host_t* host);
    static void RequestProcess(const clap_host_t* host);
    static void RequestCallback(const clap_host_t* host);

    clap_host_t mHost{};
    const clap_plugin_entry_t* mEntry = nullptr;
    const clap_plugin_t* mPlugin = nullptr;
    const clap_plugin_gui_t* mGui = nullptr;
    const clap_plugin_state_t* mState = nullptr;
    bool mGuiCreated = false;
    bool mActive = false;

    // Whether start_processing has been called on the audio thread since activation.
    bool mProcessing = false;
    uint64_t mSteadyTime = 0;

    // Set when a main-thread callback is queued, so a busy plugin queues one, not hundreds.
    std::atomic<bool> mCallbackQueued{false};
    // Cleared by Unload, so a callback queued before it finds nothing to call. Shared with the
    // queued blocks, which may outlive the host.
    std::shared_ptr<std::atomic<bool>> mAlive = std::make_shared<std::atomic<bool>>(true);
};
} // namespace nanoq::ios
