#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <span>

namespace guitarfx
{
/**
 * Notes passed between nodes of a signal graph: from a node that makes them (Guitar to MIDI) to
 * the nodes downstream of it that play them (a hosted instrument).
 *
 * A source fills a NoteBlock every block it runs: the events it decided on, in time order, and
 * the note it is holding once the block is over. The executor hands a player the blocks of every
 * source upstream of it that ran this block (SignalGraphExecutor, "Note routing"), and the
 * player's NotePlayer turns them into the MIDI its instrument receives.
 *
 * The held note is what keeps a note from hanging. A player that missed an event, because its
 * instrument was busy for a block, a source was bypassed or removed, or the player itself was
 * bypassed and has just come back, works out what should be sounding from the held notes of the
 * sources it can see and sends whatever note-offs, note-ons and bends put its instrument there.
 * A source that did not run this block holds nothing.
 *
 * Everything here is fixed-size, so none of it allocates on the audio thread.
 */
struct NoteEvent
{
    enum class Type : std::uint8_t
    {
        NoteOn,
        NoteOff,
        PitchBend
    };

    Type type = Type::NoteOn;
    std::uint8_t channel = 0;  ///< 0 to 15
    std::uint8_t note = 0;     ///< 0 to 127
    std::uint8_t velocity = 0; ///< 1 to 127 for a note-on
    std::int16_t bend = 0;     ///< -8192 to 8191, for a pitch bend
    int sampleOffset = 0;      ///< within the block
};

/// What a note source made in one block. See the file comment.
struct NoteBlock
{
    static constexpr int kMaxEvents = 32;
    static constexpr std::int16_t kMinBend = -8192;
    static constexpr std::int16_t kMaxBend = 8191;

    std::array<NoteEvent, kMaxEvents> events{};
    int count = 0;

    /// The note held once the block is over, or -1, with its channel and velocity; and the
    /// source's pitch bend on that channel, which stays where it was last sent even between notes.
    int heldNote = -1;
    std::uint8_t heldVelocity = 0;
    std::uint8_t channel = 0;
    std::int16_t bend = 0;

    void Clear()
    {
        count = 0;
    }

    /// Appends `event` unless the block is full. The held note is not touched: a caller updates it
    /// once the event has been added, so what is held always matches what was sent.
    bool Push(const NoteEvent& event)
    {
        if (count >= kMaxEvents)
        {
            return false;
        }

        events[static_cast<std::size_t>(count)] = event;
        ++count;
        return true;
    }

    [[nodiscard]] std::span<const NoteEvent> Events() const
    {
        return {events.data(), static_cast<std::size_t>(count)};
    }
};

/// A three-byte MIDI channel message for a player to deliver.
struct NoteMidiMessage
{
    std::uint8_t status = 0;
    std::uint8_t data1 = 0;
    std::uint8_t data2 = 0;
    int sampleOffset = 0;
};

/**
 * The player's side: the MIDI an instrument should receive this block, from the NoteBlocks of
 * the sources feeding it.
 *
 * Build() merges the sources' events in time order and then reconciles, at the end of the block,
 * against their held notes: see the file comment. Commit() once the messages have reached the
 * instrument; a Build() that is never committed (the instrument skipped the block) is simply
 * rebuilt from the same starting point next time, and reconciliation catches up what was missed.
 *
 * It tracks the notes it has sent per channel and note number, so two sources on one channel
 * share them: a note-on for a note already sounding restarts it, and a note-off turns it off even
 * if the other source still holds it, until reconciliation sends it again at the end of the block.
 * Pitch bend is per channel, and the last source to bend a channel sets it.
 */
class NotePlayer
{
  public:
    static constexpr int kMaxMessages = 128;
    static constexpr int kMaxSources = 8;

    /// The messages for a block of `numSamples`. The span stays valid until the next Build().
    [[nodiscard]] std::span<const NoteMidiMessage> Build(std::span<const NoteBlock* const> sources, int numSamples)
    {
        mCount = 0;
        mDropped = 0;
        mPendingSounding = mSounding;
        mPendingBend = mBend;
        const int last = std::max(0, numSamples - 1);

        if (mReleaseAll)
        {
            ReleaseAll(0);
        }

        const auto sourceCount = std::min<std::size_t>(sources.size(), kMaxSources);
        MergeEvents(sources.first(sourceCount), last);
        Reconcile(sources.first(sourceCount), last);
        return {mMessages.data(), static_cast<std::size_t>(mCount)};
    }

    /// The last Build()'s messages reached the instrument.
    void Commit()
    {
        mSounding = mPendingSounding;
        mBend = mPendingBend;
        mReleaseAll = false;
    }

    /// The next Build() starts by turning off every note sent and centring every bend: for an
    /// instrument that has been reset, which may or may not have stopped its voices.
    void ReleaseAllNext()
    {
        mReleaseAll = true;
    }

    /// The instrument has nothing sounding: a new instance. Nothing is sent to turn anything off,
    /// and reconciliation starts whatever the sources hold.
    void Forget()
    {
        mSounding = {};
        mBend = {};
        mReleaseAll = false;
    }

    [[nodiscard]] bool IsSounding(int channel, int note) const
    {
        return Test(mSounding, channel, note);
    }

    [[nodiscard]] std::int16_t Bend(int channel) const
    {
        return (channel >= 0 && channel < kChannels) ? mBend[static_cast<std::size_t>(channel)] : 0;
    }

    /// Messages the last Build() had no room for; reconciliation sends them later.
    [[nodiscard]] int Dropped() const
    {
        return mDropped;
    }

  private:
    static constexpr int kChannels = 16;
    static constexpr int kNotes = 128;
    static constexpr int kWords = kChannels * kNotes / 64;
    using NoteSet = std::array<std::uint64_t, kWords>;

    [[nodiscard]] static bool Test(const NoteSet& set, int channel, int note)
    {
        const int key = channel * kNotes + note;
        return (set[static_cast<std::size_t>(key / 64)] >> (key % 64)) & 1u;
    }

    static void Assign(NoteSet& set, int channel, int note, bool on)
    {
        const int key = channel * kNotes + note;
        const std::uint64_t bit = std::uint64_t{1} << (key % 64);
        auto& word = set[static_cast<std::size_t>(key / 64)];
        word = on ? (word | bit) : (word & ~bit);
    }

    bool Push(std::uint8_t status, std::uint8_t data1, std::uint8_t data2, int offset)
    {
        if (mCount >= kMaxMessages)
        {
            ++mDropped;
            return false;
        }

        mMessages[static_cast<std::size_t>(mCount)] = {status, data1, data2, offset};
        ++mCount;
        return true;
    }

    void NoteOff(int channel, int note, int offset)
    {
        if (Test(mPendingSounding, channel, note) &&
            Push(static_cast<std::uint8_t>(0x80 | channel), static_cast<std::uint8_t>(note), 0, offset))
        {
            Assign(mPendingSounding, channel, note, false);
        }
    }

    void NoteOn(int channel, int note, int velocity, int offset)
    {
        if (Test(mPendingSounding, channel, note))
        {
            NoteOff(channel, note, offset);

            if (Test(mPendingSounding, channel, note))
            {
                return; // no room to restart it; reconciliation will
            }
        }

        const auto clamped = static_cast<std::uint8_t>(std::clamp(velocity, 1, 127));

        if (Push(static_cast<std::uint8_t>(0x90 | channel), static_cast<std::uint8_t>(note), clamped, offset))
        {
            Assign(mPendingSounding, channel, note, true);
        }
    }

    void PitchBend(int channel, std::int16_t bend, int offset)
    {
        auto& current = mPendingBend[static_cast<std::size_t>(channel)];

        if (current == bend)
        {
            return;
        }

        const int value = std::clamp(static_cast<int>(bend), static_cast<int>(NoteBlock::kMinBend),
                                     static_cast<int>(NoteBlock::kMaxBend)) +
                          8192;

        if (Push(static_cast<std::uint8_t>(0xE0 | channel), static_cast<std::uint8_t>(value & 0x7F),
                 static_cast<std::uint8_t>((value >> 7) & 0x7F), offset))
        {
            current = bend;
        }
    }

    void ReleaseAll(int offset)
    {
        for (int channel = 0; channel < kChannels; ++channel)
        {
            for (int note = 0; note < kNotes; ++note)
            {
                NoteOff(channel, note, offset);
            }

            PitchBend(channel, 0, offset);
        }
    }

    /// Every source's events, in time order; a stable merge, so simultaneous events keep the
    /// order their source gave them, and a lower-numbered source goes first.
    void MergeEvents(std::span<const NoteBlock* const> sources, int last)
    {
        std::array<int, kMaxSources> next{};

        while (true)
        {
            int best = -1;
            int bestOffset = 0;

            for (std::size_t s = 0; s < sources.size(); ++s)
            {
                const NoteBlock* block = sources[s];

                if (!block || next[s] >= std::min(block->count, NoteBlock::kMaxEvents))
                {
                    continue;
                }

                const int offset = block->events[static_cast<std::size_t>(next[s])].sampleOffset;

                if (best < 0 || offset < bestOffset)
                {
                    best = static_cast<int>(s);
                    bestOffset = offset;
                }
            }

            if (best < 0)
            {
                return;
            }

            const NoteEvent& event = sources[static_cast<std::size_t>(best)]
                                         ->events[static_cast<std::size_t>(next[static_cast<std::size_t>(best)]++)];
            const int offset = std::clamp(event.sampleOffset, 0, last);
            const int channel = event.channel & 0x0F;
            const int note = event.note & 0x7F;

            switch (event.type)
            {
            case NoteEvent::Type::NoteOn:
                NoteOn(channel, note, event.velocity, offset);
                break;

            case NoteEvent::Type::NoteOff:
                NoteOff(channel, note, offset);
                break;

            case NoteEvent::Type::PitchBend:
                PitchBend(channel, event.bend, offset);
                break;
            }
        }
    }

    /// Brings what has been sent into line with what the sources hold: bends first, so a note
    /// started here starts at its source's pitch, then note-offs, then note-ons.
    void Reconcile(std::span<const NoteBlock* const> sources, int last)
    {
        NoteSet held{};
        std::array<std::int16_t, kChannels> bend{};
        std::array<std::uint8_t, kChannels * kNotes> velocity{};

        for (const NoteBlock* block : sources)
        {
            if (!block)
            {
                continue;
            }

            const int channel = block->channel & 0x0F;
            bend[static_cast<std::size_t>(channel)] = block->bend;

            if (block->heldNote >= 0 && block->heldNote < kNotes)
            {
                Assign(held, channel, block->heldNote, true);
                velocity[static_cast<std::size_t>(channel * kNotes + block->heldNote)] = block->heldVelocity;
            }
        }

        for (int channel = 0; channel < kChannels; ++channel)
        {
            PitchBend(channel, bend[static_cast<std::size_t>(channel)], last);
        }

        for (int word = 0; word < kWords; ++word)
        {
            std::uint64_t extra =
                mPendingSounding[static_cast<std::size_t>(word)] & ~held[static_cast<std::size_t>(word)];

            while (extra != 0)
            {
                const int bit = std::countr_zero(extra);
                extra &= extra - 1;
                const int key = word * 64 + bit;
                NoteOff(key / kNotes, key % kNotes, last);
            }
        }

        for (int word = 0; word < kWords; ++word)
        {
            std::uint64_t missing =
                held[static_cast<std::size_t>(word)] & ~mPendingSounding[static_cast<std::size_t>(word)];

            while (missing != 0)
            {
                const int bit = std::countr_zero(missing);
                missing &= missing - 1;
                const int key = word * 64 + bit;
                NoteOn(key / kNotes, key % kNotes, velocity[static_cast<std::size_t>(key)], last);
            }
        }
    }

    NoteSet mSounding{};
    NoteSet mPendingSounding{};
    std::array<std::int16_t, kChannels> mBend{};
    std::array<std::int16_t, kChannels> mPendingBend{};
    std::array<NoteMidiMessage, kMaxMessages> mMessages{};
    int mCount = 0;
    int mDropped = 0;
    bool mReleaseAll = false;
};
} // namespace guitarfx
