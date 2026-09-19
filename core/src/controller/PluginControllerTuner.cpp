/**
 * PluginControllerTuner.cpp - The signal-path test tone.
 *
 * SignalTestService owns the tone and its measurement; what lives here is its
 * message plumbing. The tuner answers its own messages (controller/TunerService.cpp).
 */

#include "PluginController.h"

#include "controller/SignalTestService.h"

namespace guitarfx
{
bool PluginController::StartSignalPathTest(double frequencyHz, double durationSeconds)
{
    return mSignalTest->Start(frequencyHz, durationSeconds, mHost.GetSampleRate());
}

void PluginController::HandleSignalTestRequest(const nlohmann::json& payload)
{
    double freq = payload.value("frequency", 440.0);
    double dur = payload.value("duration", 1.0);
    StartSignalPathTest(freq, dur);
}
} // namespace guitarfx
