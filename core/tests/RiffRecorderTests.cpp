// RiffRecorder: riff capture on the looper's recorder. A take must hold exactly the samples played,
// in both channels, from the end of the count-in: what the old recorder wrote, so takes saved to a
// riff library are the same as before. An armed take also keeps 10 ms ahead of its first note.

#include "controller/RiffRecorder.h"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

using namespace guitarfx;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 100; // not a divisor of the bar, so events land mid-block
constexpr double kBar = 24000.0;

int gFailures = 0;

void Check(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << what << std::endl;
        ++gFailures;
    }
}

float Left(std::int64_t t)
{
    return static_cast<float>(0.4 * std::sin(static_cast<double>(t) * 0.013));
}

float Right(std::int64_t t)
{
    return static_cast<float>(0.3 * std::cos(static_cast<double>(t) * 0.007));
}

/// Feeds the recorder blocks of the two test signals (silence before `soundFrom`).
struct Feed
{
    RiffRecorder& recorder;
    std::int64_t now = 0;
    std::int64_t soundFrom = 0;
    int ends = 0;

    void Run(std::int64_t samples)
    {
        std::vector<float> l(kBlock);
        std::vector<float> r(kBlock);

        while (samples > 0)
        {
            const int n = static_cast<int>(std::min<std::int64_t>(kBlock, samples));

            for (int i = 0; i < n; ++i)
            {
                const std::int64_t t = now + i;
                l[static_cast<std::size_t>(i)] = t >= soundFrom ? Left(t) : 0.0f;
                r[static_cast<std::size_t>(i)] = t >= soundFrom ? Right(t) : 0.0f;
            }

            float* inputs[2] = {l.data(), r.data()};
            ends += recorder.Process(inputs, n) ? 1 : 0;
            now += n;
            samples -= n;
        }
    }
};

bool Exact(const RiffRecorder::Audio& audio, std::int64_t from, std::int64_t count)
{
    if (static_cast<std::int64_t>(audio.left.size()) != count || static_cast<std::int64_t>(audio.right.size()) != count)
    {
        return false;
    }

    for (std::int64_t k = 0; k < count; ++k)
    {
        if (audio.left[static_cast<std::size_t>(k)] != Left(from + k) ||
            audio.right[static_cast<std::size_t>(k)] != Right(from + k))
        {
            return false;
        }
    }

    return true;
}

/// Counted in, then recording the bars asked for: it fills up and stops on its own.
void TestCountedInTakeIsExact()
{
    RiffRecorder recorder;
    recorder.Prepare(kSampleRate, kBlock);
    Feed feed{recorder};
    feed.Run(1000);
    const auto takeLength = static_cast<std::size_t>(2 * kBar);
    recorder.Start(std::vector<float>(takeLength), std::vector<float>(takeLength), false, kBar, 1);
    Check(recorder.Busy(), "counting in");
    feed.Run(static_cast<std::int64_t>(kBar));
    Check(!recorder.Poll().started, "nothing recorded during the count-in");
    feed.Run(static_cast<std::int64_t>(2 * kBar) + 500);
    const auto status = recorder.Poll();
    Check(status.finished && status.recorded == static_cast<std::int64_t>(takeLength), "a full take stops itself");
    Check(feed.ends == 1, "the block it filled up on says so, once");
    Check(!recorder.Busy(), "and the recorder is free");
    auto audio = recorder.Release();
    audio.Rotate();
    Check(Exact(audio, 1000 + static_cast<std::int64_t>(kBar), static_cast<std::int64_t>(takeLength)),
          "the take is the input from the end of the count-in, sample for sample, both channels");
    Check(!recorder.HasTake(), "released");
}

/// Stopped early, the take is what was played up to the stop.
void TestStoppedEarlyKeepsWhatWasPlayed()
{
    RiffRecorder recorder;
    recorder.Prepare(kSampleRate, kBlock);
    Feed feed{recorder};
    const auto longest = static_cast<std::size_t>(4 * kBar);
    recorder.Start(std::vector<float>(longest), std::vector<float>(longest), false, kBar, 0);
    feed.Run(30000);
    Check(recorder.Close(), "a stop closes the take");
    auto audio = recorder.Release();
    audio.Rotate();
    Check(Exact(audio, 0, 30000), "and keeps all of it");
}

/// Armed: counted in, then listening; the take starts 10 ms before the first note, and a ring that
/// wrapped while it waited comes back in order.
void TestArmedTakeAfterCountIn()
{
    RiffRecorder recorder;
    recorder.Prepare(kSampleRate, kBlock);
    Feed feed{recorder};
    const std::int64_t armed = 1000;
    const auto longest = static_cast<std::size_t>(kBar);
    // Waits more than the whole buffer before the note, so the ring wraps.
    feed.soundFrom = armed + static_cast<std::int64_t>(kBar) + 30000 + 77;
    feed.Run(armed);
    recorder.Start(std::vector<float>(longest), std::vector<float>(longest), true, kBar, 1);
    feed.Run(feed.soundFrom - armed + 10000);
    const auto started = recorder.Poll();
    Check(started.started && !started.finished, "the note starts the take");
    const std::int64_t preRoll = 480;
    Check(started.armLead == feed.soundFrom - preRoll - (armed + static_cast<std::int64_t>(kBar)),
          "the take starts 10 ms ahead of the note, counted from the end of the count-in");
    feed.Run(static_cast<std::int64_t>(kBar));
    Check(recorder.Poll().finished, "it stops when it fills up");
    auto audio = recorder.Release();
    audio.Rotate();
    bool preRollSilent = true;

    for (std::int64_t k = 0; k < preRoll; ++k)
    {
        preRollSilent = preRollSilent && audio.left[static_cast<std::size_t>(k)] == 0.0f;
    }

    Check(preRollSilent, "the pre-roll is what came just before the note");
    bool exact = static_cast<std::int64_t>(audio.left.size()) == static_cast<std::int64_t>(kBar);

    for (std::int64_t k = preRoll; exact && k < static_cast<std::int64_t>(kBar); ++k)
    {
        const std::int64_t t = feed.soundFrom - preRoll + k;
        exact = audio.left[static_cast<std::size_t>(k)] == Left(t) && audio.right[static_cast<std::size_t>(k)] == Right(t);
    }

    Check(exact, "then the note on, in order, though the ring wrapped");
}

/// A stop before an armed take hears anything leaves nothing to keep.
void TestArmedStopBeforeANote()
{
    RiffRecorder recorder;
    recorder.Prepare(kSampleRate, kBlock);
    Feed feed{recorder};
    feed.soundFrom = 1 << 30;
    const auto longest = static_cast<std::size_t>(kBar);
    recorder.Start(std::vector<float>(longest), std::vector<float>(longest), true, kBar, 0);
    feed.Run(20000);
    Check(recorder.Busy() && !recorder.Poll().started, "armed, waiting");
    Check(!recorder.Close(), "a stop has nothing to close");
    const auto audio = recorder.Release();
    Check(audio.length == 0 && !recorder.Busy(), "and the recorder is free");
}
} // namespace

int main()
{
    TestCountedInTakeIsExact();
    TestStoppedEarlyKeepsWhatWasPlayed();
    TestArmedTakeAfterCountIn();
    TestArmedStopBeforeANote();

    if (gFailures > 0)
    {
        std::cerr << gFailures << " check(s) failed" << std::endl;
        return 1;
    }

    std::cout << "RiffRecorderTests passed" << std::endl;
    return 0;
}
