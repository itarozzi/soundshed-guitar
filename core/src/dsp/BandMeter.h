#pragma once

namespace guitarfx
{
/// One band of a multi-band effect as its node panel meters it (EffectProcessor::ReadBandMeters),
/// sent with the node's spectrum (SignalGraphExecutor::ReadWatchedSpectrum).
struct BandMeter
{
    double lowHz = 0.0;           ///< where the band starts; 0 for the lowest
    double highHz = 0.0;          ///< where it ends; 0 for the highest
    float gainReductionDb = 0.0f; ///< the deepest cut since the last read, 0 or more
    float levelDb = -120.0f;      ///< the band detector's level, dBFS
    float thresholdDb = 0.0f;     ///< the input level, dBFS, where the band's curve starts to act
};
} // namespace guitarfx
