#pragma once

// What the plugin sends to the MIDI output: the preset name for the controller's display
// (ControllerDisplayFeed), then the bank and setlist slot picked in the app (SetlistMidiOut).
// Called from processBlock after the incoming messages were cleared, so nothing is echoed
// back out. Never blocks or allocates.

#include "PluginController.h"
#include "controller/ControllerDisplayFeed.h"
#include "controller/SetlistMidiOut.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>

inline void AddControllerMidiOutput (guitarfx::PluginController& controller, juce::MidiBuffer& midiMessages)
{
    guitarfx::ControllerDisplayFeed::SysExBuffer displaySysEx {};

    if (const auto size = controller.TakeControllerDisplaySysEx (displaySysEx); size > 0)
        midiMessages.addEvent (displaySysEx.data(), static_cast<int> (size), 0);

    // Bank Select before Program Change: events at the same position keep the order added.
    std::array<guitarfx::SetlistMidiOut::ShortMessage, guitarfx::SetlistMidiOut::kMaxMessages> setlistPick {};
    const auto pickCount = controller.TakeSetlistMidiOut (setlistPick);

    for (std::size_t i = 0; i < pickCount; ++i)
        midiMessages.addEvent (setlistPick[i].bytes.data(), setlistPick[i].size, 0);
}
