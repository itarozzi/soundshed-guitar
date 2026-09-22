#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include "dsp/PitchTracker.h"
#include "dsp/effects/SignalsmithSupport.h"
#include "signalsmith-stretch.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace guitarfx
{
/**
 * Auto-Arpeggiator effect.
 *
 * Rhythmically cycles through a semitone interval pattern by pitch-shifting
 * the full guitar signal, producing an arpeggio effect without the need for
 * a separate synthesizer.
 *
 * Step timing is BPM-synced via the requiresTempo injection mechanism:
 * PluginController::ProcessAudio() calls MultiPresetMixer::SetTempo() each
 * block which ultimately calls SetParam("bpm", bpm) on this effect.
 *
 * Audio architecture:
 *  - Steps with 0 semitones: dry audio passes through (no stretch latency).
 *  - Steps with non-zero semitones: audio is pitch-shifted via Signalsmith
 *    Stretch (same library as PitchShiftEffect).
 *  - Gate envelope is applied per-sample to control note length and attack,
 *    independent of which DSP path is active.
 *  - Wet/dry mix controls blend between gated-wet and always-on-dry.
 *
 * Pitch trigger (Above/Below a threshold): the left input feeds the shared
 * PitchTracker (dsp/PitchTracker.h), and every analysis frame of 2048 samples at
 * 48 kHz (about 43 ms at any rate) the frame's pitch is smoothed and debounced
 * into the arp's on/off state. The pitch used to come from a brute-force YIN run
 * over each 2048-sample frame on the audio thread: a burst of about 340 us in one
 * 64-sample block of every 32, read up to 190 cents sharp (it took the first whole-
 * sample period under its threshold, not the bottom of the dip, so a note a
 * semitone below the threshold tripped it), and no pitch at all at 96 kHz and
 * above, where the longest period no longer fit the frame.
 */
class AutoArpEffect : public EffectProcessor
{
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
        if (!ValidatePrepare(sampleRate, maxBlockSize))
        {
            return;
        }

        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;

        const auto buf = static_cast<size_t>(maxBlockSize);
        mWetL.assign(buf, 0.0f);
        mWetR.assign(buf, 0.0f);
        mZero.assign(buf, 0.0f);

        ConfigureSignalsmithLive(mStretch, 2, sampleRate);
        mConfigured = true;
        mRng.seed(std::random_device{}());

        mTracker.Prepare(sampleRate);
        mPitchFrameLength = std::max(1, static_cast<int>(std::lround(sampleRate * kPitchFrameSeconds)));
        mDetectedHz = 0.0;
        mArpActive = true;

        RebuildStepList();
        UpdatePhaseIncrement();
        ApplyStretchSemitones(mCurrentSemitones);

        Reset();
    }

    void Reset() override
    {
        mPhase = 0.0;
        mCurrentStep = 0;

        if (!mStepSemitones.empty())
        {
            mCurrentSemitones = mStepSemitones[0];
        }

        if (mConfigured)
        {
            mStretch.reset();
            ApplyStretchSemitones(mCurrentSemitones);
        }

        mDetectedHz = 0.0;
        mSmoothedHz = 0.0;
        mTriggerVote = 0;
        mArpActive = (mPitchMode == 0);
        ResetPitchGate();
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (!inputs || !outputs || !mConfigured)
        {
            return;
        }

        numSamples = std::min(numSamples, mMaxBlockSize);

        // Pitch-mode gating — track the pitch, evaluate the trigger condition once per frame.
        if (mPitchMode != 0 && inputs[0])
        {
            // Turned on (again): start from an empty history rather than whatever was playing then.
            if (!mPitchGateRunning)
            {
                ResetPitchGate();
                mPitchGateRunning = true;
            }

            UpdatePitchGate(inputs[0], numSamples);
        }
        else
        {
            mPitchGateRunning = false;
        }

        // When pitch-gated off, pass through dry audio unchanged.
        if (!mArpActive)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                if (outputs[0])
                {
                    outputs[0][i] = inputs[0] ? inputs[0][i] : 0.0f;
                }

                if (outputs[1])
                {
                    outputs[1][i] = inputs[1] ? inputs[1][i] : 0.0f;
                }
            }

            return;
        }

        // Pitch-shift (or bypass) based on the semitones active at block start.
        const int blockSemitones = mCurrentSemitones;

        if (blockSemitones == 0)
        {
            // No pitch change: copy input to wet buffers directly.
            for (int i = 0; i < numSamples; ++i)
            {
                mWetL[static_cast<size_t>(i)] = inputs[0] ? inputs[0][i] : 0.0f;
                mWetR[static_cast<size_t>(i)] = inputs[1] ? inputs[1][i] : 0.0f;
            }
        }
        else
        {
            if (static_cast<size_t>(numSamples) > mWetL.size())
            {
                mWetL.resize(static_cast<size_t>(numSamples), 0.0f);
                mWetR.resize(static_cast<size_t>(numSamples), 0.0f);
                mZero.resize(static_cast<size_t>(numSamples), 0.0f);
            }

            float* ip[2] = {inputs[0] ? inputs[0] : mZero.data(), inputs[1] ? inputs[1] : mZero.data()};
            float* wp[2] = {mWetL.data(), mWetR.data()};
            mStretch.process(ip, numSamples, wp, numSamples);
        }

        // Per-sample: apply gate envelope, advance phase, detect step transitions.
        const float dryMix = static_cast<float>(1.0 - mMix);
        const float wetMix = static_cast<float>(mMix);
        const float attackFrac = mAttack;
        const float gateFrac = mGate;
        // Release window starts at gateFrac; clamped so it never overruns phase 1.0
        const float releaseFrac = std::min(mRelease, std::max(0.0f, 1.0f - gateFrac));
        const float releaseEnd = gateFrac + releaseFrac;

        for (int i = 0; i < numSamples; ++i)
        {
            // Gate envelope: [0, attack) ramp up | [attack, gate) hold | [gate, gate+release) ramp down | silence
            const float phase = static_cast<float>(mPhase);
            float gateGain;

            if (phase < attackFrac)
            {
                gateGain = (attackFrac > 0.0f) ? (phase / attackFrac) : 1.0f;
            }
            else if (phase < gateFrac)
            {
                gateGain = 1.0f;
            }
            else if (releaseFrac > 0.0f && phase < releaseEnd)
            {
                gateGain = 1.0f - (phase - gateFrac) / releaseFrac;
            }
            else
            {
                gateGain = 0.0f;
            }

            const float dryL = inputs[0] ? inputs[0][i] : 0.0f;
            const float dryR = inputs[1] ? inputs[1][i] : 0.0f;

            if (outputs[0])
            {
                outputs[0][i] = dryL * dryMix + mWetL[static_cast<size_t>(i)] * gateGain * wetMix;
            }

            if (outputs[1])
            {
                outputs[1][i] = dryR * dryMix + mWetR[static_cast<size_t>(i)] * gateGain * wetMix;
            }

            // Advance phase; on wrap, advance to next step.
            mPhase += mPhaseIncrement;

            if (mPhase >= 1.0)
            {
                mPhase -= 1.0;
                const int count = static_cast<int>(mStepSemitones.size());

                if (count > 0)
                {
                    if (mRandomDirection)
                    {
                        mCurrentStep = static_cast<int>(mRng() % static_cast<unsigned>(count));
                    }
                    else
                    {
                        mCurrentStep = (mCurrentStep + 1) % count;
                    }

                    if (mRandomPattern)
                    {
                        // Fresh random semitone for this step so every note is unpredictable.
                        mCurrentSemitones = static_cast<int>(mRng() % 25u) - 12;
                        mStepSemitones[static_cast<size_t>(mCurrentStep)] = mCurrentSemitones;
                    }
                    else
                    {
                        mCurrentSemitones = mStepSemitones[static_cast<size_t>(mCurrentStep)];
                    }

                    // Update stretch with new pitch for the next block.
                    ApplyStretchSemitones(mCurrentSemitones);
                }
            }
        }
    }

    void SetParam(const std::string& key, double value) override
    {
        if (key == "bpm")
        {
            mBpm = std::clamp(value, 30.0, 300.0);
            UpdatePhaseIncrement();
        }
        else if (key == "stepRate")
        {
            mStepRate = static_cast<int>(std::clamp(std::round(value), 0.0, 6.0));
            UpdatePhaseIncrement();
        }
        else if (key == "numSteps")
        {
            mNumSteps = static_cast<int>(std::clamp(std::round(value), 2.0, 8.0));
            RebuildStepList();
        }
        else if (key == "pattern")
        {
            mPattern = static_cast<int>(std::clamp(std::round(value), 0.0, 5.0));
            RebuildStepList();
        }
        else if (key == "direction")
        {
            mDirection = static_cast<int>(std::clamp(std::round(value), 0.0, 2.0));
            RebuildStepList();
        }
        else if (key == "gate")
        {
            mGate = static_cast<float>(std::clamp(value, 0.05, 1.0));
        }
        else if (key == "attack")
        {
            mAttack = static_cast<float>(std::clamp(value, 0.0, 0.5));
        }
        else if (key == "release")
        {
            mRelease = static_cast<float>(std::clamp(value, 0.0, 0.5));
        }
        else if (key == "mix")
        {
            mMix = std::clamp(value, 0.0, 1.0);
        }
        else if (key == "pitchMode")
        {
            mPitchMode = static_cast<int>(std::clamp(std::round(value), 0.0, 2.0));

            if (mPitchMode == 0)
            {
                mArpActive = true;
            }
        }
        else if (key == "pitchThreshold")
        {
            mPitchThreshold = std::clamp(value, 50.0, 2000.0);
        }
        else if (key.size() > 4 && key.substr(0, 4) == "step")
        {
            // step0 .. step7
            const int idx = std::stoi(key.substr(4));

            if (idx >= 0 && idx < kMaxCustomSteps)
            {
                mCustomSteps[static_cast<size_t>(idx)] = static_cast<int>(std::clamp(std::round(value), -24.0, 24.0));

                if (mPattern == kPatternCustom)
                {
                    RebuildStepList();
                }
            }
        }
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (key == "bpm")
        {
            return mBpm;
        }

        if (key == "stepRate")
        {
            return static_cast<double>(mStepRate);
        }

        if (key == "numSteps")
        {
            return static_cast<double>(mNumSteps);
        }

        if (key == "pattern")
        {
            return static_cast<double>(mPattern);
        }

        if (key == "direction")
        {
            return static_cast<double>(mDirection);
        }

        if (key == "gate")
        {
            return mGate;
        }

        if (key == "attack")
        {
            return mAttack;
        }

        if (key == "release")
        {
            return mRelease;
        }

        if (key == "mix")
        {
            return mMix;
        }

        if (key == "pitchMode")
        {
            return static_cast<double>(mPitchMode);
        }

        if (key == "pitchThreshold")
        {
            return mPitchThreshold;
        }

        if (key.size() > 4 && key.substr(0, 4) == "step")
        {
            const int idx = std::stoi(key.substr(4));

            if (idx >= 0 && idx < kMaxCustomSteps)
            {
                return static_cast<double>(mCustomSteps[static_cast<size_t>(idx)]);
            }
        }

        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "arp_auto";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "modulation";
    }

    [[nodiscard]] int GetLatencySamples() const override
    {
        // Signalsmith reports latency in two halves; host PDC needs the sum.
        // Note: steps at 0 st bypass Stretch (variable latency by design).
        if (!mConfigured)
        {
            return 0;
        }

        return SignalsmithTotalLatencySamples(mStretch);
    }

  private:
    // ── Pattern table ─────────────────────────────────────────────────────
    static constexpr int kMaxCustomSteps = 8;
    static constexpr int kPatternCustom = 4;
    static constexpr int kPatternRandom = 5;
    /// The trigger is judged once per frame of this length: 2048 samples at 48 kHz, the window the old
    /// detector analysed in one go, so the debounce and smoothing below keep their timing.
    static constexpr double kPitchFrameSeconds = 2048.0 / 48000.0;
    static constexpr double kSilenceRmsSquared = 9.0e-6; // a frame under RMS 0.003 has no pitch
    static constexpr int kActivateFrames = 2;            // consecutive on-windows needed to activate
    static constexpr int kDeactivateFrames = 5;          // consecutive off-windows needed to deactivate
    static constexpr double kPitchEmaAlpha = 0.6;        // EMA weight for new pitch measurement

    // Base patterns: [pattern][step], -1 terminates the list
    static constexpr int kPatternTable[5][9] = {
        {0, 4, 7, 12, -1, -1, -1, -1, -1},    // 0: Major Triad
        {0, 3, 7, 12, -1, -1, -1, -1, -1},    // 1: Minor Triad
        {0, 7, 12, -1, -1, -1, -1, -1, -1},   // 2: Power Chord
        {0, 12, -1, -1, -1, -1, -1, -1, -1},  // 3: Octaves
        {-1, -1, -1, -1, -1, -1, -1, -1, -1}, // 4: Custom (use mCustomSteps / mNumSteps)
    };

    // Beats per step for each stepRate enum value (0-6).
    // Based on a common 4/4 meter reference beat (quarter note = 1 beat).
    static constexpr double kBeatFractions[7] = {
        1.0,        // 0: 1/4  note  = 1 beat
        0.5,        // 1: 1/8  note  = 0.5 beats
        0.25,       // 2: 1/16 note  = 0.25 beats
        0.125,      // 3: 1/32 note  = 0.125 beats
        1.0 / 3.0,  // 4: 1/8  triplet = 1/3 beat
        1.0 / 6.0,  // 5: 1/16 triplet = 1/6 beat
        1.0 / 12.0, // 6: 1/32 triplet = 1/12 beat
    };

    // ── Helpers ───────────────────────────────────────────────────────────

    // Compute phase increment (per sample) for the current BPM and step rate.
    void UpdatePhaseIncrement()
    {
        const double beatsPerStep = kBeatFractions[static_cast<size_t>(mStepRate)];
        const double secondsPerBeat = 60.0 / std::max(1.0, mBpm);
        const double stepSeconds = secondsPerBeat * beatsPerStep;
        const double stepSamples = stepSeconds * std::max(1.0, mSampleRate);
        mPhaseIncrement = 1.0 / std::max(1.0, stepSamples);
    }

    // Rebuild the resolved step semitone list from pattern/direction/numSteps.
    void RebuildStepList()
    {
        std::vector<int> base;

        mRandomPattern = (mPattern == kPatternRandom);
        mRandomDirection = mRandomPattern; // Random pattern always picks steps randomly

        if (mPattern == kPatternRandom)
        {
            // Populate the pool with mNumSteps random semitones in [0, 12].
            // Steps are re-randomized individually on each advance in Process().
            for (int i = 0; i < mNumSteps; ++i)
            {
                base.push_back(static_cast<int>(mRng() % 25u) - 12);
            }
        }
        else if (mPattern == kPatternCustom)
        {
            for (int i = 0; i < mNumSteps && i < kMaxCustomSteps; ++i)
            {
                base.push_back(mCustomSteps[static_cast<size_t>(i)]);
            }
        }
        else
        {
            const auto& row = kPatternTable[static_cast<size_t>(mPattern)];

            for (int i = 0; i < 9 && row[i] >= 0; ++i)
            {
                base.push_back(row[i]);
            }
        }

        if (base.empty())
        {
            mStepSemitones = {0};
            mCurrentStep = 0;
            mCurrentSemitones = 0;
            return;
        }

        // Apply direction (ignored for Random pattern — steps are always picked randomly)
        if (!mRandomPattern)
        {
            switch (mDirection)
            {
            case 0: // Up — use base as-is
                mStepSemitones = base;
                break;
            case 1: // Down — reverse
                mStepSemitones = std::vector<int>(base.rbegin(), base.rend());
                break;
            case 2: // Up-Down — base + reverse without duplicating endpoints
            {
                mStepSemitones = base;

                if (base.size() > 1)
                {
                    for (int i = static_cast<int>(base.size()) - 2; i >= 1; --i)
                    {
                        mStepSemitones.push_back(base[static_cast<size_t>(i)]);
                    }
                }

                break;
            }
            default:
                mStepSemitones = base;
                break;
            }
        }
        else
        {
            mStepSemitones = base;
        }

        // Clamp current step index to new list size
        const int count = static_cast<int>(mStepSemitones.size());

        if (count > 0)
        {
            mCurrentStep = mCurrentStep % count;
            mCurrentSemitones = mStepSemitones[static_cast<size_t>(mCurrentStep)];

            if (mConfigured)
            {
                ApplyStretchSemitones(mCurrentSemitones);
            }
        }
    }

    void ResetPitchGate()
    {
        mTracker.Reset();
        mDetectionsSeen = 0;
        mFrameSamples = 0;
        mFrameEnergy = 0.0;
        mFramePitchHz = 0.0;
    }

    // Feeds the pitch tracker and, at the end of each frame, updates the trigger with the frame's
    // pitch: the tracker's accepted pitch from its latest detection in the frame that found one, or
    // none when no detection did or the frame is quieter than RMS 0.003. No allocation, no locks.
    void UpdatePitchGate(const float* input, int numSamples)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float x = input[i];
            mTracker.Push(x);
            mFrameEnergy += static_cast<double>(x) * static_cast<double>(x);

            if (mTracker.DetectionCount() != mDetectionsSeen)
            {
                mDetectionsSeen = mTracker.DetectionCount();

                if (mTracker.HasPitch() && mTracker.FrequencyHz() > 0.0)
                {
                    mFramePitchHz = mTracker.FrequencyHz();
                }
            }

            if (++mFrameSamples >= mPitchFrameLength)
            {
                const bool audible = mFrameEnergy >= kSilenceRmsSquared * static_cast<double>(mFrameSamples);
                UpdateTrigger(audible ? mFramePitchHz : 0.0);
                mFrameSamples = 0;
                mFrameEnergy = 0.0;
                mFramePitchHz = 0.0;
            }
        }
    }

    // One frame's pitch (0 for none) into the smoothed pitch and the debounced on/off state.
    void UpdateTrigger(double frameHz)
    {
        mDetectedHz = frameHz;

        // EMA smoothing — update only when a confident pitch is detected;
        // during silence let the smoothed value decay gently.
        if (mDetectedHz > 0.0)
        {
            mSmoothedHz = kPitchEmaAlpha * mDetectedHz + (1.0 - kPitchEmaAlpha) * mSmoothedHz;
        }
        else
        {
            mSmoothedHz *= 0.80; // decay toward zero on silence
        }

        const bool conditionMet =
            (mPitchMode == 1) ? (mSmoothedHz > mPitchThreshold) : (mSmoothedHz > 20.0 && mSmoothedHz < mPitchThreshold);

        // Asymmetric debounce: fewer windows needed to activate than to release,
        // so the arp latches on quickly but doesn't drop out on brief dips.
        if (conditionMet)
        {
            mTriggerVote = std::max(0, mTriggerVote) + 1;

            if (mTriggerVote >= kActivateFrames && !mArpActive)
            {
                // Reset to beat-start on fresh activation.
                mPhase = 0.0;
                mCurrentStep = 0;

                if (!mStepSemitones.empty())
                {
                    mCurrentSemitones = mStepSemitones[0];
                    ApplyStretchSemitones(mCurrentSemitones);
                }

                mStretch.reset();
                mArpActive = true;
            }
        }
        else
        {
            mTriggerVote = std::min(0, mTriggerVote) - 1;

            if (mTriggerVote <= -kDeactivateFrames)
            {
                mArpActive = false;
            }
        }
    }

    // Apply semitone transposition to the Signalsmith Stretch instance.
    void ApplyStretchSemitones(int semitones)
    {
        if (!mConfigured)
        {
            return;
        }

        static constexpr double kTonalityLimitHz = 8000.0;
        const float tonalityLimit = static_cast<float>(kTonalityLimitHz / std::max(1.0, mSampleRate));
        mStretch.setTransposeSemitones(static_cast<float>(semitones), tonalityLimit);
    }

    // ── Parameters ────────────────────────────────────────────────────────
    double mBpm = 120.0;
    int mStepRate = 1;  // 0=1/4, 1=1/8, 2=1/16, 3=1/8T, 4=1/16T, 5=1/32, 6=1/32T
    int mNumSteps = 4;  // active steps in Custom mode
    int mPattern = 0;   // 0=Major, 1=Minor, 2=Power, 3=Octaves, 4=Custom
    int mDirection = 0; // 0=Up, 1=Down, 2=UpDown
    float mGate = 0.8f;
    float mAttack = 0.05f;
    float mRelease = 0.08f; // short fade-out to avoid clicks at gate close
    int mCustomSteps[kMaxCustomSteps] = {0, 4, 7, 12, 0, 0, 0, 0};
    double mMix = 0.8;
    int mPitchMode = 0;             // 0=Always, 1=Above threshold, 2=Below threshold
    double mPitchThreshold = 330.0; // Hz — default E4 (open high E string)

    // ── Internal state ────────────────────────────────────────────────────
    double mSampleRate = 48000.0;
    int mMaxBlockSize = 512;
    bool mConfigured = false;
    double mPhase = 0.0;
    double mPhaseIncrement = 0.0;
    int mCurrentStep = 0;
    int mCurrentSemitones = 0;
    bool mRandomPattern = false;
    bool mRandomDirection = false;
    std::mt19937 mRng;
    std::vector<int> mStepSemitones;
    signalsmith::stretch::SignalsmithStretch<float> mStretch;
    std::vector<float> mWetL;
    std::vector<float> mWetR;
    std::vector<float> mZero;
    // Pitch detection state
    PitchTracker mTracker;
    std::uint64_t mDetectionsSeen = 0;
    bool mPitchGateRunning = false; // the tracker is being fed (a pitch trigger mode is on)
    int mPitchFrameLength = 2048;   // samples per trigger evaluation
    int mFrameSamples = 0;          // samples into the current frame
    double mFrameEnergy = 0.0;      // sum of squares over the current frame
    double mFramePitchHz = 0.0;     // the current frame's pitch so far, 0 for none
    double mDetectedHz = 0.0;       // the last frame's pitch (Hz)
    double mSmoothedHz = 0.0;       // EMA-smoothed fundamental used for threshold comparison
    int mTriggerVote = 0;           // debounce counter (>0 = consecutive on-frames, <0 = off-frames)
    bool mArpActive = true;         // current pitch-gate state
};

// ── Registration ──────────────────────────────────────────────────────────

inline void RegisterAutoArpEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kAutoArp;
    info.aliases = {"arp_auto"};
    info.displayName = "Auto Arpeggiator";
    info.category = "modulation";
    info.description =
        "BPM-synced rhythmic arpeggiator. Cycles through interval patterns by pitch-shifting the signal each step.";
    info.requiresTempo = true;
    info.parameters = {
        // Step timing
        {"stepRate",
         "Step Rate",
         1.0,
         0.0,
         6.0,
         "enum",
         "timing",
         false,
         1.0,
         {"1/4 Note", "1/8 Note", "1/16 Note", "1/32 Note", "1/8 Triplet", "1/16 Triplet", "1/32 Triplet"}},
        // Pattern
        {"pattern",
         "Pattern",
         0.0,
         0.0,
         5.0,
         "enum",
         "pattern",
         false,
         1.0,
         {"Major Triad", "Minor Triad", "Power Chord", "Octaves", "Custom", "Random"}},
        {"direction", "Direction", 0.0, 0.0, 2.0, "enum", "pattern", false, 1.0, {"Up", "Down", "Up-Down"}},
        {"numSteps", "Steps", 4.0, 2.0, 8.0, "enum", "pattern", false, 1.0, {"2", "3", "4", "5", "6", "7", "8"}},
        // Envelope
        {"gate", "Gate", 0.8, 0.05, 1.0, "", "envelope", false, 0.0, {}},
        {"attack", "Attack", 0.05, 0.0, 0.5, "", "envelope", false, 0.0, {}},
        {"release", "Release", 0.08, 0.0, 0.5, "", "envelope", false, 0.0, {}},
        // Per-step intervals (Custom mode)
        {"step0", "Step 1", 0.0, -24.0, 24.0, "st", "steps", true, 1.0, {}},
        {"step1", "Step 2", 4.0, -24.0, 24.0, "st", "steps", true, 1.0, {}},
        {"step2", "Step 3", 7.0, -24.0, 24.0, "st", "steps", true, 1.0, {}},
        {"step3", "Step 4", 12.0, -24.0, 24.0, "st", "steps", true, 1.0, {}},
        {"step4", "Step 5", 0.0, -24.0, 24.0, "st", "steps", true, 1.0, {}},
        {"step5", "Step 6", 0.0, -24.0, 24.0, "st", "steps", true, 1.0, {}},
        {"step6", "Step 7", 0.0, -24.0, 24.0, "st", "steps", true, 1.0, {}},
        {"step7", "Step 8", 0.0, -24.0, 24.0, "st", "steps", true, 1.0, {}},
        // Pitch trigger
        {"pitchMode", "Pitch Trigger", 0.0, 0.0, 2.0, "enum", "trigger", false, 1.0, {"Always", "Above", "Below"}},
        {"pitchThreshold", "Pitch", 330.0, 50.0, 2000.0, "Hz", "trigger", false, 0.0, {}},
        // Mix
        {"mix", "Mix", 0.8, 0.0, 1.0, "", "", false, 0.0, {}},
    };

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<AutoArpEffect>(); });
}
} // namespace guitarfx
