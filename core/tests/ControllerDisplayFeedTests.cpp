#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "controller/ControllerDisplayFeed.h"

using namespace guitarfx;

namespace
{
bool Expect(const bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << message << std::endl;
        return false;
    }

    return true;
}

using Bytes = std::vector<std::uint8_t>;

Bytes Build(std::uint8_t line, std::string_view text)
{
    ControllerDisplayFeed::SysExBuffer out{};
    const auto size = ControllerDisplayFeed::BuildSetTextSysEx(line, text, out);
    return Bytes(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(size));
}

Bytes PresetNameMessage(std::string_view text)
{
    return Build(ControllerDisplayFeed::kPresetNameLine, text);
}

Bytes Take(ControllerDisplayFeed& feed)
{
    ControllerDisplayFeed::SysExBuffer out{};
    const auto size = feed.TakeSysEx(out);
    return Bytes(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(size));
}

bool ExpectDisplayText(std::string_view utf8, std::string_view expected, const std::string& what)
{
    const auto actual = ControllerDisplayFeed::ToDisplayText(utf8);
    return Expect(actual == expected, what + ": expected \"" + std::string(expected) + "\", got \"" + actual + "\"");
}
} // namespace

int main()
{
    bool allPassed = true;

    // ── The message ──────────────────────────────────────────────────────
    // Byte for byte what soundshed-go's midi_control_build_text() writes.
    const Bytes clean{0xF0, 0x7D, 0x53, 0x47, 0x07, 0x00, 'C', 'l', 'e', 'a', 'n', 0xF7};
    allPassed &= Expect(Build(0, "Clean") == clean, "SET_TEXT framing for line 0");
    allPassed &=
        Expect(Build(1, "") == Bytes{0xF0, 0x7D, 0x53, 0x47, 0x07, 0x01, 0xF7}, "An empty SET_TEXT for line 1");

    const Bytes cut = Build(0, std::string(40, 'x'));
    allPassed &= Expect(cut.size() == ControllerDisplayFeed::kMaxSysExBytes && cut.back() == 0xF7,
                        "Text past 32 characters is cut and the message still ends in F7");

    const Bytes masked = Build(0, "\xF7");
    allPassed &= Expect(masked.size() == 8 && masked[6] == 0x77, "A byte with the top bit set is masked to 7 bits");

    // ── Display text ─────────────────────────────────────────────────────
    // UTF-8 is spelled in bytes so the test does not depend on the compiler's source character
    // set. A letter straight after a hex escape is escaped too (\x65 is 'e'), or it would be
    // read as another hex digit.
    allPassed &= ExpectDisplayText("Plexi Lead", "Plexi Lead", "Plain ASCII is unchanged");
    allPassed &= ExpectDisplayText("M\xC3\xB6tley Cr\xC3\xBC\x65", "Motley Crue", "Accents are dropped");
    allPassed &= ExpectDisplayText("Stra\xC3\x9F\x65", "Strasse", "Sharp s is spelled out");
    allPassed &= ExpectDisplayText("Plexi \xE2\x80\x94 Lead", "Plexi - Lead", "An em dash becomes a hyphen");
    allPassed &= ExpectDisplayText("Don\xE2\x80\x99t Stop", "Don't Stop", "A curly apostrophe becomes a straight one");
    allPassed &= ExpectDisplayText("Wait\xE2\x80\xA6", "Wait...", "An ellipsis becomes three dots");
    allPassed &= ExpectDisplayText("Fire \xF0\x9F\x94\xA5", "Fire ?", "A character with no ASCII form becomes '?'");
    allPassed &= ExpectDisplayText("Tab\tand\nnewline", "Tab and newline", "Control characters become spaces");
    allPassed &= ExpectDisplayText("   padded   ", "padded", "Leading and trailing spaces are trimmed");
    allPassed &= ExpectDisplayText("   ", "", "A name of only spaces is empty");
    allPassed &= ExpectDisplayText("Bad \xFF byte", "Bad ? byte", "An invalid byte becomes '?'");
    allPassed &= ExpectDisplayText("\xC0\xAF", "??", "An overlong encoding is rejected byte by byte");
    allPassed &= ExpectDisplayText("Cut \xE2\x80", "Cut ??", "A sequence truncated by the end of the name is rejected");
    allPassed &= ExpectDisplayText("abcdefghijklmnopqrstuvwxyz0123456789", "abcdefghijklmnopqrstuvwxyz012345",
                                   "Display text is cut to 32 characters");
    allPassed &= ExpectDisplayText("abcdefghijklmnopqrstuvwxyz01234 6789", "abcdefghijklmnopqrstuvwxyz01234",
                                   "A cut that ends on a space does not leave it trailing");

    // ── The feed ─────────────────────────────────────────────────────────
    {
        ControllerDisplayFeed feed;
        allPassed &= Expect(Take(feed).empty(), "Nothing is queued before the first update");

        feed.Update("Clean", true);
        allPassed &= Expect(Take(feed) == PresetNameMessage("Clean"), "The first update sends the name");
        allPassed &= Expect(Take(feed).empty(), "A queued message is collected once");

        feed.Update("Clean", true);
        allPassed &= Expect(Take(feed).empty(), "An unchanged name is not sent again");

        feed.RequestRefresh();
        feed.Update("Clean", true);
        allPassed &= Expect(Take(feed) == PresetNameMessage("Clean"), "A refresh sends an unchanged name again");
        feed.Update("Clean", true);
        allPassed &= Expect(Take(feed).empty(), "One refresh sends once");

        feed.Update("Crunch", true);
        feed.Update("Lead", true);
        allPassed &= Expect(Take(feed) == PresetNameMessage("Lead"), "Only the newest name is kept");
        allPassed &= Expect(Take(feed).empty(), "The replaced name is not sent afterwards");

        feed.Update("Lead", false);
        allPassed &= Expect(Take(feed).empty(), "Nothing is sent while the feed is off");
        feed.Update("Lead", true);
        allPassed &= Expect(Take(feed) == PresetNameMessage("Lead"), "Turning the feed back on sends the name again");

        feed.Update("Crunch", true);
        feed.Update("Crunch", false);
        allPassed &= Expect(Take(feed).empty(), "Turning the feed off drops a message not yet collected");

        feed.Update("", true);
        allPassed &= Expect(Take(feed) == PresetNameMessage(""), "No active preset clears the line");

        feed.Update("Clean", true);
        std::array<std::uint8_t, 4> tooSmall{};
        allPassed &= Expect(feed.TakeSysEx(tooSmall) == 0, "A buffer too small for the message takes nothing");
        allPassed &= Expect(Take(feed) == PresetNameMessage("Clean"), "The message survives a failed take");

        feed.Update("M\xC3\xB6tley Cr\xC3\xBC\x65", true);
        allPassed &=
            Expect(Take(feed) == PresetNameMessage("Motley Crue"), "The feed sends display text, not raw UTF-8");
    }

    if (allPassed)
    {
        std::cout << "ControllerDisplayFeedTests passed" << std::endl;
    }

    return allPassed ? 0 : 1;
}
