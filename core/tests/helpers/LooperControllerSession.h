#pragma once

// A PluginController on a standalone test host, driven as the UI drives the looper: messages in,
// blocks of a tone (or silence) through ProcessAudio, idle ticks, and the looper messages kept.
// Include it in one translation unit of a test executable (it defines the failure counter).

#include "PluginController.h"

#include "MessageThreadTestHost.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace looper_session
{
using namespace guitarfx;

inline constexpr double kSampleRate = 48000.0;
inline constexpr int kBlock = 128;

inline int gFailures = 0;

inline void Check(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << what << std::endl;
        ++gFailures;
    }
}

/// A standalone host that keeps the looper messages it is sent.
class LooperHost : public test::PumpedTestHost
{
  public:
    using test::PumpedTestHost::PumpedTestHost;

    [[nodiscard]] bool IsStandalone() const override
    {
        return true;
    }

    void SendMessageToUI(const std::string& message) override
    {
        const auto parsed = nlohmann::json::parse(message, nullptr, false);

        if (!parsed.is_object())
        {
            return;
        }

        const std::lock_guard<std::mutex> lock(mMutex);
        const std::string type = parsed.value("type", std::string());

        if (type.rfind("looper", 0) == 0 || type.rfind("riff", 0) == 0)
        {
            mLast[type] = parsed;
            ++mCounts[type];
        }

        if (type == "state" || type == "metronomeState" || parsed.contains("metronome"))
        {
            mLastBroadcast = parsed;
        }
    }

    nlohmann::json Last(const std::string& type)
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        return mLast.count(type) ? mLast[type] : nlohmann::json{};
    }

    int Count(const std::string& type)
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        return mCounts[type];
    }

  private:
    std::mutex mMutex;
    std::map<std::string, nlohmann::json> mLast;
    std::map<std::string, int> mCounts;
    nlohmann::json mLastBroadcast;
};

struct Session
{
    LooperHost& host;
    PluginController& controller;
    bool playing = false;
    std::size_t phase = 0;
    /// When set, the left output of every block is appended here (sample i is input sample i).
    std::vector<float>* capture = nullptr;

    void Send(const nlohmann::json& message)
    {
        controller.HandleUIMessage(message.dump());
    }

    /// `seconds` of blocks, an idle tick every four.
    void Run(double seconds)
    {
        std::vector<float> l(kBlock);
        std::vector<float> r(kBlock);
        std::vector<float> ol(kBlock);
        std::vector<float> orr(kBlock);
        const int blocks = static_cast<int>(std::ceil(seconds * kSampleRate / kBlock));

        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < kBlock; ++i, ++phase)
            {
                l[static_cast<std::size_t>(i)] =
                    playing ? static_cast<float>(0.3 * std::sin(static_cast<double>(phase) * 0.03)) : 0.0f;
                r[static_cast<std::size_t>(i)] = l[static_cast<std::size_t>(i)];
            }

            float* inputs[2] = {l.data(), r.data()};
            float* outputs[2] = {ol.data(), orr.data()};
            controller.ProcessAudio(inputs, outputs, kBlock);

            if (capture)
            {
                capture->insert(capture->end(), ol.begin(), ol.end());
            }

            if (b % 4 == 3)
            {
                controller.OnIdle();
                host.Pump();
            }
        }

        controller.OnIdle();
        host.Pump();
    }

    /// Idle ticks over `milliseconds` of real time, so the looper's worker can write and read files.
    void Settle(int milliseconds = 600)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);

        while (std::chrono::steady_clock::now() < until)
        {
            Run(0.01);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    nlohmann::json State()
    {
        return host.Last("looperState");
    }

    void Command(const std::string& command)
    {
        Send({{"type", "looperCommand"}, {"command", command}});
    }
};
} // namespace looper_session
