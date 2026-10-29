#pragma once

// GateLearnService — the noise gate's Learn, from the UI's button to the threshold it suggests.
//
// The measuring happens on the audio thread, inside the gate (GateNoiseLearner). This starts a
// listen on one gate — the global gate or a node in a preset — then polls it from the idle loop
// and reports progress, and finally the noise floor and the threshold it suggests, as
// "gateLearnStatus". A gate is any effect that is a NoiseLearnHost: the noise gate, with one
// detector, or the Multi-band Gate, with one per band, whose status carries a `bands` array. It never sets the
// threshold itself: the UI applies it as it would a turned knob, so the change reaches the preset, undo and the host
// like any other edit.
//
// The live gate is looked up afresh under the DSP lock on every poll and never held between
// them, since an edit elsewhere in the chain can rebuild it. A rebuilt gate has forgotten the
// listen, so the poll starts it again there. While a listen runs the mixer is held awake, or the
// quiet it is listening to would put the chains to sleep and the gate would hear nothing.

#include <chrono>
#include <functional>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

namespace guitarfx
{
class MessageHandlerRegistry;
class MultiPresetMixer;
class EffectProcessor;
class NoiseLearnHost;

class GateLearnService
{
  public:
    using SendMessageFn = std::function<void(const std::string&)>;
    /// The mixer id of the preset being edited: where a "preset" request with no presetId goes,
    /// as node edits do.
    using ActivePresetIdFn = std::function<std::string()>;

    GateLearnService(SendMessageFn sendMessage, MultiPresetMixer& mixer, std::mutex& dspMutex,
                     ActivePresetIdFn activePresetId);

    /// Registers "gateLearn" with the dispatcher.
    void RegisterMessageHandlers(MessageHandlerRegistry& registry);

    /// Message thread: follows the listen in progress, if there is one.
    void OnIdle();

    [[nodiscard]] bool IsListening() const
    {
        return mActive;
    }

  private:
    struct Target
    {
        /// "global" for the global gate, "preset" for a node in a preset.
        std::string scope;
        std::string presetId;
        std::string nodeId;
    };

    /// "gateLearn": {action: "start"|"cancel", scope, nodeId, presetId}.
    void HandleRequest(const nlohmann::json& payload);
    void Start(Target target);
    void Cancel();
    /// The gate the target names, in the running chain. Call with the DSP lock held.
    [[nodiscard]] NoiseLearnHost* FindGate() const;
    /// Starts every band of `gate` listening under the current session.
    void StartListening(NoiseLearnHost& gate) const;
    void Finish();
    void Send(const std::string& state, const nlohmann::json& extra = nlohmann::json::object()) const;

    SendMessageFn mSendMessage;
    ActivePresetIdFn mActivePresetId;
    MultiPresetMixer& mMixer;
    std::mutex& mDSPMutex;

    bool mActive = false;
    int mSession = 0;
    Target mTarget;
    std::chrono::steady_clock::time_point mStarted{};
    std::chrono::steady_clock::time_point mLastProgressSent{};
};
} // namespace guitarfx
