#include "controller/ControllerDisplayFeed.h"

#include <algorithm>

namespace guitarfx
{
namespace
{
constexpr std::uint8_t kSysExStart = 0xF0;
constexpr std::uint8_t kSysExEnd = 0xF7;
constexpr std::uint8_t kManufacturerNonCommercial = 0x7D;
constexpr std::uint8_t kDeviceFamilyS = 0x53;
constexpr std::uint8_t kDeviceFamilyG = 0x47;
constexpr std::uint8_t kCommandSetText = 0x07;

constexpr char32_t kReplacementCharacter = 0xFFFD;

/// ASCII for U+00C0..U+00FF, Latin-1's accented letters: the base letter, or the usual
/// spelling for the ligatures and the few letters with no base form.
constexpr std::array<const char*, 64> kLatin1Letters = {
    "A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I",  "I",  // U+00C0
    "D", "N", "O", "O", "O", "O", "O",  "x", "O", "U", "U", "U", "U", "Y", "Th", "ss", // U+00D0
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i",  "i",  // U+00E0
    "d", "n", "o", "o", "o", "o", "o",  "/", "o", "u", "u", "u", "u", "y", "th", "y"}; // U+00F0

/// Decodes the code point starting at utf8[index] and moves `index` past it. A malformed,
/// truncated, overlong or surrogate sequence yields U+FFFD and moves on by one byte, so
/// decoding picks up again at the next lead byte.
char32_t DecodeNext(std::string_view utf8, std::size_t& index)
{
    const auto lead = static_cast<unsigned char>(utf8[index]);

    if (lead < 0x80)
    {
        ++index;
        return lead;
    }

    std::size_t length = 0;
    char32_t codePoint = 0;

    if ((lead & 0xE0) == 0xC0)
    {
        length = 2;
        codePoint = lead & 0x1F;
    }
    else if ((lead & 0xF0) == 0xE0)
    {
        length = 3;
        codePoint = lead & 0x0F;
    }
    else if ((lead & 0xF8) == 0xF0)
    {
        length = 4;
        codePoint = lead & 0x07;
    }
    else
    {
        ++index;
        return kReplacementCharacter;
    }

    if (index + length > utf8.size())
    {
        ++index;
        return kReplacementCharacter;
    }

    for (std::size_t offset = 1; offset < length; ++offset)
    {
        const auto continuation = static_cast<unsigned char>(utf8[index + offset]);

        if ((continuation & 0xC0) != 0x80)
        {
            ++index;
            return kReplacementCharacter;
        }

        codePoint = (codePoint << 6) | (continuation & 0x3F);
    }

    static constexpr std::array<char32_t, 5> kSmallestForLength = {0, 0, 0x80, 0x800, 0x10000};

    if (codePoint < kSmallestForLength[length] || codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF))
    {
        ++index;
        return kReplacementCharacter;
    }

    index += length;
    return codePoint;
}

/// Appends what the display shows for `codePoint`.
void AppendAscii(std::string& out, char32_t codePoint)
{
    if (codePoint < 0x20 || codePoint == 0x7F)
    {
        out += ' ';
        return;
    }

    if (codePoint < 0x80)
    {
        out += static_cast<char>(codePoint);
        return;
    }

    if (codePoint >= 0xC0 && codePoint <= 0xFF)
    {
        out += kLatin1Letters[codePoint - 0xC0];
        return;
    }

    switch (codePoint)
    {
    case 0x00A0: // no-break space
    case 0x2002: // en space
    case 0x2003: // em space
    case 0x2009: // thin space
        out += ' ';
        return;

    case 0x00B4: // acute accent
    case 0x2018: // left single quote
    case 0x2019: // right single quote, the apostrophe most editors type
    case 0x201A: // low single quote
    case 0x2032: // prime
        out += '\'';
        return;

    case 0x00AB: // left guillemet
    case 0x00BB: // right guillemet
    case 0x201C: // left double quote
    case 0x201D: // right double quote
    case 0x201E: // low double quote
    case 0x2033: // double prime
        out += '"';
        return;

    case 0x2010: // hyphen
    case 0x2011: // non-breaking hyphen
    case 0x2012: // figure dash
    case 0x2013: // en dash
    case 0x2014: // em dash
    case 0x2015: // horizontal bar
    case 0x2212: // minus sign
        out += '-';
        return;

    case 0x2026: // ellipsis
        out += "...";
        return;

    default:
        out += '?';
        return;
    }
}
} // namespace

std::string ControllerDisplayFeed::ToDisplayText(std::string_view utf8)
{
    std::string ascii;
    ascii.reserve(utf8.size());

    for (std::size_t index = 0; index < utf8.size();)
    {
        AppendAscii(ascii, DecodeNext(utf8, index));
    }

    const auto first = ascii.find_first_not_of(' ');

    if (first == std::string::npos)
    {
        return {};
    }

    ascii.erase(0, first);

    if (ascii.size() > kMaxTextLength)
    {
        ascii.resize(kMaxTextLength);
    }

    ascii.erase(ascii.find_last_not_of(' ') + 1);
    return ascii;
}

std::size_t ControllerDisplayFeed::BuildSetTextSysEx(std::uint8_t line, std::string_view text, SysExBuffer& out)
{
    std::size_t size = 0;
    out[size++] = kSysExStart;
    out[size++] = kManufacturerNonCommercial;
    out[size++] = kDeviceFamilyS;
    out[size++] = kDeviceFamilyG;
    out[size++] = kCommandSetText;
    out[size++] = line & 0x7F;

    for (const char character : text.substr(0, kMaxTextLength))
    {
        out[size++] = static_cast<std::uint8_t>(static_cast<unsigned char>(character) & 0x7F);
    }

    out[size++] = kSysExEnd;
    return size;
}

void ControllerDisplayFeed::Update(std::string_view presetName, bool enabled)
{
    if (!enabled)
    {
        mLastQueuedName.reset();

        if (mHasQueued.load(std::memory_order_acquire))
        {
            std::lock_guard<std::mutex> lock(mQueuedMutex);
            mHasQueued.store(false, std::memory_order_release);
        }

        return;
    }

    const bool refresh = mRefreshRequested.exchange(false, std::memory_order_acq_rel);

    if (!refresh && mLastQueuedName.has_value() && *mLastQueuedName == presetName)
    {
        return;
    }

    SysExBuffer message{};
    const std::size_t size = BuildSetTextSysEx(kPresetNameLine, ToDisplayText(presetName), message);

    {
        std::lock_guard<std::mutex> lock(mQueuedMutex);
        mQueued = message;
        mQueuedSize = size;
        mHasQueued.store(true, std::memory_order_release);
    }

    mLastQueuedName = std::string(presetName);
}

void ControllerDisplayFeed::RequestRefresh()
{
    mRefreshRequested.store(true, std::memory_order_release);
}

std::size_t ControllerDisplayFeed::TakeSysEx(std::span<std::uint8_t> out)
{
    if (!mHasQueued.load(std::memory_order_acquire))
    {
        return 0;
    }

    std::unique_lock<std::mutex> lock(mQueuedMutex, std::try_to_lock);

    if (!lock.owns_lock() || !mHasQueued.load(std::memory_order_acquire) || out.size() < mQueuedSize)
    {
        return 0;
    }

    std::copy_n(mQueued.begin(), mQueuedSize, out.begin());
    mHasQueued.store(false, std::memory_order_release);
    return mQueuedSize;
}
} // namespace guitarfx
