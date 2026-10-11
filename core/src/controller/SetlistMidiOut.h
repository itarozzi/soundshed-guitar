#pragma once

// SetlistMidiOut — tells the MIDI device which bank and setlist slot was picked in the app,
// so a controller (or a second unit following along) shows the same selection.
//
// A slot picked in the app goes out as the setlist's Bank Select, when the setlist has a bank
// number, followed by a Program Change of the slot's index: the same "preset N of bank B" a
// controller sends to pick it (docs/automation-and-midi-mapping.md, §1), so the device hears
// what it would have sent itself. A setlist picked on its own sends only its Bank Select,
// since nothing is loaded. How the bank number is split across CC0 and CC32 is a setting,
// because devices differ.
//
// Only picks made in the app are sent. A change that came from MIDI was the device's own, and
// one from the DAW is the host's to route; echoing either back is at best redundant and at
// worst a loop with a device that answers a program change with one of its own.
//
// Threads. Queue() runs on the message thread and replaces anything not yet collected: only
// the newest pick matters to the device. The audio thread collects it with Take() and adds it
// to the block's MIDI output. Take() never blocks or allocates — it try-locks, and leaves the
// messages for the next block if the message thread is in the middle of a Queue().

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>

namespace guitarfx
{
class SetlistMidiOut
{
  public:
    /// How a bank number goes out as Bank Select.
    enum class BankSelect
    {
        MsbLsb, ///< CC0 = bank / 128, then CC32 = bank % 128: the MIDI standard, banks 0..16383.
        Msb,    ///< CC0 = bank, banks 0..127.
        Lsb,    ///< CC32 = bank, banks 0..127.
        None,   ///< No Bank Select; a slot sends its Program Change alone.
    };

    /// A channel message of up to three bytes.
    struct ShortMessage
    {
        std::array<std::uint8_t, 3> bytes{};
        std::uint8_t size = 0;
    };

    /// Bank Select MSB, Bank Select LSB, Program Change.
    static constexpr std::size_t kMaxMessages = 3;
    using Messages = std::array<ShortMessage, kMaxMessages>;

    /// The settings value for each mode ("msbLsb", "msb", "lsb", "none"). Anything else,
    /// including an absent value, is the default, MsbLsb.
    [[nodiscard]] static BankSelect ParseBankSelect(std::string_view value);

    /// Writes the messages for a pick into `out` and returns how many: the Bank Select for
    /// `bank` when there is one and `mode` sends it, then the Program Change for `program` when
    /// there is one. `channel` is 1..16. Returns 0, sending nothing, when the channel, the bank
    /// or the program cannot be expressed: a device told the wrong bank would load the wrong
    /// thing, which is worse than not hearing about the pick.
    static std::size_t Build(int channel, BankSelect mode, std::optional<int> bank, std::optional<int> program,
                             Messages& out);

    /// Message thread: queues a pick, replacing one not yet collected. Returns false, queuing
    /// nothing, when Build() makes no messages for it.
    bool Queue(int channel, BankSelect mode, std::optional<int> bank, std::optional<int> program);

    /// Audio thread: copies the queued messages into `out`, in the order they are to be sent,
    /// and returns how many. Returns 0 when nothing is queued, when the lock is busy, or when
    /// `out` is too small — the messages then stay queued for the next call.
    [[nodiscard]] std::size_t Take(std::span<ShortMessage> out);

  private:
    std::mutex mQueuedMutex;
    Messages mQueued{};
    std::size_t mQueuedCount = 0;
    /// Written only under mQueuedMutex; read without it as a cheap "anything to do?".
    std::atomic<bool> mHasQueued{false};
};
} // namespace guitarfx
