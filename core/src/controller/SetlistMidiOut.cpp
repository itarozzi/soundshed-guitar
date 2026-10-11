#include "controller/SetlistMidiOut.h"

#include <algorithm>

namespace guitarfx
{
namespace
{
constexpr std::uint8_t kControlChange = 0xB0;
constexpr std::uint8_t kProgramChange = 0xC0;
constexpr std::uint8_t kBankSelectMsb = 0;
constexpr std::uint8_t kBankSelectLsb = 32;
constexpr int kMaxDataByte = 127;
constexpr int kMaxFourteenBit = 16383;

SetlistMidiOut::ShortMessage ControlChange(std::uint8_t status, std::uint8_t controller, int value)
{
    return {{status, controller, static_cast<std::uint8_t>(value)}, 3};
}
} // namespace

SetlistMidiOut::BankSelect SetlistMidiOut::ParseBankSelect(std::string_view value)
{
    if (value == "msb")
    {
        return BankSelect::Msb;
    }

    if (value == "lsb")
    {
        return BankSelect::Lsb;
    }

    if (value == "none")
    {
        return BankSelect::None;
    }

    return BankSelect::MsbLsb;
}

std::size_t SetlistMidiOut::Build(int channel, BankSelect mode, std::optional<int> bank, std::optional<int> program,
                                  Messages& out)
{
    if (channel < 1 || channel > 16)
    {
        return 0;
    }

    if (program.has_value() && (*program < 0 || *program > kMaxDataByte))
    {
        return 0;
    }

    const bool sendsBank = bank.has_value() && mode != BankSelect::None;

    if (sendsBank)
    {
        const int maxBank = mode == BankSelect::MsbLsb ? kMaxFourteenBit : kMaxDataByte;

        if (*bank < 0 || *bank > maxBank)
        {
            return 0;
        }
    }

    const auto channelBits = static_cast<std::uint8_t>(channel - 1);
    const auto controlChange = static_cast<std::uint8_t>(kControlChange | channelBits);
    std::size_t count = 0;

    if (sendsBank)
    {
        switch (mode)
        {
        case BankSelect::MsbLsb:
            out[count++] = ControlChange(controlChange, kBankSelectMsb, *bank >> 7);
            out[count++] = ControlChange(controlChange, kBankSelectLsb, *bank & 0x7F);
            break;
        case BankSelect::Msb:
            out[count++] = ControlChange(controlChange, kBankSelectMsb, *bank);
            break;
        case BankSelect::Lsb:
            out[count++] = ControlChange(controlChange, kBankSelectLsb, *bank);
            break;
        case BankSelect::None:
            break;
        }
    }

    if (program.has_value())
    {
        out[count++] = {{static_cast<std::uint8_t>(kProgramChange | channelBits), static_cast<std::uint8_t>(*program)},
                        2};
    }

    return count;
}

bool SetlistMidiOut::Queue(int channel, BankSelect mode, std::optional<int> bank, std::optional<int> program)
{
    Messages messages{};
    const std::size_t count = Build(channel, mode, bank, program, messages);

    if (count == 0)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(mQueuedMutex);
    mQueued = messages;
    mQueuedCount = count;
    mHasQueued.store(true, std::memory_order_release);
    return true;
}

std::size_t SetlistMidiOut::Take(std::span<ShortMessage> out)
{
    if (!mHasQueued.load(std::memory_order_acquire))
    {
        return 0;
    }

    std::unique_lock<std::mutex> lock(mQueuedMutex, std::try_to_lock);

    if (!lock.owns_lock() || !mHasQueued.load(std::memory_order_acquire) || out.size() < mQueuedCount)
    {
        return 0;
    }

    std::copy_n(mQueued.begin(), mQueuedCount, out.begin());
    mHasQueued.store(false, std::memory_order_release);
    return mQueuedCount;
}
} // namespace guitarfx
