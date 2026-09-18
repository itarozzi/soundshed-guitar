#pragma once

// HostStateRelay — answers a host's request for the state blob from any thread.
//
// Building the blob reads what the message thread owns and changes without a lock: the
// working copy of the focused preset, the mixer slot cache, settings, automation. It also
// asks each hosted plugin for its live state, and a hosted processor found under the DSP
// lock stays alive after the lock is released only on the message thread (see
// MultiPresetMixer::RecordNodeConfig): anywhere else, the message thread can retire and
// destroy it mid-read. So the blob is only ever built on the message thread.
//
// Hosts do not all ask there. JUCE's AAX wrapper keeps a per-thread chunk because Pro Tools
// asks from several threads, an AU's ClassInfo can be read on any thread, JUCE runs its own
// message thread under an LV2 host on Linux, and a host is free to ignore the VST3 and CLAP
// threading rules. A request from another thread is posted to the message thread and waited
// for. The wait is bounded, because the message thread may itself be waiting on the thread
// that asked; then, and whenever the message thread is too busy to start on it in time, the
// request is answered with the most recent blob the message thread built instead.
//
// That fallback is refreshed by every build on the message thread, and by the controller
// whenever the working copy changes (see TakeRefreshDue), so a host that can only ever be
// answered from it still saves current state.

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace guitarfx
{
class IPluginHost;

class HostStateRelay
{
  public:
    using BuildFn = std::function<std::string()>;

    /// How long a request from another thread waits for the message thread to start on it.
    /// Long enough to ride out ordinary message-thread work; short, because a host that is
    /// holding its message thread while it waits for the answer is frozen for this long.
    static constexpr std::chrono::milliseconds kDefaultOffThreadWait{1000};

    /// The most often a change to the working copy rebuilds the fallback.
    static constexpr std::chrono::milliseconds kDefaultRefreshInterval{1000};

    explicit HostStateRelay(IPluginHost& host, std::chrono::milliseconds offThreadWait = kDefaultOffThreadWait,
                            std::chrono::milliseconds refreshInterval = kDefaultRefreshInterval);

    HostStateRelay(const HostStateRelay&) = delete;
    HostStateRelay& operator=(const HostStateRelay&) = delete;

    /// Any thread but the message thread. Posts `build` to the message thread and returns what
    /// it built, or Remembered() if the message thread has not started on it within the wait.
    /// Once started it is waited for: the message thread is working, not blocked. A request
    /// that was given up on is dropped when the message thread reaches it, without calling
    /// `build`, so `build` may capture objects that do not outlive the caller.
    [[nodiscard]] std::string BuildOnMessageThread(BuildFn build) const;

    /// Message thread: `state` is current. It becomes the answer for requests the message
    /// thread cannot take, and clears any pending refresh.
    void Remember(std::string state);

    /// Any thread: the blob last passed to Remember(), or empty if there has been none.
    [[nodiscard]] std::string Remembered() const;

    /// Any thread: something the blob carries has changed since it was remembered.
    void MarkStale();

    /// Message thread: true when the remembered blob is stale and was remembered at least the
    /// refresh interval ago. The caller then builds and Remember()s a new one.
    [[nodiscard]] bool TakeRefreshDue(std::chrono::steady_clock::time_point now);

  private:
    IPluginHost& mHost;
    const std::chrono::milliseconds mOffThreadWait;
    const std::chrono::milliseconds mRefreshInterval;

    mutable std::mutex mRememberedMutex;
    std::shared_ptr<const std::string> mRemembered;

    std::atomic<bool> mStale{false};
    std::chrono::steady_clock::time_point mRememberedAt{}; // message thread only
};
} // namespace guitarfx
