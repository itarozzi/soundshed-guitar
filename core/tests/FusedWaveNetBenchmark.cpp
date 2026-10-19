// Times the fused WaveNet pass (dsp/nam/FusedWaveNet.h) against the NAM core it stands in for, the
// model alone, at real-time block sizes, and writes the result as a Markdown table and as JSON.
//
// Written to decide whether arm64 could run the fused pass by default, which it now does
// (GUITARFX_FUSED_WAVENET_NEON, core/CMakeLists.txt). .github/workflows/fused-benchmark.yml runs this
// on arm64 and x64 macOS, Linux and Windows. It works on any build with a fused kernel (AVX2, SSE,
// NEON).
//
// Both engines are built by NamModelCache, as the app builds them, and run in the same process with
// rounds interleaved (and their order alternated), so a shared or noisy machine moves both alike.
// The figure kept is each engine's best round. Each case also checks the fused output against the
// NAM core's, from fresh models, and fails below the 100 dB FusedWaveNetTests requires.
//
// Usage:
//   FusedWaveNetBenchmark [--block N]... [--rounds R] [--seconds S] [--sizes 1.0,0.0]
//                         [--markdown FILE] [--json FILE] [--require-kernel NAME] [model.nam]...
//
// With no models it runs a standard WaveNet and an A2 container from the test assets. A slimmable
// container runs at every size in --sizes; any other model at its only size.
// Exit status: 0, 1 if a fused output strays from the NAM core's, 2 on bad arguments or when the
// kernel is not --require-kernel's (a build that silently fell back to the NAM core).

#include "dsp/NamModelCache.h"
#include "dsp/nam/FusedWaveNet.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "NAM/activations.h"
#include "NAM/dsp.h"
#include "NAM/slimmable.h"

#if defined(__APPLE__)
    #include <pthread.h>
    #include <sys/qos.h>
#elif defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#endif

#if defined(_MSC_VER)
    #include <float.h>
#elif defined(__x86_64__) || defined(__i386__)
    #include <xmmintrin.h>
#endif

namespace fs = std::filesystem;
namespace fwn = guitarfx::fusedwavenet;
namespace cache = guitarfx::nammodelcache;
using nlohmann::json;

namespace nam::factory
{
void ForceFactoryRegistration();
}

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr double kMinMatchDb = 100.0; // FusedWaveNetTests' kMinSnrDb

std::string Utf8(const fs::path& p)
{
    const auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

struct Options
{
    std::vector<int> blocks;
    int rounds = 5;
    double seconds = 2.0;
    std::vector<double> sizes = {1.0, 0.0};
    std::vector<fs::path> models;
    fs::path markdown;
    fs::path jsonOut;
    std::string requireKernel;
};

struct Subject
{
    fs::path path;
    std::string label;
    double size = 1.0;
    bool container = false;
};

/// One engine's timing over the rounds of a case.
struct Timing
{
    double bestMeanUs = 0.0;
    double p99AtBestUs = 0.0;
    double worstMeanUs = 0.0;
    std::vector<double> roundMeansUs;
};

struct Row
{
    Subject subject;
    int block = 0;
    bool fusedEngine = false; // false when the shape is not one the fused pass takes
    double matchDb = 0.0;
    Timing core;
    Timing fused;
};

std::vector<double> ParseList(const std::string& text)
{
    std::vector<double> out;
    std::stringstream ss(text);

    for (std::string item; std::getline(ss, item, ',');)
    {
        if (!item.empty())
        {
            out.push_back(std::atof(item.c_str()));
        }
    }

    return out;
}

bool ParseArgs(int argc, char** argv, Options& o)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string{}; };

        if (a == "--block")
        {
            for (const double b : ParseList(next()))
            {
                o.blocks.push_back(static_cast<int>(b));
            }
        }
        else if (a == "--rounds")
        {
            o.rounds = std::atoi(next().c_str());
        }
        else if (a == "--seconds")
        {
            o.seconds = std::atof(next().c_str());
        }
        else if (a == "--sizes")
        {
            o.sizes = ParseList(next());
        }
        else if (a == "--markdown")
        {
            o.markdown = next();
        }
        else if (a == "--json")
        {
            o.jsonOut = next();
        }
        else if (a == "--require-kernel")
        {
            o.requireKernel = next();
        }
        else if (!a.empty() && a[0] == '-')
        {
            std::cerr << "unknown argument: " << a << "\n";
            return false;
        }
        else
        {
            o.models.emplace_back(std::u8string(a.begin(), a.end()));
        }
    }

    if (o.blocks.empty())
    {
        o.blocks = {64, 128, 512};
    }

    const bool blocksOk = std::all_of(o.blocks.begin(), o.blocks.end(), [](int b) { return b > 0 && b <= 8192; });
    return blocksOk && o.rounds > 0 && o.seconds > 0.0 && !o.sizes.empty();
}

/// The audio thread's footing: user-interactive QoS on macOS, which keeps the thread on the
/// performance cores of Apple silicon; high priority on Windows.
void RaiseThreadPriority()
{
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#elif defined(_WIN32)
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
}

/// Flush-to-zero and denormals-are-zero, as the app's audio callback runs (juce::ScopedNoDenormals).
void FlushDenormals()
{
#if defined(_MSC_VER)
    unsigned int current = 0;
    _controlfp_s(&current, _DN_FLUSH, _MCW_DN);
#elif defined(__x86_64__) || defined(__i386__)
    _mm_setcsr(_mm_getcsr() | 0x8040u);
#elif defined(__aarch64__)
    std::uint64_t fpcr = 0;
    asm volatile("mrs %0, fpcr" : "=r"(fpcr));
    asm volatile("msr fpcr, %0" : : "r"(fpcr | (1ull << 24)));
#endif
}

const char* Architecture()
{
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86-64";
#else
    return "other";
#endif
}

std::string Compiler()
{
#if defined(__clang__)
    return std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
    return "gcc " + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__);
#elif defined(_MSC_VER)
    return "MSVC " + std::to_string(_MSC_FULL_VER);
#else
    return "unknown";
#endif
}

/// FusedWaveNetTests' guitar-like signal: decaying notes plus a little noise.
std::vector<NAM_SAMPLE> MakeInput(int frames)
{
    std::vector<NAM_SAMPLE> x(static_cast<std::size_t>(frames));
    unsigned seed = 999;

    for (int i = 0; i < frames; ++i)
    {
        const double t = i / kSampleRate;
        const double env = std::exp(-std::fmod(t, 0.5) * 5.0);
        seed = seed * 1664525u + 1013904223u;
        x[static_cast<std::size_t>(i)] = static_cast<NAM_SAMPLE>(
            0.3 * env * std::sin(2 * 3.14159265358979 * 110.0 * t) +
            0.25 * env * std::sin(2 * 3.14159265358979 * 331.0 * t) + (double(seed >> 8) / double(1 << 24) - 0.5) * 0.01);
    }

    return x;
}

bool IsFused(const ::nam::DSP& model)
{
    return dynamic_cast<const fwn::FusedWaveNet*>(&model) != nullptr ||
           dynamic_cast<const fwn::FusedContainer*>(&model) != nullptr;
}

/// A model as the effect gets one: from the cache on the chosen engine, sized, then Reset for
/// the host's block size.
std::unique_ptr<::nam::DSP> Build(const Subject& s, bool fused, int block)
{
    cache::SetFusedEngineEnabled(fused);
    auto model = cache::GetModel(s.path);

    if (!model)
    {
        throw std::runtime_error("cannot load " + Utf8(s.path));
    }

    if (auto* slimmable = dynamic_cast<::nam::SlimmableModel*>(model.get()))
    {
        slimmable->SetSlimmableSize(s.size);
    }

    model->Reset(kSampleRate, block);
    return model;
}

/// Runs `blocks` host blocks from `pos` in `input` (wrapping), timing each process call.
std::vector<double> TimeBlocks(::nam::DSP& model, const std::vector<NAM_SAMPLE>& input, std::size_t& pos, int block,
                               int blocks, std::vector<NAM_SAMPLE>& in, std::vector<NAM_SAMPLE>& out)
{
    std::vector<double> us;
    us.reserve(static_cast<std::size_t>(blocks));

    for (int b = 0; b < blocks; ++b)
    {
        for (int i = 0; i < block; ++i)
        {
            in[static_cast<std::size_t>(i)] = input[pos];
            pos = (pos + 1) % input.size();
        }

        NAM_SAMPLE* inPtr[1] = {in.data()};
        NAM_SAMPLE* outPtr[1] = {out.data()};
        const auto t0 = std::chrono::steady_clock::now();
        model.process(inPtr, outPtr, block);
        const auto t1 = std::chrono::steady_clock::now();
        us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }

    return us;
}

void Record(Timing& t, std::vector<double> us)
{
    double sum = 0.0;

    for (const double v : us)
    {
        sum += v;
    }

    const double mean = sum / static_cast<double>(us.size());
    std::sort(us.begin(), us.end());
    const double p99 = us[std::min(us.size() - 1, static_cast<std::size_t>(static_cast<double>(us.size()) * 0.99))];

    if (t.roundMeansUs.empty() || mean < t.bestMeanUs)
    {
        t.bestMeanUs = mean;
        t.p99AtBestUs = p99;
    }

    t.worstMeanUs = std::max(t.worstMeanUs, mean);
    t.roundMeansUs.push_back(mean);
}

/// The fused output against the NAM core's, both from fresh models: 1 s in blocks of `block`.
double MatchDb(const Subject& s, int block, const std::vector<NAM_SAMPLE>& input)
{
    auto core = Build(s, false, block);
    auto fused = Build(s, true, block);
    std::vector<NAM_SAMPLE> in(static_cast<std::size_t>(block)), a(in.size()), b(in.size());
    const auto frames = static_cast<std::size_t>(kSampleRate);
    double signal = 0.0, error = 0.0;

    for (std::size_t done = 0; done + static_cast<std::size_t>(block) <= frames; done += static_cast<std::size_t>(block))
    {
        std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(done), block, in.begin());
        NAM_SAMPLE* inPtr[1] = {in.data()};
        NAM_SAMPLE* aPtr[1] = {a.data()};
        NAM_SAMPLE* bPtr[1] = {b.data()};
        core->process(inPtr, aPtr, block);
        fused->process(inPtr, bPtr, block);

        for (int i = 0; i < block; ++i)
        {
            const double r = a[static_cast<std::size_t>(i)];
            const double d = b[static_cast<std::size_t>(i)] - r;
            signal += r * r;
            error += d * d;
        }
    }

    return error == 0.0 ? 999.0 : 10.0 * std::log10(signal / error);
}

Row RunCase(const Subject& s, int block, const Options& o, const std::vector<NAM_SAMPLE>& input)
{
    Row row;
    row.subject = s;
    row.block = block;
    auto core = Build(s, false, block);
    auto fused = Build(s, true, block);
    row.fusedEngine = IsFused(*fused);
    row.matchDb = MatchDb(s, block, input);

    const int blocks = std::max(1, static_cast<int>(o.seconds * kSampleRate / block));
    std::vector<NAM_SAMPLE> in(static_cast<std::size_t>(block)), out(in.size());
    std::size_t corePos = 0, fusedPos = 0;

    // Warm both: caches, branch predictors, the frequency governor.
    const int warm = std::max(1, static_cast<int>(0.25 * kSampleRate / block));
    TimeBlocks(*core, input, corePos, block, warm, in, out);
    TimeBlocks(*fused, input, fusedPos, block, warm, in, out);

    for (int round = 0; round < o.rounds; ++round)
    {
        // Alternate which engine goes first, so a drift in the machine's speed favours neither.
        if (round % 2 == 0)
        {
            Record(row.core, TimeBlocks(*core, input, corePos, block, blocks, in, out));
            Record(row.fused, TimeBlocks(*fused, input, fusedPos, block, blocks, in, out));
        }
        else
        {
            Record(row.fused, TimeBlocks(*fused, input, fusedPos, block, blocks, in, out));
            Record(row.core, TimeBlocks(*core, input, corePos, block, blocks, in, out));
        }
    }

    return row;
}

std::vector<Subject> Subjects(const Options& o)
{
    std::vector<fs::path> paths = o.models;

    if (paths.empty())
    {
        const fs::path amps = fs::path(GUITARFX_TEST_RESOURCES_DIR) / "assets" / "amps" / "Guitar";
        paths = {amps / "TimR" / "JCM800 2203 1985" / "JCM800 Hi P6 B8 M4 T7 G6.nam",
                 amps / "A2" / "Boosted-6505+-A2-Chugs-FullRig.nam"};
    }

    std::vector<Subject> subjects;

    for (const auto& path : paths)
    {
        cache::SetFusedEngineEnabled(false);
        auto probe = cache::GetModel(path);

        if (!probe)
        {
            throw std::runtime_error("cannot load " + Utf8(path));
        }

        const bool container = dynamic_cast<::nam::SlimmableModel*>(probe.get()) != nullptr;
        const std::string name = Utf8(path.filename());

        for (const double size : container ? o.sizes : std::vector<double>{1.0})
        {
            subjects.push_back({path, name, size, container});
        }
    }

    return subjects;
}

std::string Fixed(double v, int places)
{
    char text[64];
    std::snprintf(text, sizeof(text), "%.*f", places, v);
    return text;
}

double LoadPercent(double us, int block)
{
    return 100.0 * us / (1e6 * block / kSampleRate);
}

std::string SizeLabel(const Subject& s)
{
    return s.container ? Fixed(s.size, 1) : "-";
}

void WriteMarkdown(const fs::path& file, const std::vector<Row>& rows, const Options& o)
{
    std::ofstream f(file, std::ios::binary);
    f << "### Fused WaveNet (" << fwn::KernelName() << ") vs the NAM core\n\n";
    f << Architecture() << ", " << Compiler() << ". " << kSampleRate / 1000.0 << " kHz, mono, model alone. Best of "
      << o.rounds << " interleaved rounds of " << Fixed(o.seconds, 1)
      << " s each. Load is the share of the block's real-time budget.\n\n";
    f << "| Model | Size | Block | NAM core µs | Fused µs | Speedup | NAM core load | Fused load | Fused p99 µs | "
         "Match dB |\n";
    f << "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|\n";

    for (const auto& r : rows)
    {
        const std::string fusedCell = r.fusedEngine ? Fixed(r.fused.bestMeanUs, 2) : "not fused";
        f << "| " << r.subject.label << " | " << SizeLabel(r.subject) << " | " << r.block << " | "
          << Fixed(r.core.bestMeanUs, 2) << " | " << fusedCell << " | **"
          << Fixed(r.core.bestMeanUs / r.fused.bestMeanUs, 2) << "x** | " << Fixed(LoadPercent(r.core.bestMeanUs, r.block), 1)
          << "% | " << Fixed(LoadPercent(r.fused.bestMeanUs, r.block), 1) << "% | " << Fixed(r.fused.p99AtBestUs, 1)
          << " | " << (r.matchDb >= 999.0 ? "identical" : Fixed(r.matchDb, 1)) << (r.matchDb < kMinMatchDb ? " ✗" : "")
          << " |\n";
    }

    double coreSpread = 1.0;
    double fusedSpread = 1.0;

    for (const auto& r : rows)
    {
        coreSpread = std::max(coreSpread, r.core.worstMeanUs / r.core.bestMeanUs);
        fusedSpread = std::max(fusedSpread, r.fused.worstMeanUs / r.fused.bestMeanUs);
    }

    f << "\nRound spread, worst round over best: NAM core up to " << Fixed(coreSpread, 2) << "x, fused up to "
      << Fixed(fusedSpread, 2) << "x. A wide spread means the machine was busy: trust the speedup over the "
      << "absolute times.\n";
}

json ToJson(const std::vector<Row>& rows, const Options& o)
{
    json results = json::array();

    for (const auto& r : rows)
    {
        results.push_back({{"model", r.subject.label},
                           {"path", Utf8(r.subject.path)},
                           {"size", r.subject.size},
                           {"container", r.subject.container},
                           {"block", r.block},
                           {"fusedEngine", r.fusedEngine},
                           {"matchDb", r.matchDb},
                           {"speedup", r.core.bestMeanUs / r.fused.bestMeanUs},
                           {"core", {{"bestMeanUs", r.core.bestMeanUs}, {"p99AtBestUs", r.core.p99AtBestUs},
                                     {"roundMeansUs", r.core.roundMeansUs}}},
                           {"fused", {{"bestMeanUs", r.fused.bestMeanUs}, {"p99AtBestUs", r.fused.p99AtBestUs},
                                      {"roundMeansUs", r.fused.roundMeansUs}}}});
    }

    return {{"kernel", fwn::KernelName()},  {"architecture", Architecture()}, {"compiler", Compiler()},
            {"sampleRate", kSampleRate},    {"rounds", o.rounds},             {"secondsPerRound", o.seconds},
            {"results", std::move(results)}};
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        Options o;

        if (!ParseArgs(argc, argv, o))
        {
            std::cerr << "usage: FusedWaveNetBenchmark [--block N[,N...]] [--rounds R] [--seconds S] [--sizes 1.0,0.0]\n"
                         "                             [--markdown FILE] [--json FILE] [--require-kernel NAME] "
                         "[model.nam]...\n";
            return 2;
        }

        nam::factory::ForceFactoryRegistration();
        // The app's NAM core runs fast tanh (NamModelCache switches it on), and so does the fused pass.
        ::nam::activations::Activation::enable_fast_tanh();
        RaiseThreadPriority();
        FlushDenormals();

        std::cout << "Fused WaveNet kernel: " << fwn::KernelName() << " (" << Architecture() << ", " << Compiler()
                  << ")\n";

        if (!o.requireKernel.empty() && o.requireKernel != fwn::KernelName())
        {
            std::cerr << "ERROR: expected the " << o.requireKernel << " kernel, this build has "
                      << fwn::KernelName() << "\n";
            return 2;
        }

        if (!fwn::IsAvailable())
        {
            std::cout << "No fused kernel in this build, so both columns are the NAM core. On arm64, it was "
                         "configured with -DGUITARFX_FUSED_WAVENET_NEON=OFF.\n";
        }

        const auto input = MakeInput(static_cast<int>(kSampleRate) * 4);
        std::vector<Row> rows;
        bool allMatch = true;

        for (const auto& subject : Subjects(o))
        {
            for (const int block : o.blocks)
            {
                rows.push_back(RunCase(subject, block, o, input));
                const auto& r = rows.back();
                allMatch = allMatch && r.matchDb >= kMinMatchDb;
                std::printf("%-44s size %-3s block %4d  core %8.2f us  fused %8.2f us%s  %5.2fx  match %s dB\n",
                            subject.label.c_str(), SizeLabel(subject).c_str(), block, r.core.bestMeanUs,
                            r.fused.bestMeanUs, r.fusedEngine ? "" : " (not fused)",
                            r.core.bestMeanUs / r.fused.bestMeanUs,
                            r.matchDb >= 999.0 ? "identical" : Fixed(r.matchDb, 1).c_str());
                std::fflush(stdout);
            }
        }

        cache::SetFusedEngineEnabled(true);

        if (!o.markdown.empty())
        {
            WriteMarkdown(o.markdown, rows, o);
        }

        if (!o.jsonOut.empty())
        {
            std::ofstream f(o.jsonOut, std::ios::binary);
            f << ToJson(rows, o).dump(2) << "\n";
        }

        if (!allMatch)
        {
            std::cerr << "ERROR: a fused output is below " << kMinMatchDb << " dB from the NAM core's\n";
            return 1;
        }

        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 2;
    }
}
