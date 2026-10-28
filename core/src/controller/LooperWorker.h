#pragma once

// LooperWorker — one low-priority thread for the looper's slow work: writing the session folder,
// saving and loading projects, re-tone renders. Jobs run one at a time in the order they were
// posted, so a save posted after a layer's write finds the layer on disk. A job hands its result
// back with Deliver(); the message thread runs what was delivered from its idle tick (RunDelivered),
// so nothing the service owns is touched from the worker.

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace guitarfx
{
class LooperWorker
{
  public:
    using Job = std::function<void()>;

    LooperWorker() : mThread([this] { Run(); })
    {
    }

    ~LooperWorker()
    {
        Stop();
    }

    LooperWorker(const LooperWorker&) = delete;
    LooperWorker& operator=(const LooperWorker&) = delete;

    /// Message thread.
    void Post(Job job)
    {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mJobs.push_back(std::move(job));
        }

        mWake.notify_one();
    }

    /// Worker: queue `result` for the message thread.
    void Deliver(Job result)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mDelivered.push_back(std::move(result));
    }

    /// Message thread: run what jobs delivered since the last call.
    void RunDelivered()
    {
        std::vector<Job> results;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            results.swap(mDelivered);
        }

        for (auto& result : results)
        {
            result();
        }
    }

    /// True while a job is queued or running.
    [[nodiscard]] bool Busy()
    {
        std::lock_guard<std::mutex> lock(mMutex);
        return mRunning || !mJobs.empty();
    }

    /// Finishes the job in hand, drops the rest, and joins. Results not yet run are dropped too.
    void Stop()
    {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mStopping = true;
            mJobs.clear();
        }

        mWake.notify_one();

        if (mThread.joinable())
        {
            mThread.join();
        }
    }

  private:
    void Run()
    {
        for (;;)
        {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mMutex);
                mWake.wait(lock, [this] { return mStopping || !mJobs.empty(); });

                if (mStopping)
                {
                    return;
                }

                job = std::move(mJobs.front());
                mJobs.pop_front();
                mRunning = true;
            }

            job();
            std::lock_guard<std::mutex> lock(mMutex);
            mRunning = false;
        }
    }

    std::mutex mMutex;
    std::condition_variable mWake;
    std::deque<Job> mJobs;
    std::vector<Job> mDelivered;
    bool mStopping = false;
    bool mRunning = false;
    std::thread mThread;
};
} // namespace guitarfx
