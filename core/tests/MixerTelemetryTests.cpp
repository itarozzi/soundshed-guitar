#include "dsp/MixerTelemetry.h"

#include <cmath>
#include <iostream>

using namespace guitarfx;

int main()
{
    MixerTelemetry telemetry;
    const float left[] = {1.5f, -0.5f};
    const float right[] = {0.5f, -2.0f};

    telemetry.Record(MixerTelemetry::Stage::RawInput, left, right, 2);
    telemetry.Record(MixerTelemetry::Stage::Input, left, nullptr, 2);
    telemetry.Record(MixerTelemetry::Stage::Output, nullptr, nullptr, 2);
    telemetry.NoteOversizedBlock();

    const auto snapshot = telemetry.GetSnapshot();
    if (snapshot.rawInput.peak != 2.0 || snapshot.rawInput.clipCount != 2 ||
        std::abs(snapshot.rawInput.rms - std::sqrt(6.75 / 4.0)) > 1e-6 ||
        snapshot.input.peak != 1.5 || snapshot.input.clipCount != 1 ||
        std::abs(snapshot.input.rms - std::sqrt(2.5 / 2.0)) > 1e-6 ||
        snapshot.output.peak != 0.0 || snapshot.output.rms != 0.0 ||
        telemetry.GetOversizedBlockCount() != 1)
    {
        std::cerr << "Mixer level accumulation changed\n";
        return 1;
    }

    SignalGraphExecutor::NodeSignalLevel source;
    source.nodeId = "gate";
    source.nodeType = "dynamics.gate";
    source.peak = 0.8;
    source.rms = 0.4;
    source.clipCount = 3;
    source.channelCount = 2;
    const auto node = MixerTelemetry::ToSnapshotNode(source, "preset", "slot-a");
    if (node.scope != "preset" || node.presetId != "slot-a" || node.nodeId != source.nodeId ||
        node.nodeType != source.nodeType || node.channelCount != 2 || node.levels.peak != 0.8 ||
        node.levels.rms != 0.4 || node.levels.clipCount != 3)
    {
        std::cerr << "Node diagnostics mapping changed\n";
        return 1;
    }

    telemetry.SetEnabled(false);
    MixerTelemetry movedReadings;
    movedReadings.CopyFrom(telemetry);
    if (movedReadings.IsEnabled() || movedReadings.GetOversizedBlockCount() != 1 ||
        movedReadings.GetSnapshot().rawInput.peak != 2.0)
    {
        std::cerr << "Telemetry state was not retained during mixer move\n";
        return 1;
    }
    return 0;
}
