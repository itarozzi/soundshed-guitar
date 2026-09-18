#include "controller/HostStateRelay.h"

#include "IPluginHost.h"

#include <condition_variable>
#include <exception>
#include <optional>
#include <utility>

namespace guitarfx
{
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
    : mHost(host), mOffThreadWait(offThreadWait), mRefreshInterval(refreshInterval)
{
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
    auto remembered = std::make_shared<const std::string>(std::move(state));
    {
        const std::lock_guard<std::mutex> lock(mRememberedMutex);
        mRemembered.swap(remembered);
    }
    // The blob it replaced is freed here, outside the lock.

    mStale.store(false, std::memory_order_release);
    mRememberedAt = std::chrono::steady_clock::now();
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
} // namespace guitarfx
