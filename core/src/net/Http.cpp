#include "net/Http.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <set>
#include <thread>

namespace guitarfx::net
{
// ── CancelToken ─────────────────────────────────────────────────────────────────

void CancelToken::Cancel()
{
    std::function<void()> abort;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mCancelled.store(true);
        abort = mAbort;
    }
    // Outside the lock: an abort may wait on the transport, which may be clearing its own.
    if (abort)
        abort();
}

bool CancelToken::SetAbort(std::function<void()> abort)
{
    std::lock_guard<std::mutex> lock(mMutex);
    if (mCancelled.load())
        return false;
    mAbort = std::move(abort);
    return true;
}

void CancelToken::ClearAbort()
{
    std::lock_guard<std::mutex> lock(mMutex);
    mAbort = nullptr;
}

// ── HttpWorkers ─────────────────────────────────────────────────────────────────

struct HttpWorkers::Impl
{
    std::shared_ptr<HttpTransport> transport;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> jobs;
    std::set<std::shared_ptr<CancelToken>> inFlight;
    bool stopping = false;
    std::vector<std::thread> threads;

    void Loop()
    {
        for (;;)
        {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [this] { return stopping || !jobs.empty(); });
                if (stopping)
                    return;
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            job();
        }
    }
};

HttpWorkers::HttpWorkers(std::shared_ptr<HttpTransport> transport, int threads) : mImpl(std::make_unique<Impl>())
{
    mImpl->transport = std::move(transport);
    for (int i = 0; i < std::max(1, threads); ++i)
        mImpl->threads.emplace_back([impl = mImpl.get()] { impl->Loop(); });
}

HttpWorkers::~HttpWorkers()
{
    std::set<std::shared_ptr<CancelToken>> inFlight;
    {
        std::lock_guard<std::mutex> lock(mImpl->mutex);
        mImpl->stopping = true;
        mImpl->jobs.clear();
        inFlight = mImpl->inFlight;
    }
    // Every request in flight returns at once; a job that goes on to make another gets an
    // immediate "cancelled" (Perform checks `stopping`).
    for (const auto& token : inFlight)
        token->Cancel();
    mImpl->wake.notify_all();
    for (auto& thread : mImpl->threads)
        if (thread.joinable())
            thread.join();
}

void HttpWorkers::Run(std::function<void()> job)
{
    {
        std::lock_guard<std::mutex> lock(mImpl->mutex);
        if (mImpl->stopping)
            return;
        mImpl->jobs.push_back(std::move(job));
    }
    mImpl->wake.notify_one();
}

HttpResponse HttpWorkers::Perform(const HttpRequest& request)
{
    auto token = std::make_shared<CancelToken>();
    {
        std::lock_guard<std::mutex> lock(mImpl->mutex);
        if (mImpl->stopping || !mImpl->transport)
        {
            HttpResponse cut;
            cut.cancelled = true;
            cut.error = "Cancelled";
            return cut;
        }
        mImpl->inFlight.insert(token);
    }

    auto response = mImpl->transport->Perform(request, *token);

    {
        std::lock_guard<std::mutex> lock(mImpl->mutex);
        mImpl->inFlight.erase(token);
    }
    if (token->IsCancelled())
    {
        response.status = 0;
        response.cancelled = true;
        if (response.error.empty())
            response.error = "Cancelled";
    }
    return response;
}
} // namespace guitarfx::net
