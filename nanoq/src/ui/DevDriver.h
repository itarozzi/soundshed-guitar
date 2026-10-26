#pragma once

// DevDriver - lets a script drive the UI, for testing it the way the JUCE Nano's debug server
// does, without the operating system's input (which a background window on macOS ignores, and
// which would take the user's own pointer).
//
// Off unless SOUNDSHED_NANOQ_DRIVE names a file. Each tick, if that file exists, its lines are
// played into the view and the file is deleted, so a script writes a file and waits for it to
// go. Coordinates are the view's own (logical pixels from its top-left, below any title bar):
//
//   click x y          a left click             double x y    a double click
//   right x y          a right click            move x y      the pointer moves there
//   drag x1 y1 x2 y2   press, move, release     scroll x y dy a wheel turn (dy > 0 is up)

#include <elements.hpp>

#include <filesystem>
#include <optional>

namespace nanoq::ui
{
class DevDriver
{
public:
    /// The driver the environment asks for, or nullopt (the normal case).
    static std::optional<DevDriver> FromEnvironment();

    /// Plays and deletes the command file, if one is waiting. UI thread.
    void Poll(cycfi::elements::view& view);

private:
    explicit DevDriver(std::filesystem::path file) : mFile(std::move(file)) {}

    std::filesystem::path mFile;
};
} // namespace nanoq::ui
