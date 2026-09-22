// Notes into a hosted instrument, against a stub AudioPluginInstance that records the MIDI it is
// given, so it runs on any machine.
//
// Covers what JuceHostedPluginEffect adds on top of NotePlayer (tested in the core):
//   * a note source's note-on reaches the plugin at the sample it was made;
//   * with no source left (the Guitar to MIDI node bypassed or removed), the held note is let go;
//   * an instrument, with no audio input, starts from a silent buffer rather than the guitar;
//   * a Plugin Host with no note source sends its plugin nothing, as before;
//   * Reset() turns off what was sent; a new instance starts the note still held upstream;
//   * and, where Surge XT (or GUITARFX_TEST_INSTRUMENT) is installed, a real instrument plays.

#include "JuceHostedPluginEffect.h"

#include "dsp/NoteEvents.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 256;

struct ReceivedMessage
{
    int status = 0;
    int data1 = 0;
    int data2 = 0;
    int sampleOffset = 0;
};

/// An instrument: no audio input, a stereo output, MIDI in. It writes a constant into its output
/// and records what it was given, and the largest sample already in its buffer when called.
class StubInstrument final : public juce::AudioPluginInstance
{
public:
    StubInstrument()
        : juce::AudioPluginInstance (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {
    }

    std::vector<std::vector<ReceivedMessage>> blocks;
    float largestIncomingSample = 0.0f;

    void fillInPluginDescription (juce::PluginDescription& description) const override
    {
        description.name = "Stub Instrument";
        description.pluginFormatName = "Stub";
        description.manufacturerName = "Soundshed Tests";
        description.isInstrument = true;
        description.uniqueId = 2;
    }

    const juce::String getName() const override { return "Stub Instrument"; }
    void prepareToPlay (double, int) override {}
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            largestIncomingSample = std::max (largestIncomingSample, buffer.getMagnitude (ch, 0, buffer.getNumSamples()));

        std::vector<ReceivedMessage> received;

        for (const auto metadata : midi)
        {
            const auto* data = metadata.data;
            received.push_back ({ data[0], metadata.numBytes > 1 ? data[1] : 0, metadata.numBytes > 2 ? data[2] : 0,
                                  metadata.samplePosition });
        }

        blocks.push_back (std::move (received));

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            buffer.clear (ch, 0, buffer.getNumSamples());

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            juce::FloatVectorOperations::fill (buffer.getWritePointer (ch), 0.25f, buffer.getNumSamples());
    }

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock& destData) override { destData.reset(); }
    void setStateInformation (const void*, int) override {}
};

int gFailures = 0;

void Check (bool condition, const std::string& what)
{
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << what << "\n";
    gFailures += condition ? 0 : 1;
}

/// One block through the effect: a guitar-ish input, and `sources` as the executor would hand them.
std::vector<float> RunBlock (guitarfx::JuceHostedPluginEffect& effect, std::vector<const guitarfx::NoteBlock*> sources)
{
    std::vector<float> inL (kBlock, 0.5f), inR (kBlock, 0.5f), outL (kBlock, 0.0f), outR (kBlock, 0.0f);
    float* inputs[2] = { inL.data(), inR.data() };
    float* outputs[2] = { outL.data(), outR.data() };
    effect.SetNoteInput (sources);
    effect.Process (inputs, outputs, kBlock);
    return outL;
}

guitarfx::NoteBlock NoteOnBlock (int note, int offset)
{
    guitarfx::NoteBlock block;
    guitarfx::NoteEvent on;
    on.type = guitarfx::NoteEvent::Type::NoteOn;
    on.note = static_cast<std::uint8_t> (note);
    on.velocity = 90;
    on.sampleOffset = offset;
    block.Push (on);
    block.heldNote = note;
    block.heldVelocity = 90;
    return block;
}

guitarfx::NoteBlock HoldingBlock (int note)
{
    guitarfx::NoteBlock block;
    block.heldNote = note;
    block.heldVelocity = 90;
    return block;
}

bool Has (const std::vector<ReceivedMessage>& block, int status, int note, int offset = -1)
{
    return std::any_of (block.begin(), block.end(), [&] (const ReceivedMessage& m) {
        return m.status == status && m.data1 == note && (offset < 0 || m.sampleOffset == offset);
    });
}

void TestNotesReachTheInstrument()
{
    guitarfx::JuceHostedPluginEffect effect;
    auto plugin = std::make_unique<StubInstrument>();
    auto* stub = plugin.get();
    effect.InstallHostedPluginForTesting (std::move (plugin));
    effect.Prepare (kSampleRate, kBlock);

    const auto quiet = RunBlock (effect, {});
    Check (!stub->blocks.empty() && stub->blocks.back().empty(), "with no note source the instrument is sent nothing");

    auto start = NoteOnBlock (64, 100);
    const auto out = RunBlock (effect, { &start });
    Check (stub->blocks.size() == 2 && Has (stub->blocks.back(), 0x90, 64, 100),
           "a note-on reaches the instrument at the sample it was made");
    Check (stub->largestIncomingSample == 0.0f, "an instrument starts from a silent buffer, not the guitar");
    Check (std::abs (out[10] - 0.25f) < 1.0e-6f, "and at Mix 1 the Plugin Host's output is the instrument's");

    auto holding = HoldingBlock (64);
    RunBlock (effect, { &holding });
    Check (stub->blocks.back().empty(), "a note still held sends nothing more");

    RunBlock (effect, {});
    Check (Has (stub->blocks.back(), 0x80, 64), "with its source gone the held note is turned off");

    RunBlock (effect, {});
    Check (stub->blocks.back().empty(), "and only once");
}

void TestResetAndNewInstance()
{
    guitarfx::JuceHostedPluginEffect effect;
    auto plugin = std::make_unique<StubInstrument>();
    auto* stub = plugin.get();
    effect.InstallHostedPluginForTesting (std::move (plugin));
    effect.Prepare (kSampleRate, kBlock);

    auto start = NoteOnBlock (60, 0);
    RunBlock (effect, { &start });
    effect.Reset();
    auto holding = HoldingBlock (60);
    RunBlock (effect, { &holding });
    Check (Has (stub->blocks.back(), 0x80, 60, 0) && Has (stub->blocks.back(), 0x90, 60),
           "Reset() turns the note off, and the note still held upstream starts again");

    // A new instance: nothing on it to turn off, and the held note starts on it.
    auto second = std::make_unique<StubInstrument>();
    auto* fresh = second.get();
    effect.InstallHostedPluginForTesting (std::move (second));
    effect.Prepare (kSampleRate, kBlock);
    RunBlock (effect, { &holding });
    Check (!fresh->blocks.empty() && Has (fresh->blocks.back(), 0x90, 60) && !Has (fresh->blocks.back(), 0x80, 60),
           "a new instance is sent the held note, and no note-off for the old one");
    (void) stub;
}

float Rms (const std::vector<float>& block)
{
    double sum = 0.0;

    for (const float v : block)
        sum += static_cast<double> (v) * v;

    return static_cast<float> (std::sqrt (sum / static_cast<double> (std::max<std::size_t> (1, block.size()))));
}

/// A real instrument, when one is installed: silent until a note, sounding while it is held, and
/// quiet again once its source has gone. GUITARFX_TEST_INSTRUMENT names a plugin to use instead of
/// Surge XT; with neither there, the check is skipped.
void TestRealInstrument()
{
    std::filesystem::path path = R"(C:\Program Files\Common Files\VST3\Surge Synth Team\Surge XT.vst3)";

    if (const char* chosen = std::getenv ("GUITARFX_TEST_INSTRUMENT"); chosen && *chosen)
        path = chosen;

    if (!std::filesystem::exists (path))
    {
        std::cout << "[SKIP] no instrument at " << path.string() << "\n";
        return;
    }

    guitarfx::JuceHostedPluginEffect effect;
    effect.SetParam ("mix", 1.0);
    effect.Prepare (kSampleRate, kBlock);

    if (!effect.LoadResource (path))
    {
        Check (false, "the instrument at " + path.string() + " loads");
        return;
    }

    const int blocksPerSecond = static_cast<int> (kSampleRate) / kBlock;
    float before = 0.0f;

    for (int i = 0; i < blocksPerSecond / 2; ++i)
        before = std::max (before, Rms (RunBlock (effect, {})));

    auto start = NoteOnBlock (57, 0);
    RunBlock (effect, { &start });
    auto holding = HoldingBlock (57);
    float during = 0.0f;

    for (int i = 0; i < blocksPerSecond / 2; ++i)
        during = std::max (during, Rms (RunBlock (effect, { &holding })));

    float after = 1.0f;

    for (int i = 0; i < 4 * blocksPerSecond && after > 1.0e-4f; ++i)
        after = Rms (RunBlock (effect, {}));

    std::cout << "  " << path.filename().string() << ": before " << before << ", holding " << during << ", after " << after << "\n";
    Check (before < 1.0e-4f && during > 1.0e-3f, "a real instrument is silent until it is given a note, and plays it");
    Check (after < 1.0e-4f, "and falls quiet once the note's source has gone");
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    TestNotesReachTheInstrument();
    TestResetAndNewInstance();
    TestRealInstrument();

    std::cout << "\nHosted plugin note input tests: " << (gFailures == 0 ? "all passed" : std::to_string (gFailures) + " failed")
              << "\n";
    return gFailures == 0 ? 0 : 1;
}
