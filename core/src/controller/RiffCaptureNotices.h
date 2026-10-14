#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace guitarfx
{
/// What riff capture's audio-thread half has to tell the UI: that the take started, how far it
/// has got, and that it filled up. The audio thread stores the numbers that go with a notice and
/// raises it; PluginController::PublishRiffCaptureNotices() takes it on the message thread and
/// builds the message there, so the audio thread builds no JSON and allocates nothing for it. A
/// notice raised again before it is taken goes out once, with the latest numbers.
struct RiffCaptureNotices
{
    static constexpr std::uint32_t kStarted = 1u << 0;
    static constexpr std::uint32_t kProgress = 1u << 1;
    static constexpr std::uint32_t kStopped = 1u << 2;

    std::atomic<std::uint32_t> pending{0};
    std::atomic<std::size_t> barAlignOffsetSamples{0};
    std::atomic<std::size_t> capturedSamples{0};
    std::atomic<int> stoppedBars{0};

    RiffCaptureNotices() = default;

    /// A capture is moved only while it is built or imported, before the audio thread has seen
    /// it, so the moved-to notices start with nothing pending.
    RiffCaptureNotices(RiffCaptureNotices&&) noexcept
    {
    }

    RiffCaptureNotices& operator=(RiffCaptureNotices&&) noexcept
    {
        return *this;
    }

    RiffCaptureNotices(const RiffCaptureNotices&) = delete;
    RiffCaptureNotices& operator=(const RiffCaptureNotices&) = delete;

    /// Audio thread, after the notice's numbers are stored.
    void Raise(std::uint32_t notice) noexcept
    {
        pending.fetch_or(notice, std::memory_order_release);
    }

    /// Message thread: every notice raised since the last call.
    [[nodiscard]] std::uint32_t Take() noexcept
    {
        return pending.exchange(0, std::memory_order_acquire);
    }
};
} // namespace guitarfx
