#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "dsp/IRWavLoader.h"
#include "util/Wav.h"

#ifndef GUITARFX_TEST_RESOURCES_DIR
    #error "GUITARFX_TEST_RESOURCES_DIR must be defined"
#endif

namespace fs = std::filesystem;

namespace
{
std::vector<std::uint8_t> ReadBinaryFile(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);

    if (!input)
    {
        return {};
    }

    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool ReportCheck(bool condition, const std::string& message)
{
    std::cout << message << (condition ? " PASS" : " FAIL") << '\n';
    return condition;
}

bool TestDecodeRegressionFixture()
{
    const fs::path resourcesDir = fs::path(GUITARFX_TEST_RESOURCES_DIR);
    const fs::path wavPath = resourcesDir / "assets" / "wav" / "M1F1-uint8WE-AFsp(1).wav";

    std::cout << "Test: Decode extensible 8-bit stereo WAV fixture\n";
    std::cout << "Fixture: " << wavPath << '\n';

    bool passed = true;
    passed &= ReportCheck(fs::exists(wavPath), "  fixture exists:");

    if (!passed)
    {
        return false;
    }

    const auto bytes = ReadBinaryFile(wavPath);
    passed &= ReportCheck(!bytes.empty(), "  fixture bytes loaded:");

    if (!passed)
    {
        return false;
    }

    const auto decoded = guitarfx::util::DecodePcmWav(bytes);
    passed &= ReportCheck(decoded.has_value(), "  DecodePcmWav accepts WAVE_FORMAT_EXTENSIBLE PCM:");

    if (!decoded)
    {
        return false;
    }

    passed &= ReportCheck(decoded->channels == 2, "  channel count is stereo:");
    passed &= ReportCheck(static_cast<int>(decoded->sampleRate) == 8000, "  sample rate is 8000 Hz:");
    passed &= ReportCheck(decoded->bitsPerSample == 8, "  bit depth is 8-bit:");
    passed &= ReportCheck(decoded->channelSamples.size() == 2, "  decoded channel buffers present:");
    passed &= ReportCheck(!decoded->channelSamples.empty() && decoded->channelSamples[0].size() == 23493,
                          "  decoded frame count matches expected fixture:");

    if (passed)
    {
        const double firstLeft = decoded->channelSamples[0].empty() ? 0.0 : decoded->channelSamples[0].front();
        const double firstRight = decoded->channelSamples[1].empty() ? 0.0 : decoded->channelSamples[1].front();
        std::cout << "  first samples: L=" << firstLeft << ", R=" << firstRight << '\n';
    }

    return passed;
}

// ── The IR loader (dsp/IRWavLoader.h) ───────────────────────────────────────

void PutBE16(std::vector<std::uint8_t>& out, std::uint16_t v)
{
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

void PutBE32(std::vector<std::uint8_t>& out, std::uint32_t v)
{
    PutBE16(out, static_cast<std::uint16_t>(v >> 16));
    PutBE16(out, static_cast<std::uint16_t>(v));
}

void PutLE16(std::vector<std::uint8_t>& out, std::uint16_t v)
{
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}

void PutLE32(std::vector<std::uint8_t>& out, std::uint32_t v)
{
    PutLE16(out, static_cast<std::uint16_t>(v));
    PutLE16(out, static_cast<std::uint16_t>(v >> 16));
}

void PutTag(std::vector<std::uint8_t>& out, const char* tag)
{
    out.insert(out.end(), tag, tag + 4);
}

constexpr std::int16_t kSamples[] = {16384, -16384, 8192, 0};

/// Writes `bytes` to a scratch file named `name` and loads it through the IR loader.
bool LoadIr(const std::string& name, const std::vector<std::uint8_t>& bytes, guitarfx::IRWavData& out)
{
    const fs::path dir = fs::temp_directory_path() / "guitarfx-ir-loader-tests";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path path = dir / name;
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    return guitarfx::irwav::LoadAudioFile(path, out);
}

bool HoldsTheSamples(const guitarfx::IRWavData& data)
{
    if (data.channels != 1 || data.samples.size() != std::size(kSamples) || data.sampleRate != 48000.0)
    {
        return false;
    }

    for (std::size_t i = 0; i < std::size(kSamples); ++i)
    {
        if (data.samples[i] != static_cast<float>(kSamples[i]) / 32767.0f)
        {
            return false;
        }
    }

    return true;
}

/// An AIFF/AIFC file of the four samples, mono at 48 kHz. `aifc` writes Logic's 'sowt' (16-bit
/// little-endian) with its compression name; `dataOffset` puts that many bytes before the audio.
std::vector<std::uint8_t> Aiff(bool aifc, std::uint32_t dataOffset)
{
    std::vector<std::uint8_t> body;
    PutTag(body, aifc ? "AIFC" : "AIFF");

    // A Pascal string: a length byte and 13 characters, an even 14 bytes, so no pad byte.
    const char kName[] = "\x0Dlittle endian";
    PutTag(body, "COMM");
    PutBE32(body, aifc ? 22 + 14 : 18);
    PutBE16(body, 1);                   // channels
    PutBE32(body, std::size(kSamples)); // frames
    PutBE16(body, 16);                  // bits
    const std::uint8_t k48k[] = {0x40, 0x0E, 0xBB, 0x80, 0, 0, 0, 0, 0, 0};
    body.insert(body.end(), std::begin(k48k), std::end(k48k));

    if (aifc)
    {
        PutTag(body, "sowt");
        body.insert(body.end(), kName, kName + 14);
    }

    PutTag(body, "SSND");
    PutBE32(body, 8 + dataOffset + 2 * static_cast<std::uint32_t>(std::size(kSamples)));
    PutBE32(body, dataOffset);
    PutBE32(body, 0); // block size
    body.insert(body.end(), dataOffset, 0xAB);

    for (const std::int16_t s : kSamples)
    {
        aifc ? PutLE16(body, static_cast<std::uint16_t>(s)) : PutBE16(body, static_cast<std::uint16_t>(s));
    }

    std::vector<std::uint8_t> file;
    PutTag(file, "FORM");
    PutBE32(file, static_cast<std::uint32_t>(body.size()));
    file.insert(file.end(), body.begin(), body.end());
    return file;
}

/// A 16-bit mono 48 kHz WAV of the four samples, with an odd-sized JUNK chunk before the audio,
/// followed by its pad byte or (as some writers do) not, and the data chunk's size as given.
std::vector<std::uint8_t> WavWithOddChunk(std::uint32_t dataSize, bool padded = true)
{
    std::vector<std::uint8_t> body;
    PutTag(body, "WAVE");
    PutTag(body, "fmt ");
    PutLE32(body, 16);
    PutLE16(body, 1);         // PCM
    PutLE16(body, 1);         // channels
    PutLE32(body, 48000);     // rate
    PutLE32(body, 48000 * 2); // byte rate
    PutLE16(body, 2);         // block align
    PutLE16(body, 16);        // bits
    PutTag(body, "JUNK");
    PutLE32(body, 3);
    body.insert(body.end(), {1, 2, 3});

    if (padded)
    {
        body.push_back(0);
    }

    PutTag(body, "data");
    PutLE32(body, dataSize);

    for (const std::int16_t s : kSamples)
    {
        PutLE16(body, static_cast<std::uint16_t>(s));
    }

    std::vector<std::uint8_t> file;
    PutTag(file, "RIFF");
    PutLE32(file, static_cast<std::uint32_t>(body.size()));
    file.insert(file.end(), body.begin(), body.end());
    return file;
}

/// What the IR loader reads that it used not to: every AIFC file (the COMM chunk's compression
/// name threw the chunk walk off), an AIFF whose audio sits past a data offset (read as samples),
/// and a WAV with an odd-sized chunk before the audio (the pad byte threw the walk off). Without
/// the pad, as two of the test IRs are written, it must still load.
bool TestIrLoaderFormats()
{
    std::cout << "Test: IR loader formats\n";
    guitarfx::IRWavData aifc, offset, wav, unpadded;
    bool passed = true;
    passed &= ReportCheck(LoadIr("sowt.aif", Aiff(true, 0), aifc) && HoldsTheSamples(aifc), "  AIFC 'sowt':");
    passed &= ReportCheck(LoadIr("offset.aiff", Aiff(false, 4), offset) && HoldsTheSamples(offset),
                          "  AIFF with a data offset:");
    passed &= ReportCheck(LoadIr("junk.wav", WavWithOddChunk(8), wav) && HoldsTheSamples(wav),
                          "  WAV with an odd-sized chunk:");
    passed &= ReportCheck(LoadIr("unpadded.wav", WavWithOddChunk(8, false), unpadded) && HoldsTheSamples(unpadded),
                          "  WAV with an odd-sized chunk and no pad byte:");
    return passed;
}

/// A streamed WAV leaves its data size at 0xFFFFFFFF. The loader allocated that many bytes' worth
/// of samples (8.6 GB of floats at 16 bits) before reading what the file holds.
bool TestIrLoaderStreamedDataSize()
{
    std::cout << "Test: IR loader, streamed WAV data size\n";
    guitarfx::IRWavData wav;
    return ReportCheck(LoadIr("streamed.wav", WavWithOddChunk(0xFFFFFFFFu), wav) && HoldsTheSamples(wav),
                       "  reads the samples the file holds:");
}
} // namespace

int main()
{
    bool passed = TestDecodeRegressionFixture();
    passed = TestIrLoaderFormats() && passed;

    // Only once the walk reaches the data chunk, so an old loader is not asked for 8.6 GB.
    passed = passed && TestIrLoaderStreamedDataSize();

    std::cout << (passed ? "\nWAV decode regression tests PASSED\n" : "\nWAV decode regression tests FAILED\n");
    return passed ? 0 : 1;
}
