// Coverage for util::WriteFileAtomic, which PluginController::WriteFile uses for
// every file it writes.
//
// The bug it fixes: several plugin instances starting together on a fresh profile
// all extracted the factory archives in place, and one instance's NamModelCache
// parsed a .nam another was still writing ("unexpected end of input" at a 4096-byte
// boundary). A reader must only ever see no file or the complete bytes.

#include "util/FileIO.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using guitarfx::util::FileContentEquals;
using guitarfx::util::WriteFileAtomic;
using guitarfx::util::WriteTextFileAtomic;

namespace
{
bool gAllPassed = true;

void Check(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << what << std::endl;
        gAllPassed = false;
    }
    else
    {
        std::cout << "  ok: " << what << std::endl;
    }
}

fs::path MakeTempDir(const std::string& tag)
{
    const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const fs::path dir = fs::temp_directory_path() / ("soundshed-atomic-write-" + tag + "-" + unique);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

/// Deterministic bytes that differ at every offset between seeds, so a torn read
/// or a read that mixed two versions cannot match either payload by accident.
std::vector<std::uint8_t> MakePayload(std::size_t size, std::uint32_t seed)
{
    std::vector<std::uint8_t> bytes(size);
    std::uint32_t state = seed;

    for (auto& byte : bytes)
    {
        state = state * 1664525u + 1013904223u;
        byte = static_cast<std::uint8_t>(state >> 24);
    }

    return bytes;
}

/// Reads to end of file rather than trusting a size taken first, the way a JSON
/// parser consumes a stream — a file still being written reads short.
bool ReadWhole(const fs::path& path, std::vector<std::uint8_t>& out)
{
    std::ifstream input(path, std::ios::binary);

    if (!input)
    {
        return false;
    }

    out.clear();
    std::vector<char> chunk(64 * 1024);

    while (input)
    {
        input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        out.insert(out.end(), chunk.data(), chunk.data() + input.gcount());
    }

    return true;
}

std::size_t CountEntries(const fs::path& dir)
{
    std::size_t count = 0;

    for ([[maybe_unused]] const auto& entry : fs::directory_iterator(dir))
    {
        ++count;
    }

    return count;
}

struct ReaderStats
{
    std::atomic<int> absent{0};
    /// Failed opens after the file had been read once. A model load fails on
    /// these just as surely as on a torn file.
    std::atomic<int> absentAfterPresent{0};
    std::atomic<int> complete{0};
    std::atomic<int> torn{0};
    std::atomic<std::size_t> firstTornSize{0};
};

/// Reads `path` until `stop`, classifying every read as absent, one of the
/// expected payloads in full, or torn. `pause` leaves the file closed between
/// reads so a Windows replace can get in, as it would between real model loads.
void ReadUntilStopped(const fs::path& path, const std::vector<const std::vector<std::uint8_t>*>& expected,
                      const std::atomic<bool>& stop, ReaderStats& stats, std::chrono::microseconds pause)
{
    std::vector<std::uint8_t> bytes;
    bool seenFile = false;

    while (!stop.load())
    {
        if (!ReadWhole(path, bytes))
        {
            stats.absent.fetch_add(1);

            if (seenFile)
            {
                stats.absentAfterPresent.fetch_add(1);
            }
        }
        else
        {
            seenFile = true;
            bool matched = false;

            for (const auto* payload : expected)
            {
                matched = matched || bytes == *payload;
            }

            if (matched)
            {
                stats.complete.fetch_add(1);
            }
            else if (stats.torn.fetch_add(1) == 0)
            {
                stats.firstTornSize.store(bytes.size());
            }
        }

        std::this_thread::sleep_for(pause);
    }
}

void ReportReads(const ReaderStats& stats)
{
    std::cout << "  reads: " << stats.complete.load() << " complete, " << stats.absent.load() << " absent ("
              << stats.absentAfterPresent.load() << " after the file appeared), " << stats.torn.load() << " torn"
              << std::endl;
}

/// The JSON and text writers go through this. Text-mode streams on Windows turned
/// "\n" into "\r\n"; this writes the bytes it is given.
void TestTextWrite()
{
    std::cout << "\nText" << std::endl;
    const auto dir = MakeTempDir("text");
    const auto target = dir / "index.json";
    const std::string text = "{\n  \"name\": \"Caf\xC3\xA9\"\n}\n";
    const std::span<const std::uint8_t> textBytes(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());

    Check(WriteTextFileAtomic(target, text), "writes a text file");
    Check(FileContentEquals(target, textBytes), "text file holds exactly the text, newlines untranslated");
    Check(WriteTextFileAtomic(target, "[]"), "replaces a text file");
    Check(fs::file_size(target) == 2, "replaced text file holds only the new text");
    Check(CountEntries(dir) == 1, "no temporary sibling is left behind");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

/// Windows caps a path at MAX_PATH unless the host process opts out. A temporary
/// name built from the target's own name pushed a session preset that only just
/// fitted over that limit, so the preset could not be written at all.
void TestTargetNearPathLimit()
{
    std::cout << "\nTarget path near MAX_PATH" << std::endl;
    const auto dir = MakeTempDir("long");
    constexpr std::size_t kTargetLength = 255;
    const std::size_t dirLength = dir.native().size() + 1;
    std::error_code ec;

    if (dirLength + 40 > kTargetLength)
    {
        std::cout << "  skipped: the temp directory path is too long to build this case" << std::endl;
        fs::remove_all(dir, ec);
        return;
    }

    const auto target = dir / (std::string(kTargetLength - dirLength - 5, 'n') + ".json");
    std::string error;

    Check(target.native().size() == kTargetLength,
          "target path is " + std::to_string(kTargetLength) + " characters long");
    Check(WriteFileAtomic(target, MakePayload(4096, 40), &error),
          "writes a target that only just fits" + (error.empty() ? std::string() : " (" + error + ")"));
    Check(WriteFileAtomic(target, MakePayload(4096, 41), &error), "replaces it as well");
    Check(CountEntries(dir) == 1, "no temporary sibling is left behind");

    fs::remove_all(dir, ec);
}

void TestWriteReplaceAndCleanup()
{
    std::cout << "\nWrite, replace and clean up" << std::endl;
    const auto dir = MakeTempDir("basic");
    const auto target = dir / "model.nam";
    const auto first = MakePayload(100000, 1);
    const auto second = MakePayload(4096, 2);
    std::string error;

    Check(WriteFileAtomic(target, first, &error), "writes a new file");
    Check(FileContentEquals(target, first), "new file holds the bytes");

    Check(WriteFileAtomic(target, second, &error), "replaces an existing file with a shorter one");
    Check(FileContentEquals(target, second), "replaced file holds only the new bytes");
    Check(CountEntries(dir) == 1, "no temporary sibling is left behind");

    const auto writeTimeBefore = fs::last_write_time(target);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Check(WriteFileAtomic(target, second, &error), "rewriting identical bytes succeeds");
    Check(fs::last_write_time(target) == writeTimeBefore, "identical bytes are left in place, not replaced");
    Check(CountEntries(dir) == 1, "no temporary sibling is left behind after an identical write");

    Check(WriteFileAtomic(target, std::vector<std::uint8_t>{}, &error), "writes an empty file");
    Check(fs::exists(target) && fs::file_size(target) == 0, "empty file is empty");

    error.clear();
    const auto missingDirTarget = dir / "no-such-dir" / "model.nam";
    Check(!WriteFileAtomic(missingDirTarget, first, &error), "fails when the directory does not exist");
    Check(!error.empty(), "failure explains itself");
    Check(!fs::exists(missingDirTarget), "failure creates nothing");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

void TestContentEquals()
{
    std::cout << "\nContent comparison" << std::endl;
    const auto dir = MakeTempDir("equals");
    const auto target = dir / "model.nam";
    const auto payload = MakePayload(300000, 3);
    auto sameSizeDifferent = payload;
    sameSizeDifferent.back() ^= 0xFF;

    Check(!FileContentEquals(target, payload), "missing file is not equal");
    Check(WriteFileAtomic(target, payload), "writes the comparison file");
    Check(FileContentEquals(target, payload), "identical bytes are equal");
    Check(!FileContentEquals(target, sameSizeDifferent), "same size, last byte different, is not equal");
    Check(!FileContentEquals(target, MakePayload(payload.size() - 1, 3)), "a prefix is not equal");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

/// The requested case: one thread keeps writing a large file while another keeps
/// reading it. Two payloads of different sizes, so a replace that is not atomic
/// shows up either as a short read or as a mix of the two.
void TestReaderNeverSeesPartialWrite()
{
    std::cout << "\nOne writer, one reader" << std::endl;
    const auto dir = MakeTempDir("reader");
    const auto target = dir / "model.nam";
    const auto payloadA = MakePayload(8 * 1024 * 1024, 10);
    const auto payloadB = MakePayload(5 * 1024 * 1024 + 123, 20);

    constexpr int kWrites = 80;
    std::atomic<bool> stop{false};
    std::atomic<int> writeFailures{0};
    std::string firstWriteError;
    ReaderStats stats;

    std::thread reader(
        [&]() { ReadUntilStopped(target, {&payloadA, &payloadB}, stop, stats, std::chrono::microseconds(500)); });

    std::thread writer([&]() {
        for (int i = 0; i < kWrites; ++i)
        {
            std::string error;

            if (!WriteFileAtomic(target, (i % 2 == 0) ? payloadA : payloadB, &error) && writeFailures.fetch_add(1) == 0)
            {
                firstWriteError = error;
            }
        }

        stop.store(true);
    });

    writer.join();
    reader.join();

    ReportReads(stats);

    // Failed opens after the file appeared are expected here, so not asserted. On
    // Windows a reader opening the file at the instant a *different* version
    // replaces it gets ERROR_SHARING_VIOLATION, because std::ifstream does not
    // share delete. No writer can avoid that for content that really changes;
    // what matters is that nobody ever parses part of a file. Identical bytes are
    // never replaced, and the next test asserts no failed opens for those.
    Check(stats.torn.load() == 0, "reader never saw a partial file (first torn read was " +
                                      std::to_string(stats.firstTornSize.load()) + " bytes)");
    Check(writeFailures.load() == 0,
          "every write succeeded" + (firstWriteError.empty() ? std::string() : " (" + firstWriteError + ")"));
    Check(stats.complete.load() > 0, "reader and writer actually overlapped");
    Check(FileContentEquals(target, payloadB), "last write wins");
    Check(CountEntries(dir) == 1, "no temporary sibling is left behind");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

/// What actually happens after install: several instances extract the same
/// resource at once while others parse it. Readers here do not pause. Identical
/// bytes are never replaced, so once the file exists no reader may fail to open it.
void TestConcurrentIdenticalWriters()
{
    std::cout << "\nFour writers of identical bytes, two readers" << std::endl;
    const auto dir = MakeTempDir("instances");
    const auto target = dir / "model.nam";
    const auto payload = MakePayload(2 * 1024 * 1024 + 7, 30);

    constexpr int kWriters = 4;
    constexpr int kWritesEach = 25;
    std::atomic<bool> stop{false};
    std::atomic<int> writeFailures{0};
    ReaderStats stats;

    std::vector<std::thread> readers;

    for (int r = 0; r < 2; ++r)
    {
        readers.emplace_back(
            [&]() { ReadUntilStopped(target, {&payload}, stop, stats, std::chrono::microseconds(0)); });
    }

    std::vector<std::thread> writers;

    for (int w = 0; w < kWriters; ++w)
    {
        writers.emplace_back([&]() {
            for (int i = 0; i < kWritesEach; ++i)
            {
                if (!WriteFileAtomic(target, payload))
                {
                    writeFailures.fetch_add(1);
                }
            }
        });
    }

    for (auto& writer : writers)
    {
        writer.join();
    }

    stop.store(true);

    for (auto& reader : readers)
    {
        reader.join();
    }

    ReportReads(stats);

    Check(stats.torn.load() == 0, "no reader saw a partial file");
    Check(stats.absentAfterPresent.load() == 0, "no reader failed to open the file once it existed");
    Check(writeFailures.load() == 0, "every instance's write succeeded");
    Check(FileContentEquals(target, payload), "file holds the bytes");
    Check(CountEntries(dir) == 1, "no temporary sibling is left behind");

    std::error_code ec;
    fs::remove_all(dir, ec);
}
} // namespace

int main()
{
    TestWriteReplaceAndCleanup();
    TestContentEquals();
    TestTextWrite();
    TestTargetNearPathLimit();
    TestReaderNeverSeesPartialWrite();
    TestConcurrentIdenticalWriters();

    if (gAllPassed)
    {
        std::cout << "\nAtomicFileWriteTests passed" << std::endl;
    }

    return gAllPassed ? 0 : 1;
}
