/**
 * ParamFormatTests.cpp - The settings flyouts' parameter rows (uiclient/ParamFormat.h): which
 * parameters are switches and how a value reads, held to the web UI's flyoutParamControls.ts.
 */

#include "uiclient/ParamFormat.h"

#include <iostream>
#include <string>

namespace uic = guitarfx::uiclient;

namespace
{
int gFailures = 0;

void Expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << what << std::endl;
        ++gFailures;
    }
}

uic::EffectParamInfo Param(double min, double max, std::string unit, std::vector<std::string> labels = {},
                           double step = 0.0)
{
    uic::EffectParamInfo p;
    p.minValue = min;
    p.maxValue = max;
    p.unit = std::move(unit);
    p.labels = std::move(labels);
    p.step = step;
    return p;
}

void TestSwitches()
{
    Expect(uic::IsSwitchParam(Param(0, 1, "toggle")), "a toggle is a switch");
    Expect(uic::IsSwitchParam(Param(0, 1, "", {"Independent", "Linked"})), "so is a 0..1 pair of labels");
    Expect(!uic::IsSwitchParam(Param(0, 2, "enum", {"A", "B", "C"})), "three labels are not");
    Expect(!uic::IsSwitchParam(Param(-80, 0, "dB")), "nor is a level");
}

void TestValues()
{
    Expect(uic::FormatFlyoutParamValue(Param(0, 1, "", {"Independent", "Linked"}), 1.0) == "Linked",
           "a switch reads its label");
    Expect(uic::FormatFlyoutParamValue(Param(0, 1, "toggle"), 0.0) == "Off", "or Off");
    Expect(uic::FormatFlyoutParamValue(Param(0, 2, "enum", {"Gate", "Duck", "Expand"}, 1.0), 1.0) == "Duck",
           "an enum reads its label");
    Expect(uic::FormatFlyoutParamValue(Param(-80, 0, "dB"), -60.0) == "-60 dB", "a wide span has no decimals");
    Expect(uic::FormatFlyoutParamValue(Param(0.1, 50, "ms"), 1.0) == "1.0 ms", "a span within 60 has one");
    Expect(uic::FormatFlyoutParamValue(Param(0, 1, "amount"), 0.5) == "0.50", "amount is no unit, and two places");
}
} // namespace

int main()
{
    TestSwitches();
    TestValues();

    if (gFailures > 0)
    {
        std::cerr << gFailures << " failure(s)" << std::endl;
        return 1;
    }

    std::cout << "ParamFormatTests passed" << std::endl;
    return 0;
}
