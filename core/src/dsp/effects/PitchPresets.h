#pragma once

/**
 * PitchPresets.h — Factory presets for Synth Voice, the Auto Arpeggiator and Pitch Shift.
 *
 * Synth Voice's presets leave out Output, the player's level, and Gate, a threshold set
 * against the player's own noise floor: a preset that reset it would have the synth sputter on
 * hum for anyone who had raised it. They stay within about 1 dB of the default on the demo DI,
 * and every pulse width stays at 0.5. Any other width used to add DC; the Square is zero-mean at
 * every width now, though a narrower pulse measures quieter (1.9 dB at 0.2) and needs level-matching.
 *
 * The arpeggiator's leave out Pitch Trigger and its Pitch, which decide when it plays from what
 * and where the player plays, as the wah's presets leave Auto-Engage alone. Every preset sets
 * the eight custom steps; on a built-in pattern they mirror it, so switching Pattern to Custom
 * keeps the same arp. None goes faster than 1/8 triplets, a limit chosen when a new pitch arrived
 * about 35 ms into its step; since the arp moved to the time-domain shifter (2026-10) it arrives on
 * the step's first sample.
 *
 * Pitch Shift's set everything, Semitones included: with no pedal mapped it is the interval
 * itself. A pedal preset sets it to the end the pedal rests at, inside its own Range, and a
 * mapped pedal takes over on its next move. Anything a pedal moves, or that is played live on
 * single notes, uses the Low Latency engine.
 */

#include "dsp/effects/FactoryPresetSupport.h"

#include <vector>

namespace guitarfx::pitch_presets
{
// Keep ids stable once shipped; the UIs list them.

[[nodiscard]] inline std::vector<EffectPresetDefinition> SynthVoice(const std::vector<ParameterDef>& params)
{
    const factory_presets::Builder b(params, {"outputGain", "gate"});
    constexpr double kSaw = 0.0;
    constexpr double kSquare = 1.0;
    constexpr double kTriangle = 2.0;
    constexpr double kSine = 3.0;

    return {
        b.Defaults("classic-saw", "Classic Saw"),
        b.Make("synth-bass", "Synth Bass",
               {{"attack", 2.0},
                {"release", 80.0},
                {"octaveShift", -1.0},
                {"glide", 5.0},
                {"voice2Mix", 0.25},
                {"waveShape", kSquare},
                {"voice2WaveShape", kSaw}}),
        b.Make("square-lead", "Square Lead",
               {{"attack", 4.0},
                {"release", 120.0},
                {"glide", 40.0},
                {"voice2Semitones", 12.0},
                {"voice2Mix", 0.5},
                {"waveShape", kSquare},
                {"voice2WaveShape", kTriangle}}),
        b.Make("fifth-stack", "Fifth Stack",
               {{"release", 150.0}, {"voice2Semitones", 7.0}, {"voice2Mix", 0.35}, {"voice2WaveShape", kSquare}}),
        b.Make("sine-flute", "Sine Flute",
               {{"attack", 15.0},
                {"release", 300.0},
                {"octaveShift", 1.0},
                {"glide", 60.0},
                {"voice2Semitones", -12.0},
                {"voice2Mix", 0.25},
                {"waveShape", kSine},
                {"voice2WaveShape", kSine}}),
        b.Make("saw-pad", "Saw Pad",
               {{"attack", 45.0}, {"release", 900.0}, {"glide", 80.0}, {"voice2Semitones", 12.0}, {"voice2Mix", 0.2}}),
        // The guitar with a sub synth under it, as an octave pedal.
        b.Make("sub-octave-blend", "Sub Octave Blend",
               {{"mix", 0.5}, {"attack", 3.0}, {"release", 120.0}, {"octaveShift", -1.0}, {"waveShape", kSquare}}),
    };
}

[[nodiscard]] inline std::vector<EffectPresetDefinition> AutoArp(const std::vector<ParameterDef>& params)
{
    const factory_presets::Builder b(params, {"pitchMode", "pitchThreshold"});
    // Step Rate: 1/4, 1/8, 1/16, 1/32, 1/8T, 1/16T, 1/32T.
    constexpr double kQuarter = 0.0;
    constexpr double kEighthTriplet = 4.0;
    // Pattern: Major Triad, Minor Triad, Power Chord, Octaves, Custom, Random.
    constexpr double kMinor = 1.0;
    constexpr double kPower = 2.0;
    constexpr double kOctaves = 3.0;
    constexpr double kCustom = 4.0;
    // Direction: Up, Down, Up-Down.
    constexpr double kDown = 1.0;
    constexpr double kUpDown = 2.0;

    return {
        b.Defaults("major-arp", "Major Arp"),
        b.Make("minor-up-down", "Minor Up-Down",
               {{"stepRate", kEighthTriplet},
                {"pattern", kMinor},
                {"direction", kUpDown},
                {"gate", 0.7},
                {"attack", 0.1},
                {"release", 0.1},
                {"step1", 3.0}}),
        b.Make("power-fifths", "Power Fifths",
               {{"stepRate", kEighthTriplet},
                {"pattern", kPower},
                {"direction", kDown},
                {"numSteps", 3.0},
                {"gate", 0.55},
                {"attack", 0.1},
                {"release", 0.1},
                {"step1", 7.0},
                {"step2", 12.0},
                {"step3", 0.0},
                {"mix", 0.9}}),
        // Three octaves, which no built-in pattern gives.
        b.Make("octave-bounce", "Octave Bounce",
               {{"pattern", kCustom},
                {"gate", 0.6},
                {"attack", 0.1},
                {"release", 0.1},
                {"step0", -12.0},
                {"step1", 0.0},
                {"step2", 12.0},
                {"step3", 0.0}}),
        b.Make("sus4-arp", "Sus4 Arp",
               {{"pattern", kCustom}, {"gate", 0.85}, {"attack", 0.1}, {"release", 0.15}, {"step1", 5.0}}),
        // Up a minor seventh chord and back down, in one bar.
        b.Make("minor-seventh", "Minor 7th",
               {{"pattern", kCustom},
                {"numSteps", 8.0},
                {"gate", 0.75},
                {"attack", 0.1},
                {"release", 0.12},
                {"step0", 0.0},
                {"step1", 3.0},
                {"step2", 7.0},
                {"step3", 10.0},
                {"step4", 12.0},
                {"step5", 10.0},
                {"step6", 7.0},
                {"step7", 3.0}}),
        b.Make("fifths-ladder", "Fifths Ladder",
               {{"pattern", kCustom},
                {"direction", kDown},
                {"gate", 0.65},
                {"attack", 0.1},
                {"release", 0.1},
                {"step0", -12.0},
                {"step1", -5.0},
                {"step2", 0.0},
                {"step3", 7.0}}),
        // Slow swells with the dry note under them.
        b.Make("octave-swells", "Octave Swells",
               {{"stepRate", kQuarter},
                {"pattern", kOctaves},
                {"numSteps", 2.0},
                {"gate", 0.85},
                {"attack", 0.3},
                {"release", 0.15},
                {"step1", 12.0},
                {"step2", 0.0},
                {"step3", 0.0},
                {"mix", 0.6}}),
    };
}

[[nodiscard]] inline std::vector<EffectPresetDefinition> PitchShift(const std::vector<ParameterDef>& params)
{
    const factory_presets::Builder b(params, {});
    constexpr double kLowLatency = 1.0;

    return {
        b.Defaults("full-range", "Full Range"),
        b.Make("octave-down", "Octave Down", {{"semitones", -12.0}, {"engine", kLowLatency}, {"maxSemitones", 0.0}}),
        // High Quality, as it is played on chords.
        b.Make("octave-up-blend", "Octave Up Blend", {{"semitones", 12.0}, {"mix", 0.4}, {"minSemitones", 0.0}}),
        b.Make("fifth-harmony", "Fifth Harmony",
               {{"semitones", 7.0}, {"mix", 0.5}, {"engine", kLowLatency}, {"minSemitones", 0.0}}),
        // Heel dry, toe an octave up, gliding.
        b.Make("whammy-up", "Whammy Up", {{"engine", kLowLatency}, {"stepMode", 0.0}, {"minSemitones", 0.0}}),
        // Rests at the toe, dry; rock back to dive an octave.
        b.Make("dive-bomb", "Dive Bomb", {{"engine", kLowLatency}, {"stepMode", 0.0}, {"maxSemitones", 0.0}}),
        b.Make("step-whammy", "Step Whammy", {{"engine", kLowLatency}, {"minSemitones", 0.0}}),
    };
}
} // namespace guitarfx::pitch_presets
