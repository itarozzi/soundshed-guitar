/**
 * @file SplitterRoutingTests.cpp
 * @brief The Splitter's routing modes, and the executor giving each branch what it routed.
 *
 *   - Copy writes nothing and leaves a split exactly as it was before the Splitter had modes
 *   - Switch and Blend put each branch at the gain the setting asks for, and a change crossfades
 *     at equal power, over Crossfade, without clicking
 *   - Dynamics sends soft notes to branch 1 and hard ones to branch 2: Pick holds a note's branch
 *     as it rings, Level lets a decaying note move back
 *   - Pitch sends notes to branches by range; a bend over the split note stays put, a legato move
 *     past it follows; Unpitched decides where a pick goes before its pitch is known; on the demo
 *     riffs the notes land on their branch
 *   - Stereo gives branch 1 the left channel and branch 2 the right, each on a mono connection,
 *     and a mode change moves the layout on the next block
 *   - Route allocates nothing in any mode
 *   - Nano Q words the routing as the web UI does (splitBranches.test.ts holds the same lines)
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "AudioThreadAllocations.h"
#include "DemoAudio.h"
#include "GuitarPhraseSynth.h"
#include "dsp/EffectGuids.h"
#include "dsp/PickAttackDetector.h"
#include "dsp/PitchTracker.h"
#include "dsp/SignalGraphExecutor.h"
#include "dsp/effects/BuiltinEffects.h"
#include "dsp/effects/SplitterEffect.h"
#include "presets/PresetTypes.h"
#include "uiclient/SplitRouting.h"

namespace
{
using guitarfx::ChannelLayout;
using guitarfx::GraphEdge;
using guitarfx::GraphNode;
using guitarfx::SignalGraph;
using guitarfx::SignalGraphExecutor;
using guitarfx::SplitterEffect;
using guitarfx::test::PhraseNote;

constexpr double kRate = 48000.0;
constexpr int kBlock = 64;
constexpr double kPi = 3.14159265358979323846;

int gChecks = 0;
int gFailures = 0;

void Check(bool ok, const std::string& what, const std::string& detail = "")
{
    ++gChecks;
    gFailures += ok ? 0 : 1;
    std::cout << (ok ? "  [PASS] " : "  [FAIL] ") << what;

    if (!detail.empty())
    {
        std::cout << " (" << detail << ")";
    }

    std::cout << std::endl;
}

std::string Num(double value)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f", value);
    return text;
}

/// What each branch got, left and right, from Route() over a whole signal.
struct Routed
{
    std::vector<std::vector<float>> left;
    std::vector<std::vector<float>> right;
    int routedBlocks = 0;
};

Routed RouteSignal(SplitterEffect& splitter, const std::vector<float>& left, const std::vector<float>& right,
                   int branches, int block = kBlock)
{
    Routed out;
    out.left.assign(static_cast<std::size_t>(branches), std::vector<float>(left.size(), 0.0f));
    out.right.assign(static_cast<std::size_t>(branches), std::vector<float>(left.size(), 0.0f));

    for (std::size_t start = 0; start < left.size(); start += static_cast<std::size_t>(block))
    {
        const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), left.size() - start));
        const bool routed = splitter.Route(left.data() + start, right.data() + start, branches, n);
        out.routedBlocks += routed ? 1 : 0;

        for (int branch = 0; branch < branches; ++branch)
        {
            const float* l = routed ? splitter.PortLeft(branch) : left.data() + start;
            const float* r = routed ? splitter.PortRight(branch) : right.data() + start;
            std::copy_n(l, n, out.left[static_cast<std::size_t>(branch)].begin() + static_cast<std::ptrdiff_t>(start));
            std::copy_n(r, n, out.right[static_cast<std::size_t>(branch)].begin() + static_cast<std::ptrdiff_t>(start));
        }
    }

    return out;
}

Routed RouteMono(SplitterEffect& splitter, const std::vector<float>& x, int branches)
{
    return RouteSignal(splitter, x, x, branches);
}

double Rms(const std::vector<float>& x, double fromSeconds, double toSeconds)
{
    const auto from = static_cast<std::size_t>(std::max(0.0, fromSeconds) * kRate);
    const auto to = std::min(x.size(), static_cast<std::size_t>(toSeconds * kRate));
    double sum = 0.0;

    for (std::size_t i = from; i < to; ++i)
    {
        sum += static_cast<double>(x[i]) * x[i];
    }

    return to > from ? std::sqrt(sum / static_cast<double>(to - from)) : 0.0;
}

/// How much louder `a` is than `b` over a window, in dB.
double LouderDb(const std::vector<float>& a, const std::vector<float>& b, double from, double to)
{
    return 20.0 * std::log10((Rms(a, from, to) + 1.0e-9) / (Rms(b, from, to) + 1.0e-9));
}

std::vector<float> Sine(double hz, double seconds, double amplitude = 0.5)
{
    std::vector<float> x(static_cast<std::size_t>(seconds * kRate));

    for (std::size_t i = 0; i < x.size(); ++i)
    {
        x[i] = static_cast<float>(amplitude * std::sin(2.0 * kPi * hz * static_cast<double>(i) / kRate));
    }

    return x;
}

SplitterEffect Prepared(std::initializer_list<std::pair<const char*, double>> params)
{
    SplitterEffect splitter;

    for (const auto& [key, value] : params)
    {
        splitter.SetParam(key, value);
    }

    splitter.Prepare(kRate, kBlock);
    return splitter;
}

// ── Copy ────────────────────────────────────────────────────────────────────────────────

void TestCopy()
{
    std::cout << "\nCopy" << std::endl;
    auto splitter = Prepared({});
    const auto x = Sine(220.0, 0.2);
    const auto routed = RouteMono(splitter, x, 2);
    Check(routed.routedBlocks == 0, "a fresh splitter in Copy writes nothing: the branches read its input");

    // Out to Switch and back: it routes while the gains ramp back to unity, then stops.
    splitter.SetParam("mode", 2.0);
    RouteMono(splitter, x, 2);
    splitter.SetParam("mode", 0.0);
    const auto back = RouteMono(splitter, Sine(220.0, 0.1), 2);
    const int rampBlocks = static_cast<int>(std::ceil(0.010 * kRate / kBlock));
    Check(back.routedBlocks > 0 && back.routedBlocks <= rampBlocks + 1,
          "back in Copy it ramps up to unity and then stops writing", std::to_string(back.routedBlocks) + " blocks");

    splitter.SetParam("mode", 2.0);
    splitter.SetEnabled(false);
    RouteMono(splitter, x, 2);
    Check(RouteMono(splitter, x, 2).routedBlocks == 0, "bypassed, it copies");
}

SignalGraph SplitGraph(const std::string& splitType, int branches)
{
    using namespace guitarfx::EffectGuids;
    const char* branchTypes[] = {kOverdrive, kAmpBuiltin, kChorus, kDelayDigital};
    SignalGraph graph;
    GraphNode in;
    in.id = "in";
    in.type = guitarfx::kNodeTypeInput;
    GraphNode split;
    split.id = "split";
    split.type = splitType;
    GraphNode mix;
    mix.id = "mix";
    mix.type = guitarfx::kNodeTypeMixer;
    GraphNode out;
    out.id = "out";
    out.type = guitarfx::kNodeTypeOutput;
    graph.nodes = {in, split, mix, out};
    graph.edges = {{"in", "split", 0, 0, 1.0}, {"mix", "out", 0, 0, 1.0}};

    for (int b = 0; b < branches; ++b)
    {
        GraphNode node;
        node.id = std::string(1, static_cast<char>('a' + b));
        node.type = branchTypes[b];
        graph.nodes.push_back(node);
        graph.edges.push_back({"split", node.id, b, 0, 1.0});
        graph.edges.push_back({node.id, "mix", 0, b, 1.0});
    }

    return graph;
}

std::vector<float> RunGraph(const SignalGraph& graph, const std::vector<float>& x, ChannelLayout layout)
{
    SignalGraphExecutor executor;
    executor.SetInputLayout(layout);
    executor.SetGraph(graph);
    executor.Prepare(kRate, kBlock);
    std::vector<float> out(x.size() * 2, 0.0f);
    std::vector<float> l(kBlock), r(kBlock), ol(kBlock), orr(kBlock);

    for (std::size_t start = 0; start + kBlock <= x.size(); start += kBlock)
    {
        std::copy_n(x.begin() + static_cast<std::ptrdiff_t>(start), kBlock, l.begin());
        std::copy_n(x.begin() + static_cast<std::ptrdiff_t>(start), kBlock, r.begin());
        float* ins[2] = {l.data(), r.data()};
        float* outs[2] = {ol.data(), orr.data()};
        executor.Process(ins, outs, kBlock);
        std::copy_n(ol.begin(), kBlock, out.begin() + static_cast<std::ptrdiff_t>(2 * start));
        std::copy_n(orr.begin(), kBlock, out.begin() + static_cast<std::ptrdiff_t>(2 * start + kBlock));
    }

    return out;
}

/// A split in Copy sounds exactly as one made of a node that only passes its input on, which is
/// what the splitter was before it had modes.
void TestCopyInGraphUnchanged()
{
    std::cout << "\nCopy in a graph" << std::endl;
    const auto x = guitarfx::test::RenderPhrase({{0.05, 0.4, 110.0, 0.3}, {0.5, 0.4, 330.0, 0.2}}, 1.0, kRate);

    for (const auto layout : {ChannelLayout::Mono, ChannelLayout::Stereo})
    {
        for (int branches = 2; branches <= 4; ++branches)
        {
            const auto routed = RunGraph(SplitGraph(guitarfx::EffectGuids::kSplitter, branches), x, layout);
            const auto plain = RunGraph(SplitGraph("test-passthrough-split", branches), x, layout);
            Check(routed == plain, std::to_string(branches) + " branches, " +
                                       (layout == ChannelLayout::Mono ? "mono" : "stereo") +
                                       ": sample-identical to a plain passthrough split");
        }
    }
}

// ── Switch and Blend ──────────────────────────────────────────────────────────────────────

void TestSwitchAndBlend()
{
    std::cout << "\nSwitch and Blend" << std::endl;
    const std::vector<float> ones(static_cast<std::size_t>(0.1 * kRate), 1.0f);

    auto onB = Prepared({{"mode", 2.0}, {"branch", 1.0}});
    const auto b = RouteMono(onB, ones, 2);
    Check(b.left[0].front() == 0.0f && b.left[1].front() == 1.0f,
          "Switch set before audio starts on its branch, without a ramp");

    onB.SetParam("branch", 0.0);
    const auto moved = RouteMono(onB, ones, 2);
    double worstPower = 0.0;
    std::size_t done = 0;

    for (std::size_t i = 0; i < moved.left[0].size(); ++i)
    {
        const double power = moved.left[0][i] * moved.left[0][i] + moved.left[1][i] * moved.left[1][i];
        worstPower = std::max(worstPower, std::abs(power - 1.0));

        if (done == 0 && moved.left[0][i] == 1.0f && moved.left[1][i] == 0.0f)
        {
            done = i;
        }
    }

    Check(worstPower < 1.0e-4, "a switch crossfades at equal power", "worst " + Num(worstPower));
    const double crossfadeMs = 1000.0 * static_cast<double>(done) / kRate;
    Check(crossfadeMs > 9.0 && crossfadeMs < 11.0, "over the 10 ms Crossfade", Num(crossfadeMs) + " ms");

    auto past = Prepared({{"mode", 2.0}, {"branch", 3.0}});
    Check(RouteMono(past, ones, 2).left[1].back() == 1.0f, "a branch past the split's count is its last");

    auto blend = Prepared({{"mode", 1.0}, {"position", 0.5}});
    const auto half = RouteMono(blend, ones, 2);
    Check(std::abs(half.left[0].back() - std::sqrt(0.5f)) < 1.0e-6f &&
              std::abs(half.left[1].back() - std::sqrt(0.5f)) < 1.0e-6f,
          "Blend halfway: both branches at -3 dB, equal power");

    auto linear = Prepared({{"mode", 1.0}, {"position", 0.5}, {"curve", 1.0}});
    Check(std::abs(RouteMono(linear, ones, 2).left[0].back() - 0.5f) < 1.0e-6f, "and at half amplitude with Linear");

    auto three = Prepared({{"mode", 1.0}, {"position", 0.5}});
    const auto middle = RouteMono(three, ones, 3);
    Check(middle.left[0].back() == 0.0f && middle.left[1].back() == 1.0f && middle.left[2].back() == 0.0f,
          "with three branches, halfway is all branch 2");

    // A pedal sweep: every step small, at equal power throughout.
    auto pedal = Prepared({{"mode", 1.0}, {"position", 0.0}});
    const auto tone = Sine(220.5, 1.0);
    std::vector<float> sum(tone.size());
    double largestStep = 0.0;

    for (std::size_t start = 0; start + kBlock <= tone.size(); start += kBlock)
    {
        pedal.SetParam("position", static_cast<double>(start) / static_cast<double>(tone.size()));
        pedal.Route(tone.data() + start, tone.data() + start, 2, kBlock);

        for (int i = 0; i < kBlock; ++i)
        {
            sum[start + static_cast<std::size_t>(i)] = pedal.PortLeft(0)[i] + pedal.PortLeft(1)[i];
        }
    }

    for (std::size_t i = 1; i < sum.size(); ++i)
    {
        largestStep = std::max(largestStep, static_cast<double>(std::abs(sum[i] - sum[i - 1])));
    }

    Check(largestStep < 0.035, "a pedal sweeping Position does not click", "largest step " + Num(largestStep));
}

// ── Dynamics ──────────────────────────────────────────────────────────────────────────────

void TestDynamics()
{
    std::cout << "\nDynamics" << std::endl;
    std::vector<PhraseNote> notes;
    const double peaks[] = {0.02, 0.4, 0.4, 0.02, 0.4, 0.02};

    for (int i = 0; i < 6; ++i)
    {
        notes.push_back({0.1 + 0.5 * i, 0.4, i % 2 == 0 ? 110.0 : 196.0, peaks[i]});
    }

    const auto phrase = guitarfx::test::RenderPhrase(notes, 3.2, kRate);
    auto pick = Prepared({{"mode", 3.0}});
    const auto routed = RouteMono(pick, phrase, 2);
    int right = 0;
    std::string detail;

    for (const auto& note : notes)
    {
        const bool hard = note.peak > 0.1;
        const double margin = hard ? LouderDb(routed.left[1], routed.left[0], note.start + 0.01, note.start + 0.38)
                                   : LouderDb(routed.left[0], routed.left[1], note.start + 0.01, note.start + 0.38);
        right += margin > 20.0 ? 1 : 0;
        detail += Num(margin) + " ";
    }

    Check(right == 6, "Pick: soft notes ring on branch 1, hard ones on branch 2, the whole note", detail + "dB");

    // A struck note decaying from -6 dBFS past the -18 dB threshold at about 0.7 s.
    std::vector<float> decay(static_cast<std::size_t>(2.5 * kRate));

    for (std::size_t i = 0; i < decay.size(); ++i)
    {
        const double t = static_cast<double>(i) / kRate;
        decay[i] = static_cast<float>(0.5 * std::exp(-t / 0.5) * std::sin(2.0 * kPi * 147.0 * t));
    }

    auto level = Prepared({{"mode", 3.0}, {"detect", 1.0}, {"release", 50.0}});
    const auto followed = RouteMono(level, decay, 2);
    Check(LouderDb(followed.left[1], followed.left[0], 0.05, 0.4) > 20.0 &&
              LouderDb(followed.left[0], followed.left[1], 1.0, 2.0) > 20.0,
          "Level: the note starts on branch 2 and moves to branch 1 as it decays");

    auto held = Prepared({{"mode", 3.0}});
    const auto heldRouted = RouteMono(held, decay, 2);
    Check(LouderDb(heldRouted.left[1], heldRouted.left[0], 1.0, 2.0) > 20.0, "Pick holds it on branch 2");

    auto three = Prepared({{"mode", 3.0}});
    const auto withThird = RouteMono(three, phrase, 3);
    Check(withThird.left[2] == phrase, "a third branch gets everything");
}

// ── Pitch ─────────────────────────────────────────────────────────────────────────────────

double Hz(double midi)
{
    return guitarfx::test::NoteHz(midi);
}

void TestPitch()
{
    std::cout << "\nPitch" << std::endl;
    // Split at C4: E2, A2, D3 below; E4, G4, B4 above, in turn.
    const double midi[] = {40, 64, 45, 67, 50, 71};
    std::vector<PhraseNote> notes;

    for (int i = 0; i < 6; ++i)
    {
        notes.push_back({0.1 + 0.6 * i, 0.5, Hz(midi[i]), 0.3});
    }

    const auto phrase = guitarfx::test::RenderPhrase(notes, 3.8, kRate);
    auto all = Prepared({{"mode", 4.0}});
    const auto routed = RouteMono(all, phrase, 2);
    int right = 0;
    std::string detail;

    for (std::size_t i = 0; i < notes.size(); ++i)
    {
        const int branch = midi[i] >= 60 ? 1 : 0;
        const double margin =
            LouderDb(routed.left[static_cast<std::size_t>(branch)], routed.left[static_cast<std::size_t>(1 - branch)],
                     notes[i].start + 0.15, notes[i].start + 0.48);
        right += margin > 20.0 ? 1 : 0;
        detail += Num(margin) + " ";
    }

    Check(right == 6, "each note rings on the branch for its range", detail + "dB");

    // Unpitched All: both branches hear the pick of the first high note after a low one.
    const double attack = notes[1].start;
    const double share = LouderDb(routed.left[0], routed.left[1], attack + 0.001, attack + 0.008);
    Check(std::abs(share) < 4.0, "Unpitched All: a pick goes to both branches until its pitch is known",
          Num(share) + " dB");

    auto last = Prepared({{"mode", 4.0}, {"unpitched", 1.0}});
    const auto lastRouted = RouteMono(last, phrase, 2);
    Check(LouderDb(lastRouted.left[0], lastRouted.left[1], attack + 0.001, attack + 0.008) > 20.0,
          "Unpitched Last: it stays on the last note's branch");

    // A bend from B3 up two semitones crosses C4 and stays on branch 1; a hammer-on from A3 to
    // D4 moves to branch 2.
    std::vector<PhraseNote> moves = {{0.1, 0.9, Hz(59), 0.3, false, true, 2.0, 0.25, 0.1},
                                     {1.2, 0.4, Hz(57), 0.3, false, false},
                                     {1.6, 0.6, Hz(62), 0.3, true}};
    const auto moving = guitarfx::test::RenderPhrase(moves, 2.4, kRate);
    auto bend = Prepared({{"mode", 4.0}});
    const auto bent = RouteMono(bend, moving, 2);
    Check(LouderDb(bent.left[0], bent.left[1], 0.4, 0.95) > 20.0, "a bend across the split note stays on its branch");
    Check(LouderDb(bent.left[1], bent.left[0], 1.75, 2.15) > 20.0,
          "a hammer-on past the split note moves to the next branch");
}

/// A riff's notes as an offline read sees them: pitched notes by the median of the tracker's raw
/// estimates from 40 ms after each pick, which is no guide to when anything live could know it.
struct RiffNote
{
    double start = 0.0;
    double end = 0.0;
    double pitch = 0.0;
};

std::vector<RiffNote> ReadRiff(const std::vector<float>& x, double sampleRate)
{
    guitarfx::PickAttackDetector picks;
    picks.Prepare(sampleRate);
    guitarfx::PitchTracker tracker;
    tracker.Prepare(sampleRate);
    tracker.SetLowestFrequency(guitarfx::pitch_tracker::LowestNoteHz(0));
    std::vector<std::size_t> attacks;
    std::vector<std::pair<std::size_t, double>> estimates;
    std::uint64_t seen = 0;

    for (std::size_t i = 0; i < x.size(); ++i)
    {
        if (picks.Process(x[i]))
        {
            attacks.push_back(i);
        }

        tracker.Push(x[i]);

        if (tracker.DetectionCount() != seen)
        {
            seen = tracker.DetectionCount();

            if (tracker.HasPitch() && tracker.RawFrequencyHz() > 0.0)
            {
                estimates.emplace_back(i, guitarfx::NoteTracker::MidiFromHz(tracker.RawFrequencyHz()));
            }
        }
    }

    attacks.push_back(x.size());
    std::vector<RiffNote> notes;

    for (std::size_t a = 0; a + 1 < attacks.size(); ++a)
    {
        std::vector<double> settled;

        for (const auto& [at, pitch] : estimates)
        {
            if (at >= attacks[a] + static_cast<std::size_t>(0.04 * sampleRate) && at < attacks[a + 1])
            {
                settled.push_back(pitch);
            }
        }

        std::sort(settled.begin(), settled.end());
        RiffNote note;
        note.start = static_cast<double>(attacks[a]) / sampleRate;
        note.end = static_cast<double>(attacks[a + 1]) / sampleRate;

        if (settled.size() >= 3 && settled[settled.size() * 9 / 10] - settled[settled.size() / 10] < 1.0)
        {
            note.pitch = settled[settled.size() / 2];
        }

        notes.push_back(note);
    }

    return notes;
}

/// The demo riffs' hard-picked single notes, split between the lowest few and the rest: each one
/// rings on its branch once its pitch is known.
void TestRealRiffs()
{
    std::cout << "\nPitch on the demo riffs" << std::endl;

    for (const char* file : {"guitar-riff-01.wav", "guitar-riff-02.wav"})
    {
        double sampleRate = 0.0;
        const auto guitar = guitarfx::test::LoadDemoClipMono(file, sampleRate);

        if (guitar.empty() || sampleRate != kRate)
        {
            Check(false, "the demo clip loads at 48 kHz", file);
            continue;
        }

        const auto truth = ReadRiff(guitar, sampleRate);
        std::vector<double> pitched;

        for (const auto& note : truth)
        {
            if (note.pitch > 0.0)
            {
                pitched.push_back(note.pitch);
            }
        }

        // The split note in the widest gap between the riff's notes, so both branches have work and
        // no note sits on the line.
        std::sort(pitched.begin(), pitched.end());
        int split = 48;
        double widest = 0.0;

        for (std::size_t i = 1; i < pitched.size(); ++i)
        {
            if (pitched[i] - pitched[i - 1] > widest)
            {
                widest = pitched[i] - pitched[i - 1];
                split = static_cast<int>(std::lround(0.5 * (pitched[i] + pitched[i - 1])));
            }
        }

        auto splitter = Prepared({{"mode", 4.0}, {"splitNote1", static_cast<double>(split - 28)}});
        const auto routed = RouteMono(splitter, guitar, 2);
        int scored = 0;
        int right = 0;
        std::vector<double> arrivalsMs;

        for (const auto& note : truth)
        {
            // Long enough to judge after the tracker's floor (55 ms median, 86 ms worst, on these).
            if (note.pitch <= 0.0 || note.end - note.start < 0.15)
            {
                continue;
            }

            const auto branch = static_cast<std::size_t>(note.pitch >= split - 0.5 ? 1 : 0);
            ++scored;
            right += LouderDb(routed.left[branch], routed.left[1 - branch], note.start + 0.12, note.end - 0.01) > 10.0
                         ? 1
                         : 0;

            // When the note got there: the first 5 ms after the pick with its branch 10 dB ahead.
            for (double t = note.start; t + 0.005 < note.end; t += 0.001)
            {
                if (LouderDb(routed.left[branch], routed.left[1 - branch], t, t + 0.005) > 10.0)
                {
                    arrivalsMs.push_back(1000.0 * (t - note.start));
                    break;
                }
            }
        }

        std::sort(arrivalsMs.begin(), arrivalsMs.end());
        const std::string arrival = arrivalsMs.empty()
                                        ? std::string("none")
                                        : "on its branch " + Num(arrivalsMs[arrivalsMs.size() / 2]) +
                                              " ms after the pick (median), " + Num(arrivalsMs.back()) + " ms worst";

        const double score = scored > 0 ? static_cast<double>(right) / scored : 0.0;
        Check(scored >= 4 && score >= 0.8, std::string(file) + ": notes ring on their branch",
              std::to_string(right) + " of " + std::to_string(scored) + ", split " + std::to_string(split) + "; " +
                  arrival);
    }
}

// ── Stereo ────────────────────────────────────────────────────────────────────────────────

void TestStereo()
{
    std::cout << "\nStereo" << std::endl;
    const auto left = Sine(220.0, 0.1);
    const auto right = Sine(330.0, 0.1, 0.25);
    auto splitter = Prepared({{"mode", 5.0}});
    const auto routed = RouteSignal(splitter, left, right, 3);
    Check(routed.left[0] == left && routed.right[0] == left, "branch 1 gets the left channel, on both sides");
    Check(routed.left[1] == right && routed.right[1] == right, "branch 2 gets the right channel, on both sides");
    Check(routed.left[2] == left && routed.right[2] == right, "branch 3 gets both, as they came");
    Check(splitter.SplitsChannels(), "and says so to the layout");

    auto graph = SplitGraph(guitarfx::EffectGuids::kSplitter, 3);
    graph.FindNode("split")->params["mode"] = 5.0;

    // Branch types that cannot widen, so a branch's layout is its connection's.
    for (const char* id : {"a", "b", "c"})
    {
        graph.FindNode(id)->type = guitarfx::EffectGuids::kOverdrive;
    }

    SignalGraphExecutor executor;
    executor.SetInputLayout(ChannelLayout::Stereo);
    executor.SetGraph(graph);
    executor.Prepare(kRate, kBlock);
    std::vector<float> l(kBlock, 0.1f), r(kBlock, -0.1f), ol(kBlock), orr(kBlock);
    float* ins[2] = {l.data(), r.data()};
    float* outs[2] = {ol.data(), orr.data()};

    const auto counts = [&]() {
        executor.Process(ins, outs, kBlock);
        std::map<std::string, int> found;

        for (const auto& level : executor.GetNodeSignalLevels())
        {
            found[level.nodeId] = level.channelCount;
        }

        return found;
    };

    auto split = counts();
    Check(split["a"] == 1 && split["b"] == 1 && split["c"] == 2,
          "on a stereo input, branches 1 and 2 run mono and branch 3 stereo");

    executor.SetNodeParam("split", "mode", 0.0);
    split = counts();
    Check(split["a"] == 2 && split["b"] == 2, "back in Copy, the next block has every branch stereo again");
}

// ── Allocation ────────────────────────────────────────────────────────────────────────────

void TestNoAllocations()
{
    std::cout << "\nAllocation" << std::endl;
    const auto phrase = guitarfx::test::RenderPhrase({{0.02, 0.2, 110.0, 0.3}, {0.25, 0.2, 440.0, 0.1}}, 0.5, kRate);
    auto splitter = Prepared({});
    // Made here: a Debug std::string allocates its iterator proxy even when it is short.
    const std::string modeKey = "mode";
    const std::string lowestKey = "lowestNote";

    const auto counted = audio_thread_allocations::CountOnAudioThread([&] {
        for (double mode = 0.0; mode <= 5.0; mode += 1.0)
        {
            splitter.SetParam(modeKey, mode);
            splitter.SetParam(lowestKey, mode > 3.0 ? 1.0 : 0.0);

            for (std::size_t start = 0; start + kBlock <= phrase.size(); start += kBlock)
            {
                splitter.Route(phrase.data() + start, phrase.data() + start, 4, kBlock);
            }
        }
    });

    Check(counted.count == 0, "Route allocates nothing in any mode", audio_thread_allocations::Describe(counted));
}

// ── What the editors say ──────────────────────────────────────────────────────────────────

void TestRoutingWords()
{
    std::cout << "\nRouting line" << std::endl;
    using guitarfx::uiclient::DescribeSplitRouting;
    using guitarfx::uiclient::IsSplitterParamInUse;
    GraphNode splitter;
    splitter.type = guitarfx::EffectGuids::kSplitter;
    Check(DescribeSplitRouting(splitter, 2).empty(), "nothing to say in Copy");
    splitter.params["mode"] = 3.0;
    Check(DescribeSplitRouting(splitter, 4) ==
              "Soft playing to branch 1, hard to branch 2; branches 3 and 4 get everything",
          "Dynamics, as the web UI says it");
    splitter.params["mode"] = 4.0;
    Check(DescribeSplitRouting(splitter, 3) == "Notes to branches 1 to 3 by range, at the split notes",
          "Pitch, as the web UI says it");
    splitter.params["mode"] = 5.0;
    Check(DescribeSplitRouting(splitter, 3) == "Left to branch 1, right to branch 2; branch 3 gets both",
          "Stereo, as the web UI says it");
    Check(!IsSplitterParamInUse("splitNote2", 2) && IsSplitterParamInUse("splitNote2", 3) &&
              !IsSplitterParamInUse("splitNote3", 3) && IsSplitterParamInUse("splitNote3", 4),
          "a split note shows only when the split has a branch for it");
}
} // namespace

int main()
{
    guitarfx::RegisterAllEffects();
    std::cout << "=== Splitter routing tests ===" << std::endl;

    TestCopy();
    TestCopyInGraphUnchanged();
    TestSwitchAndBlend();
    TestDynamics();
    TestPitch();
    TestRealRiffs();
    TestStereo();
    TestNoAllocations();
    TestRoutingWords();

    std::cout << "\n" << (gChecks - gFailures) << " of " << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}
