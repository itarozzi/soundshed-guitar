/**
 * @file NotePlayerTests.cpp
 * @brief Tests for the note player a hosted instrument uses (NotePlayer in dsp/NoteEvents.h).
 *
 *   - two sources' events are merged in time order
 *   - what was sent is reconciled with what the sources hold: a source that stops running has its
 *     note turned off, a dropped block's note-on is sent in the next, an instrument that was reset
 *     has everything turned off and the held note started again, and a new one is sent the held
 *     note with nothing to turn off
 *   - pitch bend goes out at 14 bits and is centred when its source goes
 *   - a block with more than it has room for drops the rest, and the next brings it back
 */

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "dsp/NoteEvents.h"

namespace
{
using guitarfx::NoteBlock;
using guitarfx::NoteEvent;
using guitarfx::NotePlayer;

int gFailures = 0;
int gChecks = 0;

void Check(bool condition, const std::string& what, const std::string& detail = "")
{
    ++gChecks;
    gFailures += condition ? 0 : 1;
    std::cout << (condition ? "  [PASS] " : "  [FAIL] ") << what;

    if (!detail.empty())
    {
        std::cout << "  (" << detail << ")";
    }

    std::cout << std::endl;
}

NoteBlock Holding(int note, int channel = 0, int velocity = 90)
{
    NoteBlock block;
    block.heldNote = note;
    block.heldVelocity = static_cast<std::uint8_t>(velocity);
    block.channel = static_cast<std::uint8_t>(channel);
    return block;
}

void PushOn(NoteBlock& block, int note, int offset, int channel = 0)
{
    NoteEvent on;
    on.type = NoteEvent::Type::NoteOn;
    on.channel = static_cast<std::uint8_t>(channel);
    on.note = static_cast<std::uint8_t>(note);
    on.velocity = 90;
    on.sampleOffset = offset;
    block.Push(on);
}

void TestNotePlayer()
{
    std::cout << "\nThe note player" << std::endl;

    // Two sources merged in time order.
    NotePlayer player;
    NoteBlock first = Holding(60);
    PushOn(first, 60, 40);
    NoteBlock second = Holding(64, 1);
    PushOn(second, 64, 10, 1);
    const NoteBlock* both[2] = {&first, &second};
    const auto merged = player.Build(both, 64);
    Check(merged.size() == 2 && merged[0].data1 == 64 && merged[0].sampleOffset == 10 && merged[1].data1 == 60 &&
              merged[1].sampleOffset == 40,
          "two sources' events come out in time order");
    player.Commit();

    // One source goes away: its note is turned off at the end of the block.
    const NoteBlock* one[1] = {&second};
    NoteBlock quiet = Holding(64, 1);
    one[0] = &quiet;
    const auto gone = player.Build(one, 64);
    Check(gone.size() == 1 && gone[0].status == 0x80 && gone[0].data1 == 60 && gone[0].sampleOffset == 63,
          "a source that stops running has its note turned off");
    player.Commit();

    // A block the instrument never got: the next block catches up from what the source holds.
    NotePlayer dropped;
    NoteBlock start = Holding(57);
    PushOn(start, 57, 5);
    const NoteBlock* startSource[1] = {&start};
    (void)dropped.Build(startSource, 64); // not committed
    NoteBlock holding = Holding(57);
    const NoteBlock* holdingSource[1] = {&holding};
    const auto caughtUp = dropped.Build(holdingSource, 64);
    Check(caughtUp.size() == 1 && caughtUp[0].status == 0x90 && caughtUp[0].data1 == 57,
          "a dropped block's note-on is sent in the next");
    dropped.Commit();

    dropped.ReleaseAllNext();
    const auto released = dropped.Build(holdingSource, 64);
    Check(released.size() == 2 && released[0].status == 0x80 && released[0].sampleOffset == 0 &&
              released[1].status == 0x90,
          "after a reset every note is turned off, and what is still held starts again");
    dropped.Commit();

    dropped.Forget();
    const auto fresh = dropped.Build(holdingSource, 64);
    Check(fresh.size() == 1 && fresh[0].status == 0x90,
          "a new instrument is sent the held note and nothing to turn off");
    dropped.Commit();

    // Pitch bend: scaled to 14 bits, and centred again when its source goes.
    NotePlayer bender;
    NoteBlock bent = Holding(60);
    NoteEvent bend;
    bend.type = NoteEvent::Type::PitchBend;
    bend.bend = 4096;
    bent.Push(bend);
    bent.bend = 4096;
    const NoteBlock* bentSource[1] = {&bent};
    const auto bendOut = bender.Build(bentSource, 64);
    const bool scaled = std::any_of(bendOut.begin(), bendOut.end(), [](const guitarfx::NoteMidiMessage& m) {
        return m.status == 0xE0 && (m.data1 | (m.data2 << 7)) == 8192 + 4096;
    });
    bender.Commit();
    const auto centred = bender.Build({}, 64);
    const bool back = std::any_of(centred.begin(), centred.end(), [](const guitarfx::NoteMidiMessage& m) {
        return m.status == 0xE0 && (m.data1 | (m.data2 << 7)) == 8192;
    });
    Check(scaled && back, "a bend goes out at 14 bits, and is centred when its source goes");
    bender.Commit();

    // More than a block's room: what did not fit is caught up later.
    NotePlayer crowded;
    std::vector<NoteBlock> many(NotePlayer::kMaxSources);
    std::vector<const NoteBlock*> pointers;

    for (int s = 0; s < NotePlayer::kMaxSources; ++s)
    {
        many[static_cast<std::size_t>(s)] = Holding(s, s);

        for (int e = 0; e < NoteBlock::kMaxEvents; ++e)
        {
            PushOn(many[static_cast<std::size_t>(s)], s, e, s); // each after the first restarts it: off + on
        }

        pointers.push_back(&many[static_cast<std::size_t>(s)]);
    }

    const auto full = crowded.Build(pointers, 64);
    Check(full.size() == NotePlayer::kMaxMessages && crowded.Dropped() > 0, "a crowded block fills and drops the rest",
          std::to_string(crowded.Dropped()) + " dropped");
    crowded.Commit();
    std::vector<NoteBlock> steady;

    for (int s = 0; s < NotePlayer::kMaxSources; ++s)
    {
        steady.push_back(Holding(s, s));
    }

    std::vector<const NoteBlock*> steadyPointers;

    for (const auto& block : steady)
    {
        steadyPointers.push_back(&block);
    }

    (void)crowded.Build(steadyPointers, 64);
    crowded.Commit();
    bool allOn = true;

    for (int s = 0; s < NotePlayer::kMaxSources; ++s)
    {
        allOn = allOn && crowded.IsSounding(s, s);
    }

    Check(allOn, "and the next block brings every held note back");
}
} // namespace

int main()
{
    std::cout << "Note player tests" << std::endl;
    TestNotePlayer();
    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}
