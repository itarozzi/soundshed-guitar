#include "ui/DevDriver.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace nanoq::ui
{
namespace el = cycfi::elements;

std::optional<DevDriver> DevDriver::FromEnvironment()
{
    const char* path = std::getenv("SOUNDSHED_NANOQ_DRIVE");
    if (path == nullptr || *path == '\0')
        return std::nullopt;
    return DevDriver(std::filesystem::path(path));
}

void DevDriver::Poll(el::view& view)
{
    std::error_code ec;
    if (!std::filesystem::exists(mFile, ec))
        return;

    std::vector<std::string> lines;
    {
        std::ifstream in(mFile);
        for (std::string line; std::getline(in, line);)
            lines.push_back(line);
    }
    std::filesystem::remove(mFile, ec);

    auto button = [](bool down, int clicks, el::mouse_button::what which, float x, float y) {
        return el::mouse_button{down, clicks, which, 0, el::point{x, y}};
    };

    for (const auto& line : lines)
    {
        std::istringstream words(line);
        std::string verb;
        float x = 0.0f, y = 0.0f;
        if (!(words >> verb >> x >> y))
            continue;

        if (verb == "move")
        {
            view.cursor(el::point{x, y}, el::cursor_tracking::hovering);
        }
        else if (verb == "click" || verb == "right" || verb == "double")
        {
            const auto which = verb == "right" ? el::mouse_button::right : el::mouse_button::left;
            view.cursor(el::point{x, y}, el::cursor_tracking::hovering);
            view.click(button(true, 1, which, x, y));
            view.click(button(false, 1, which, x, y));
            if (verb == "double")
            {
                view.click(button(true, 2, which, x, y));
                view.click(button(false, 2, which, x, y));
            }
        }
        else if (verb == "drag")
        {
            float x2 = 0.0f, y2 = 0.0f;
            if (!(words >> x2 >> y2))
                continue;
            view.click(button(true, 1, el::mouse_button::left, x, y));
            constexpr int kSteps = 12;
            for (int i = 1; i <= kSteps; ++i)
            {
                const float t = static_cast<float>(i) / kSteps;
                view.drag(button(true, 1, el::mouse_button::left, x + (x2 - x) * t, y + (y2 - y) * t));
            }
            view.click(button(false, 1, el::mouse_button::left, x2, y2));
        }
        else if (verb == "scroll")
        {
            float dy = 0.0f;
            if (words >> dy)
                view.scroll(el::point{0.0f, dy}, el::point{x, y});
        }
    }
}
} // namespace nanoq::ui
