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
//
// Restores come the same way, and so does a program change, which loads a preset. They are
// queued for the message thread and waited for as a save is. There is nothing to fall back to,
// though, so one the message thread has not started on in time is not given up on: the caller
// returns and it stays queued, to be applied in the order the host asked. A restore also
// becomes the fallback answer for saves until then, since it is the state the host now expects
// back. The message thread applies anything queued before it reads or replaces what that
// changes (see ApplyQueued), so a host's calls take effect in the order it made them.

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace guitarfx
{
class IPluginHost;

class HostStateRelay
{
  public:
    using BuildFn = std::function<std::string()>;
    using ChangeFn = std::function<void()>;
    /// Runs on the message thread after the change, once the thread that queued it has been
    /// let go. `callerWaited` is false when that thread stopped waiting before it started.
    using AfterChangeFn = std::function<void(bool callerWaited)>;

    /// How long a request from another thread waits for the message thread to start on it.
    /// Long enough to ride out ordinary message-thread work; short, because a host that is
    /// holding its message thread while it waits for the answer is frozen for this long.
    static constexpr std::chrono::milliseconds kDefaultOffThreadWait{1000};

    /// The most often a change to the working copy rebuilds the fallback.
    static constexpr std::chrono::milliseconds kDefaultRefreshInterval{1000};

    explicit HostStateRelay(IPluginHost& host, std::chrono::milliseconds offThreadWait = kDefaultOffThreadWait,
                            std::chrono::milliseconds refreshInterval = kDefaultRefreshInterval);
    ~HostStateRelay();

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

    /// Any thread but the message thread. Queues `change` and waits the same way
    /// BuildOnMessageThread does: for the message thread to start on it, then for it to finish.
    /// True once it has run. False if it had not started within the wait; it then stays queued
    /// and runs when the message thread gets to it, unless Shutdown() comes first.
    ///
    /// `after` follows it on the message thread once the caller has been let go, so it is the
    /// place for anything that calls into the host: the caller may hold a lock the host takes.
    /// `pendingState`, when given, is remembered as the answer for saves in the meantime.
    [[nodiscard]] bool ApplyOnMessageThread(ChangeFn change, AfterChangeFn after,
                                            std::optional<std::string> pendingState = std::nullopt);

    /// Message thread: runs every queued change, oldest first. Call it before reading or
    /// replacing what they change, so they take effect before whatever the host asked for
    /// next. Returns at once when called from inside a change.
    void ApplyQueued();

    /// Any thread: drops the changes still queued and waits for one that is running. None runs
    /// after this returns, so the owner calls it before anything a change uses is torn down: a
    /// change whose caller gave up can outlive the call that queued it.
    void Shutdown();

  private:
    struct ChangeQueue;

    /// Replaces the remembered blob without touching the refresh bookkeeping. Any thread.
    void SetRemembered(std::string state);

    IPluginHost& mHost;
    const std::chrono::milliseconds mOffThreadWait;
    const std::chrono::milliseconds mRefreshInterval;

    mutable std::mutex mRememberedMutex;
    std::shared_ptr<const std::string> mRemembered;

    std::atomic<bool> mStale{false};
    std::chrono::steady_clock::time_point mRememberedAt{}; // message thread only

    /// Shared with the tasks posted to the message thread, which can outlive the relay.
    std::shared_ptr<ChangeQueue> mChanges;
};
} // namespace guitarfx
