#include "controller/HostStateRelay.h"

#include "IPluginHost.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <optional>
#include <thread>
#include <utility>

namespace guitarfx
{
/// Changes waiting for the message thread, in the order they were asked for. Changes run in
/// that order, so one ticket number says how far the message thread has got.
struct HostStateRelay::ChangeQueue
{
    struct Entry
    {
        std::uint64_t ticket = 0;
        ChangeFn change;
        AfterChangeFn after;
        bool callerGaveUp = false;
    };

    std::mutex mutex;
    std::condition_variable progressed;
    std::deque<Entry> queued;
    std::uint64_t lastQueued = 0;
    std::uint64_t lastStarted = 0;
    std::uint64_t lastFinished = 0;
    std::thread::id applyingOn{}; ///< the thread inside Apply(); none when idle
    bool shutDown = false;

    /// Runs what is queued, oldest first, until nothing is left. Only the message thread runs
    /// anything; called anywhere else, it leaves the queue for the message thread.
    void Apply(IPluginHost& host)
    {
        {
            const std::lock_guard<std::mutex> lock(mutex);

            // Already inside Apply() (a change that reaches something which applies the queue):
            // the outer call gets to whatever is queued behind it.
            if (shutDown || applyingOn != std::thread::id{} || queued.empty())
            {
                return;
            }

            // Shutdown() cannot get past this lock, so the host is still alive here. A host that
            // ran the task somewhere else has not moved it to the message thread.
            if (!host.IsMessageThread())
            {
                return;
            }

            // From here Shutdown() waits for applyingOn to clear, so the host and whatever the
            // changes capture stay alive until then.
            applyingOn = std::this_thread::get_id();
        }

        for (;;)
        {
            Entry entry;
            {
                const std::lock_guard<std::mutex> lock(mutex);

                if (shutDown || queued.empty())
                {
                    applyingOn = std::thread::id{};
                    progressed.notify_all();
                    return;
                }

                entry = std::move(queued.front());
                queued.pop_front();
                lastStarted = entry.ticket;
            }

            progressed.notify_all();

            // Anything at all: nothing may escape into the host's message loop, and applyingOn
            // has to clear or Shutdown() waits for ever.
            try
            {
                entry.change();
            }
            catch (...)
            {
            }

            {
                const std::lock_guard<std::mutex> lock(mutex);
                lastFinished = entry.ticket;
            }

            progressed.notify_all();

            if (entry.after)
            {
                try
                {
                    entry.after(!entry.callerGaveUp);
                }
                catch (...)
                {
                }
            }
        }
    }
};

namespace
{
/// One request from another thread, shared by the caller and the message thread's task so
/// that either may be the last to let go of it.
struct Request
{
    enum class Stage
    {
        Queued,    ///< posted; the message thread has not reached it
        Running,   ///< the message thread is building
        Finished,  ///< built, or declined
        Abandoned, ///< the caller stopped waiting before it started
    };

    std::mutex mutex;
    std::condition_variable finished;
    Stage stage = Stage::Queued;
    std::optional<std::string> result;
};
} // namespace

HostStateRelay::HostStateRelay(IPluginHost& host, std::chrono::milliseconds offThreadWait,
                               std::chrono::milliseconds refreshInterval)
    : mHost(host), mOffThreadWait(offThreadWait), mRefreshInterval(refreshInterval),
      mChanges(std::make_shared<ChangeQueue>())
{
}

HostStateRelay::~HostStateRelay()
{
    Shutdown();
}

std::string HostStateRelay::BuildOnMessageThread(BuildFn build) const
{
    auto request = std::make_shared<Request>();

    // The task touches nothing but `request` until it has seen that the caller is still
    // waiting, and the caller is inside the controller, which the host owns: past that check
    // `host` and whatever `build` captured are alive.
    mHost.RunOnMainThread([request, &host = mHost, build = std::move(build)]() {
        {
            const std::lock_guard<std::mutex> lock(request->mutex);

            if (request->stage != Request::Stage::Queued)
            {
                return;
            }

            request->stage = Request::Stage::Running;
        }

        std::optional<std::string> result;

        // A host that runs the task on the calling thread has not moved it anywhere, and
        // `build` would only come straight back here.
        if (host.IsMessageThread())
        {
            try
            {
                result = build();
            }
            catch (const std::exception&)
            {
                // Declined: the caller falls back to the remembered blob. Nothing may escape
                // into the host's message loop.
            }
        }

        {
            const std::lock_guard<std::mutex> lock(request->mutex);
            request->result = std::move(result);
            request->stage = Request::Stage::Finished;
        }

        request->finished.notify_all();
    });

    {
        std::unique_lock<std::mutex> lock(request->mutex);
        const bool started = request->finished.wait_for(
            lock, mOffThreadWait, [&request] { return request->stage != Request::Stage::Queued; });

        if (started)
        {
            request->finished.wait(lock, [&request] { return request->stage == Request::Stage::Finished; });

            if (request->result.has_value())
            {
                return std::move(*request->result);
            }
        }
        else
        {
            request->stage = Request::Stage::Abandoned;
        }
    }

    return Remembered();
}

void HostStateRelay::Remember(std::string state)
{
    SetRemembered(std::move(state));
    mStale.store(false, std::memory_order_release);
    mRememberedAt = std::chrono::steady_clock::now();
}

void HostStateRelay::SetRemembered(std::string state)
{
    auto remembered = std::make_shared<const std::string>(std::move(state));
    {
        const std::lock_guard<std::mutex> lock(mRememberedMutex);
        mRemembered.swap(remembered);
    }
    // The blob it replaced is freed here, outside the lock.
}

std::string HostStateRelay::Remembered() const
{
    std::shared_ptr<const std::string> remembered;
    {
        const std::lock_guard<std::mutex> lock(mRememberedMutex);
        remembered = mRemembered;
    }

    return remembered ? *remembered : std::string{};
}

void HostStateRelay::MarkStale()
{
    mStale.store(true, std::memory_order_release);
}

bool HostStateRelay::TakeRefreshDue(std::chrono::steady_clock::time_point now)
{
    if (!mStale.load(std::memory_order_acquire) || now - mRememberedAt < mRefreshInterval)
    {
        return false;
    }

    // Cleared here rather than by the Remember() that follows, so a build that throws is
    // retried on the next change rather than on every idle tick.
    mStale.store(false, std::memory_order_release);
    return true;
}

bool HostStateRelay::ApplyOnMessageThread(ChangeFn change, AfterChangeFn after, std::optional<std::string> pendingState)
{
    const auto queue = mChanges;
    std::uint64_t ticket = 0;
    {
        const std::lock_guard<std::mutex> lock(queue->mutex);

        if (queue->shutDown)
        {
            return false;
        }

        ticket = ++queue->lastQueued;
        queue->queued.push_back({ticket, std::move(change), std::move(after), false});

        // Under the queue's lock, so that of two restores queued at once from different
        // threads, the one remembered is the one that will be applied last.
        if (pendingState.has_value())
        {
            SetRemembered(std::move(*pendingState));
        }
    }

    // The task holds the queue, not the relay, and touches the host only once Apply() has
    // seen that Shutdown() has not been called.
    mHost.RunOnMainThread([queue, &host = mHost] { queue->Apply(host); });

    std::unique_lock<std::mutex> lock(queue->mutex);
    const auto startedOrClosed = [&queue, ticket] { return queue->lastStarted >= ticket || queue->shutDown; };

    if (!queue->progressed.wait_for(lock, mOffThreadWait, startedOrClosed) || queue->lastStarted < ticket)
    {
        // Left queued, or dropped by Shutdown(). Either way the caller goes, and the change is
        // told so when it does run, so that it can let the host know then.
        for (auto& entry : queue->queued)
        {
            if (entry.ticket == ticket)
            {
                entry.callerGaveUp = true;
                break;
            }
        }

        return false;
    }

    // Started: the message thread is working on it rather than blocked. It must not call into
    // the host before `after`, when this thread has been let go (see the declaration).
    queue->progressed.wait(lock, [&queue, ticket] { return queue->lastFinished >= ticket; });
    return true;
}

void HostStateRelay::ApplyQueued()
{
    mChanges->Apply(mHost);
}

void HostStateRelay::Shutdown()
{
    std::deque<ChangeQueue::Entry> dropped;
    {
        std::unique_lock<std::mutex> lock(mChanges->mutex);
        mChanges->shutDown = true;
        dropped.swap(mChanges->queued);
        mChanges->progressed.notify_all();

        // A change running on another thread is using what the owner is about to tear down.
        // One running on this thread is the caller's own concern.
        mChanges->progressed.wait(lock, [this] {
            return mChanges->applyingOn == std::thread::id{} || mChanges->applyingOn == std::this_thread::get_id();
        });
    }
    // The dropped changes' captures are released here, outside the lock.
}
} // namespace guitarfx
