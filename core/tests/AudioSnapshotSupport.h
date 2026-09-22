#pragma once

// Stimuli, WAV I/O, the block driver and the "freshly added node" parameters for AudioSnapshot.
//
// tools/audio-ab copies the harness (this header, AudioSnapshotChains.h and AudioSnapshot.cpp)
// into older checkouts so that both sides of a comparison run the same harness, so everything
// here sticks to APIs that have
// been stable since 1.5.0. Where a newer field or method is useful, it is reached through a
// `requires` check in a template, which compiles away against a revision that lacks it.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    #include <xmmintrin.h>
    #define AUDIO_SNAPSHOT_MXCSR 1
#endif

namespace audio_snapshot
{
namespace fs = std::filesystem;

constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------- stimuli

/// What goes into a chain: the samples, and where the stimulus itself ends and the
/// silence left for tails to ring out begins.
struct Stimulus
{
    std::string id;
    std::vector<float> samples;
    std::size_t playFrames = 0;
};

/// First channel of a PCM (16/24/32-bit) or float WAV, as doubles in -1..1.
inline std::vector<double> ReadWavMono(const fs::path& file, double& sampleRateOut)
{
    std::vector<double> out;
    std::ifstream f(file, std::ios::binary);

    if (!f)
    {
        return out;
    }

    const std::vector<char> all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    if (all.size() < 44 || std::memcmp(all.data(), "RIFF", 4) != 0 || std::memcmp(all.data() + 8, "WAVE", 4) != 0)
    {
        return out;
    }

    const auto u32 = [&](std::size_t p) {
        std::uint32_t v = 0;
        std::memcpy(&v, all.data() + p, 4);
        return v;
    };
    const auto u16 = [&](std::size_t p) {
        std::uint16_t v = 0;
        std::memcpy(&v, all.data() + p, 2);
        return v;
    };

    std::size_t p = 12;
    int format = 1;
    int channels = 1;
    int bits = 16;

    while (p + 8 <= all.size())
    {
        const std::string id(all.data() + p, 4);
        const std::uint32_t size = u32(p + 4);

        if (id == "fmt " && size >= 16)
        {
            format = u16(p + 8);
            channels = std::max<int>(1, u16(p + 10));
            sampleRateOut = static_cast<double>(u32(p + 12));
            bits = u16(p + 22);

            if (format == 0xFFFE && size >= 26)
            {
                format = u16(p + 32); // WAVE_FORMAT_EXTENSIBLE: the sub-format GUID starts with the tag
            }
        }
        else if (id == "data")
        {
            const std::size_t bytes = std::min<std::size_t>(size, all.size() - (p + 8));
            const std::size_t stride = static_cast<std::size_t>(channels) * static_cast<std::size_t>(bits / 8);
            const std::size_t frames = stride > 0 ? bytes / stride : 0;
            const char* base = all.data() + p + 8;
            out.resize(frames);

            for (std::size_t i = 0; i < frames; ++i)
            {
                const char* s = base + i * stride;

                if (format == 3 && bits == 32)
                {
                    float v = 0.0f;
                    std::memcpy(&v, s, 4);
                    out[i] = v;
                }
                else if (bits == 16)
                {
                    std::int16_t v = 0;
                    std::memcpy(&v, s, 2);
                    out[i] = v / 32768.0;
                }
                else if (bits == 24)
                {
                    const auto* b = reinterpret_cast<const unsigned char*>(s);
                    const std::int32_t v = static_cast<std::int32_t>((static_cast<std::uint32_t>(b[2]) << 24) |
                                                                     (static_cast<std::uint32_t>(b[1]) << 16) |
                                                                     (static_cast<std::uint32_t>(b[0]) << 8));
                    out[i] = (v >> 8) / 8388608.0;
                }
                else if (bits == 32)
                {
                    std::int32_t v = 0;
                    std::memcpy(&v, s, 4);
                    out[i] = v / 2147483648.0;
                }
            }

            break;
        }

        p += 8 + size + (size & 1u);
    }

    return out;
}

/// Four-point Hermite resampling. Only the stimulus goes through this, and both builds get
/// the same stimulus, so its own small high-frequency droop cannot show up as a difference.
inline std::vector<double> Resample(const std::vector<double>& in, double fromRate, double toRate)
{
    if (in.empty() || fromRate <= 0.0 || fromRate == toRate)
    {
        return in;
    }

    const double step = fromRate / toRate;
    const auto frames = static_cast<std::size_t>(static_cast<double>(in.size()) / step);
    std::vector<double> out(frames, 0.0);
    const auto at = [&](long long i) {
        return (i >= 0 && i < static_cast<long long>(in.size())) ? in[static_cast<std::size_t>(i)] : 0.0;
    };

    for (std::size_t n = 0; n < frames; ++n)
    {
        const double pos = static_cast<double>(n) * step;
        const auto i = static_cast<long long>(pos);
        const double t = pos - static_cast<double>(i);
        const double y0 = at(i - 1), y1 = at(i), y2 = at(i + 1), y3 = at(i + 2);
        const double c1 = 0.5 * (y2 - y0);
        const double c2 = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3;
        const double c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
        out[n] = ((c3 * t + c2) * t + c1) * t + y1;
    }

    return out;
}

inline Stimulus Finish(std::string id, const std::vector<double>& play, double sampleRate, double tailSeconds)
{
    Stimulus s;
    s.id = std::move(id);
    s.playFrames = play.size();
    s.samples.assign(play.size() + static_cast<std::size_t>(sampleRate * tailSeconds), 0.0f);
    std::transform(play.begin(), play.end(), s.samples.begin(), [](double v) { return static_cast<float>(v); });
    return s;
}

inline double DbToGain(double dB)
{
    return std::pow(10.0, dB / 20.0);
}

/// Exponential sine sweep, 20 Hz to 20 kHz by default.
inline std::vector<double> MakeSweep(double sampleRate, double seconds, double f0, double f1, double peakDb)
{
    const auto n = static_cast<std::size_t>(sampleRate * seconds);
    const double k = std::log(f1 / f0);
    const double peak = DbToGain(peakDb);
    std::vector<double> out(n, 0.0);

    for (std::size_t i = 0; i < n; ++i)
    {
        const double t = static_cast<double>(i) / sampleRate;
        out[i] = peak * std::sin(2.0 * kPi * f0 * seconds / k * (std::exp(k * t / seconds) - 1.0));
    }

    // 5 ms raised-cosine fades, so the sweep's own start and stop are not clicks.
    const auto fade = std::min<std::size_t>(n / 2, static_cast<std::size_t>(sampleRate * 0.005));

    for (std::size_t i = 0; i < fade; ++i)
    {
        const double g = 0.5 - 0.5 * std::cos(kPi * static_cast<double>(i) / static_cast<double>(fade));
        out[i] *= g;
        out[n - 1 - i] *= g;
    }

    return out;
}

/// Uniform white noise from a fixed-seed LCG, so every build sees the same samples.
inline std::vector<double> MakeNoise(double sampleRate, double seconds, double rmsDb)
{
    std::uint64_t state = 0x243F6A8885A308D3ull;
    const double scale = DbToGain(rmsDb) * std::sqrt(3.0); // uniform in [-1,1] has an rms of 1/sqrt(3)
    std::vector<double> out(static_cast<std::size_t>(sampleRate * seconds), 0.0);

    for (auto& v : out)
    {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        v = (static_cast<double>(static_cast<std::uint32_t>(state >> 32)) / 2147483648.0 - 1.0) * scale;
    }

    return out;
}

/// The stimulus battery. `di` is the demo DI guitar resampled to the render rate; the rest
/// are generated. Each carries a tail of silence so delays and reverbs are heard ringing out.
inline bool MakeStimulus(const std::string& id, double sampleRate, const std::vector<double>& di, Stimulus& out)
{
    if (id == "di" || id == "di_hot")
    {
        // Six seconds of playing from 12 s in, past the count-in.
        const auto from = static_cast<std::size_t>(sampleRate * 12.0);
        const auto count = static_cast<std::size_t>(sampleRate * 6.0);

        if (di.size() < from + count)
        {
            return false;
        }

        std::vector<double> play(di.begin() + static_cast<std::ptrdiff_t>(from),
                                 di.begin() + static_cast<std::ptrdiff_t>(from + count));

        // Faded in and out (10 and 50 ms), so the slice's own edges are not steps: a step
        // into a filter rings, and the tail would measure the cut rather than the effect.
        const auto fadeIn = static_cast<std::size_t>(sampleRate * 0.01);
        const auto fadeOut = static_cast<std::size_t>(sampleRate * 0.05);

        for (std::size_t i = 0; i < fadeIn; ++i)
        {
            play[i] *= 0.5 - 0.5 * std::cos(kPi * static_cast<double>(i) / static_cast<double>(fadeIn));
        }

        for (std::size_t i = 0; i < fadeOut; ++i)
        {
            play[count - 1 - i] *= 0.5 - 0.5 * std::cos(kPi * static_cast<double>(i) / static_cast<double>(fadeOut));
        }

        if (id == "di_hot")
        {
            for (auto& v : play)
            {
                v *= DbToGain(12.0); // pushes drives and amps harder than a nominal pickup
            }
        }

        out = Finish(id, play, sampleRate, 2.0);
        return true;
    }

    if (id == "sweep")
    {
        out = Finish(id, MakeSweep(sampleRate, 4.0, 20.0, 20000.0, -12.0), sampleRate, 1.0);
        return true;
    }

    if (id == "impulse")
    {
        std::vector<double> play(static_cast<std::size_t>(sampleRate * 0.01) + 1, 0.0);
        play.back() = DbToGain(-6.0);
        out = Finish(id, play, sampleRate, 1.5);
        return true;
    }

    if (id == "silence")
    {
        out = Finish(id, {}, sampleRate, 1.5);
        return true;
    }

    if (id == "noise")
    {
        out = Finish(id, MakeNoise(sampleRate, 3.0, -20.0), sampleRate, 1.0);
        return true;
    }

    return false;
}

// ---------------------------------------------------------------- rendering

struct Render
{
    std::vector<float> left;
    std::vector<float> right;
    long processFailures = 0; // blocks the engine declined to run (a controller that could not take its lock)
};

/// Bit test, because /fp:fast and -ffast-math fold std::isfinite away.
inline bool IsNonFinite(float v)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    return (bits & 0x7F800000u) == 0x7F800000u;
}

/// Flush-to-zero and denormals-are-zero for the scope, as juce::ScopedNoDenormals gives the
/// app's audio callback (PluginProcessorAdapter.cpp), so a decay ends where it does in the app.
class ScopedFlushDenormals
{
  public:
    ScopedFlushDenormals()
    {
#if defined(AUDIO_SNAPSHOT_MXCSR)
        mSaved = _mm_getcsr();
        _mm_setcsr(static_cast<unsigned int>(mSaved) | 0x8040u);
#elif defined(__aarch64__)
        std::uint64_t fpcr = 0;
        asm volatile("mrs %0, fpcr" : "=r"(fpcr));
        mSaved = fpcr;
        asm volatile("msr fpcr, %0" : : "r"(fpcr | (1ull << 24)));
#endif
    }

    ~ScopedFlushDenormals()
    {
#if defined(AUDIO_SNAPSHOT_MXCSR)
        _mm_setcsr(static_cast<unsigned int>(mSaved));
#elif defined(__aarch64__)
        asm volatile("msr fpcr, %0" : : "r"(mSaved));
#endif
    }

    ScopedFlushDenormals(const ScopedFlushDenormals&) = delete;
    ScopedFlushDenormals& operator=(const ScopedFlushDenormals&) = delete;

  private:
    std::uint64_t mSaved = 0;
};

/// Feeds `input` (mono, duplicated to both channels) through `process` in blocks of `block`.
template <typename ProcessFn> Render RenderBlocks(const std::vector<float>& input, int block, ProcessFn&& process)
{
    const ScopedFlushDenormals noDenormals;
    Render r;
    r.left.assign(input.size(), 0.0f);
    r.right.assign(input.size(), 0.0f);
    const auto blockSize = static_cast<std::size_t>(block);
    std::vector<float> inL(blockSize), inR(blockSize), outL(blockSize), outR(blockSize);

    for (std::size_t done = 0; done < input.size();)
    {
        const std::size_t count = std::min(blockSize, input.size() - done);
        std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(done), count, inL.begin());
        std::copy_n(inL.begin(), count, inR.begin());
        std::fill(outL.begin(), outL.end(), 0.0f);
        std::fill(outR.begin(), outR.end(), 0.0f);
        float* ins[2] = {inL.data(), inR.data()};
        float* outs[2] = {outL.data(), outR.data()};

        if (!process(ins, outs, static_cast<int>(count)))
        {
            ++r.processFailures;
        }

        std::copy_n(outL.begin(), count, r.left.begin() + static_cast<std::ptrdiff_t>(done));
        std::copy_n(outR.begin(), count, r.right.begin() + static_cast<std::ptrdiff_t>(done));
        done += count;
    }

    return r;
}

inline bool SameSamples(const Render& a, const Render& b)
{
    return a.left.size() == b.left.size() &&
           std::memcmp(a.left.data(), b.left.data(), a.left.size() * sizeof(float)) == 0 &&
           std::memcmp(a.right.data(), b.right.data(), a.right.size() * sizeof(float)) == 0;
}

inline bool IsDualMono(const Render& r)
{
    return std::memcmp(r.left.data(), r.right.data(), r.left.size() * sizeof(float)) == 0;
}

/// Peak, rms and non-finite count over both channels.
inline nlohmann::json MeasureRender(const Render& r)
{
    double peak = 0.0, sumSq = 0.0;
    long nonFinite = 0;

    for (const auto* ch : {&r.left, &r.right})
    {
        for (float v : *ch)
        {
            if (IsNonFinite(v))
            {
                ++nonFinite;
                continue;
            }

            peak = std::max(peak, std::fabs(static_cast<double>(v)));
            sumSq += static_cast<double>(v) * v;
        }
    }

    const double n = std::max<double>(1.0, 2.0 * static_cast<double>(r.left.size()));
    return {{"peak", peak}, {"rms", std::sqrt(sumSq / n)}, {"nonFinite", nonFinite}};
}

/// 32-bit float WAV, one channel when both are bit-identical (most effects are dual mono).
inline bool WriteWav(const fs::path& file, const Render& r, double sampleRate, bool mono)
{
    const std::uint16_t channels = mono ? 1 : 2;
    const auto frames = static_cast<std::uint32_t>(r.left.size());
    const std::uint32_t dataBytes = frames * channels * 4u;
    std::vector<float> interleaved(static_cast<std::size_t>(frames) * channels);

    for (std::size_t i = 0; i < frames; ++i)
    {
        interleaved[i * channels] = r.left[i];

        if (!mono)
        {
            interleaved[i * 2 + 1] = r.right[i];
        }
    }

    fs::create_directories(file.parent_path());
    std::ofstream f(file, std::ios::binary);
    const auto put32 = [&](std::uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    const auto put16 = [&](std::uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };

    f.write("RIFF", 4);
    put32(4 + (8 + 18) + (8 + 4) + (8 + dataBytes));
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    put32(18);
    put16(3); // WAVE_FORMAT_IEEE_FLOAT
    put16(channels);
    put32(static_cast<std::uint32_t>(sampleRate));
    put32(static_cast<std::uint32_t>(sampleRate) * channels * 4u);
    put16(static_cast<std::uint16_t>(channels * 4u));
    put16(32);
    put16(0);
    f.write("fact", 4);
    put32(4);
    put32(frames);
    f.write("data", 4);
    put32(dataBytes);
    f.write(reinterpret_cast<const char*>(interleaved.data()), static_cast<std::streamsize>(dataBytes));
    return static_cast<bool>(f);
}

// ---------------------------------------------------------------- effect metadata

/// The parameters PluginController gives a node the user has just added
/// (PluginControllerSignalPath.cpp): every registered default, then the preset the effect
/// nominates as its starting point, else its first factory preset. `isDefault` arrived after
/// 1.5.0, so against 1.5.0 this falls straight through to the first factory preset.
template <typename TypeInfo> std::map<std::string, double> FreshNodeParams(const TypeInfo& info)
{
    std::map<std::string, double> params;

    for (const auto& p : info.parameters)
    {
        params[p.id] = p.defaultValue;
    }

    auto preset = info.presets.end();

    if constexpr (requires { info.presets.front().isDefault; })
    {
        preset = std::find_if(info.presets.begin(), info.presets.end(),
                              [](const auto& candidate) { return candidate.isFactory && candidate.isDefault; });
    }

    if (preset == info.presets.end())
    {
        preset = std::find_if(info.presets.begin(), info.presets.end(),
                              [](const auto& candidate) { return candidate.isFactory; });
    }

    if (preset != info.presets.end())
    {
        for (const auto& [key, value] : preset->parameters)
        {
            params[key] = value;
        }
    }

    return params;
}

/// The name of the factory preset FreshNodeParams applied, or empty.
template <typename TypeInfo> std::string FreshNodePresetId(const TypeInfo& info)
{
    if constexpr (requires { info.presets.front().isDefault; })
    {
        for (const auto& p : info.presets)
        {
            if (p.isFactory && p.isDefault)
            {
                return p.id;
            }
        }
    }

    for (const auto& p : info.presets)
    {
        if (p.isFactory)
        {
            return p.id;
        }
    }

    return {};
}

template <typename ParamDef> nlohmann::json DescribeParam(const ParamDef& p)
{
    nlohmann::json j = {{"id", p.id},        {"name", p.displayName}, {"default", p.defaultValue},
                        {"min", p.minValue}, {"max", p.maxValue},     {"unit", p.unit},
                        {"step", p.step},    {"labels", p.labels}};

    if constexpr (requires { p.taper; })
    {
        j["taper"] = static_cast<int>(p.taper);
    }

    return j;
}

inline bool LooksLikeUuid(const std::string& s)
{
    return s.size() == 36 && s[8] == '-' && s[13] == '-' && s[18] == '-' && s[23] == '-';
}

/// A file-name-safe label for an effect: its first readable alias, else its display name.
template <typename TypeInfo> std::string EffectSlug(const TypeInfo& info)
{
    std::string name;

    for (const auto& alias : info.aliases)
    {
        if (!LooksLikeUuid(alias))
        {
            name = alias;
            break;
        }
    }

    if (name.empty())
    {
        name = info.displayName.empty() ? info.type : info.displayName;
    }

    for (auto& c : name)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_')
        {
            c = '_';
        }
    }

    return name;
}

/// A path from UTF-8 text. JSON strings are UTF-8, and fs::path(std::string) would read them in
/// the ANSI code page on Windows.
inline fs::path Utf8Path(const std::string& s)
{
    return fs::path(std::u8string(s.begin(), s.end()));
}

/// Points the settings root at `root`, so a controller never reads or writes the real profile.
inline void SetSettingsEnvRoot(const fs::path& root)
{
#ifdef _WIN32
    _putenv_s("APPDATA", root.string().c_str());
    _putenv_s("LOCALAPPDATA", (root / "local").string().c_str());
#else
    setenv("HOME", root.string().c_str(), 1);
    setenv("XDG_CONFIG_HOME", (root / ".config").string().c_str(), 1);
#endif
}
} // namespace audio_snapshot
