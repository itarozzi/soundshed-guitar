#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <string>

namespace guitarfx
{
class PluginController;
}

namespace mixer_instance_lock_test
{
bool Check(bool condition, const std::string& what);
bool WaitUntil(const std::function<bool()>& condition, std::chrono::milliseconds timeout);
bool WaitForAdvance(const std::atomic<int>& counter, int from, int by, std::chrono::milliseconds timeout);
bool TestRiffTrimKeepsAudioRunning(guitarfx::PluginController& controller, const std::atomic<int>& audioBlocks,
                                   const std::atomic<int>& audioLockMisses, double sampleRate);
} // namespace mixer_instance_lock_test
