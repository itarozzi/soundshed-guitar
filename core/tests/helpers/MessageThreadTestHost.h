#pragma once

// Scaffolding for tests that call the controller from threads other than the message thread.
//
// PumpedTestHost posts RunOnMainThread() to a queue, as JUCE's callAsync does, and the test's
// own thread plays the message thread by pumping it between the things it does. Tests derive
// from it to record what the controller tells the host. AudioThread calls ProcessAudio back to
// back on a thread of its own, the way a host's audio callback does.

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "IPluginHost.h"
#include "PluginController.h"

namespace guitarfx::test
{
/// Points the settings root at `root`, so a test never touches the real profile.
inline void SetSettingsEnvRoot(const std::filesystem::path& root)
{
#ifdef _WIN32
    _putenv_s("APPDATA", root.string().c_str());
#else
    setenv("HOME", root.string().c_str(), 1);
#endif
}

class PumpedTestHost : public IPluginHost
{
  public:
    PumpedTestHost(std::filesystem::path userDataPath, std::thread::id messageThread, double sampleRate, int blockSize)
        : mUserDataPath(std::move(userDataPath)), mMessageThread(messageThread), mSampleRate(sampleRate),
          mBlockSize(blockSize)
    {
    }

    void SendMessageToUI(const std::string&) override
    {
    }

    void BrowseFileAsync(BrowseFileType, const std::string&,
                         std::function<void(const BrowseFileResult&)> callback) override
    {
        callback(BrowseFileResult{});
    }

    void SaveFileAsync(BrowseFileType, const std::string&, const std::string&,
                       std::function<void(const BrowseFileResult&)> callback) override
    {
        callback(BrowseFileResult{});
    }

    /// Posts, as JUCE's callAsync does, even from the message thread itself.
    void RunOnMainThread(std::function<void()> fn) override
    {
        const std::lock_guard<std::mutex> lock(mQueueMutex);
        mQueue.push_back(std::move(fn));
    }

    [[nodiscard]] bool IsMessageThread() const override
    {
        return std::this_thread::get_id() == mMessageThread;
    }

    [[nodiscard]] std::filesystem::path GetUserDataPath() const override
    {
        return mUserDataPath;
    }

    [[nodiscard]] std::filesystem::path GetBundledAssetsPath() const override
    {
        return mUserDataPath;
    }

    [[nodiscard]] double GetSampleRate() const override
    {
        return mSampleRate;
    }

    [[nodiscard]] int GetBlockSize() const override
    {
        return mBlockSize;
    }

    /// Message thread: runs everything posted so far, and returns how many that was.
    int Pump()
    {
        std::deque<std::function<void()>> tasks;
        {
            const std::lock_guard<std::mutex> lock(mQueueMutex);
            tasks.swap(mQueue);
        }

        for (auto& task : tasks)
        {
            task();
        }

        return static_cast<int>(tasks.size());
    }

  private:
    std::filesystem::path mUserDataPath;
    std::thread::id mMessageThread;
    double mSampleRate;
    int mBlockSize;
    std::mutex mQueueMutex;
    std::deque<std::function<void()>> mQueue;
};

/// Calls ProcessAudio back to back on its own thread, with a short gap so the message thread's
/// blocking lock is not starved by the try_lock. Stop it before the controller goes.
class AudioThread
{
  public:
    AudioThread(PluginController& controller, int blockSize) : mController(controller), mBlockSize(blockSize)
    {
        mThread = std::thread([this] { Run(); });
    }

    ~AudioThread()
    {
        mStop.store(true, std::memory_order_release);
        mThread.join();
    }

    AudioThread(const AudioThread&) = delete;
    AudioThread& operator=(const AudioThread&) = delete;

    /// Blocks until the mixer has processed `blocks` more blocks; false on timeout.
    [[nodiscard]] bool WaitForBlocks(int blocks, std::chrono::milliseconds timeout = std::chrono::seconds(10)) const
    {
        const int from = mProcessed.load(std::memory_order_acquire);
        const auto deadline = std::chrono::steady_clock::now() + timeout;

        while (mProcessed.load(std::memory_order_acquire) - from < blocks)
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return false;
            }

            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }

        return true;
    }

  private:
    void Run()
    {
        const auto size = static_cast<std::size_t>(mBlockSize);
        std::vector<float> inL(size, 0.05f), inR(size, 0.05f), outL(size), outR(size);

        while (!mStop.load(std::memory_order_acquire))
        {
            float* inputs[] = {inL.data(), inR.data()};
            float* outputs[] = {outL.data(), outR.data()};

            if (mController.ProcessAudio(inputs, outputs, mBlockSize))
            {
                mProcessed.fetch_add(1, std::memory_order_acq_rel);
            }

            const auto resume = std::chrono::steady_clock::now() + std::chrono::microseconds(200);

            while (std::chrono::steady_clock::now() < resume)
            {
                std::this_thread::yield();
            }
        }
    }

    PluginController& mController;
    int mBlockSize;
    std::thread mThread;
    std::atomic<bool> mStop{false};
    std::atomic<int> mProcessed{0};
};
} // namespace guitarfx::test
