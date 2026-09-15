#pragma once

// ControllerDisplayFeed — puts the active preset's name on the display of the MIDI
// controller the app is connected to, as SysEx.
//
// The message is the Soundshed Go control surface's SET_TEXT (soundshed-go,
// docs/04-config-protocol.md):
//
//     F0 7D 53 47 07 <line> <up to 32 ASCII bytes> F7
//
// 7D is the manufacturer id reserved for non-commercial use and 53 47 is "SG", so any
// other device ignores the message. The controller answers with a RESULT SysEx, which
// the host adapter already drops on the way in. The device does not persist the text,
// so a controller that has been power-cycled shows nothing until the name is sent
// again — which is why a (re)started audio stream asks for a refresh.
//
// Threads. Update() runs on the message thread once per idle tick and queues a message
// only when the name changed, the feed was switched back on, or a refresh was asked
// for. The audio thread collects it with TakeSysEx() and adds it to the block's MIDI
// output. Only the newest message is kept: a name that changes twice before the audio
// thread looks goes out once, as the second name. TakeSysEx() never blocks or
// allocates — it try-locks, and leaves the message for the next block if the message
// thread is in the middle of its copy.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace guitarfx
{
class ControllerDisplayFeed
{
  public:
    /// Characters the controller shows on one line; longer text is cut.
    static constexpr std::size_t kMaxTextLength = 32;
    /// F0 7D 53 47 07, the line, the text, F7.
    static constexpr std::size_t kMaxSysExBytes = 5 + 1 + kMaxTextLength + 1;
    /// The display line the preset name is written to.
    static constexpr std::uint8_t kPresetNameLine = 0;

    using SysExBuffer = std::array<std::uint8_t, kMaxSysExBytes>;

    /// `utf8` as the controller can show it: printable 7-bit ASCII, at most
    /// kMaxTextLength characters, with no leading or trailing spaces. Accented Latin
    /// letters lose their accents and typographic punctuation becomes its plain ASCII
    /// form; any other character, and any malformed UTF-8, becomes '?'.
    [[nodiscard]] static std::string ToDisplayText(std::string_view utf8);

    /// Writes a complete SET_TEXT message into `out` and returns its length. `text`
    /// should already be display text; past kMaxTextLength it is cut, and every byte is
    /// masked to 7 bits so the message stays well formed whatever it is given.
    static std::size_t BuildSetTextSysEx(std::uint8_t line, std::string_view text, SysExBuffer& out);

    /// Message thread: queues `presetName` for the display if it differs from the name
    /// last queued, or a refresh is pending. While `enabled` is false nothing is sent and
    /// anything not yet collected is dropped; turning it back on sends the name again.
    void Update(std::string_view presetName, bool enabled);

    /// Any thread: send the name again at the next Update(), even if it is unchanged.
    void RequestRefresh();

    /// Audio thread: copies the queued message into `out` and returns its length.
    /// Returns 0 when nothing is queued, when the lock is busy, or when `out` is too
    /// small — the message then stays queued for the next call.
    [[nodiscard]] std::size_t TakeSysEx(std::span<std::uint8_t> out);

  private:
    /// Message thread only. The name last queued, or nothing when there has been no
    /// enabled Update() since construction or since the feed was switched off.
    std::optional<std::string> mLastQueuedName;

    std::atomic<bool> mRefreshRequested{false};

    std::mutex mQueuedMutex;
    SysExBuffer mQueued{};
    std::size_t mQueuedSize = 0;
    /// Written only under mQueuedMutex; read without it as a cheap "anything to do?".
    std::atomic<bool> mHasQueued{false};
};
} // namespace guitarfx
