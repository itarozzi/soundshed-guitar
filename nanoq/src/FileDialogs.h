#pragma once

#include "IPluginHost.h"

#include <functional>
#include <string>

namespace nanoq
{
/// Native open and save dialogs, run on a thread of their own so the engine and the view never
/// wait on them. `done` is called from that thread; the caller hands the result on.
/// Windows only for now: elsewhere the dialog reports that nothing was chosen.
void ShowOpenDialog(guitarfx::BrowseFileType type, const std::string& title,
                    std::function<void(const guitarfx::BrowseFileResult&)> done);

void ShowSaveDialog(guitarfx::BrowseFileType type, const std::string& title, const std::string& defaultName,
                    std::function<void(const guitarfx::BrowseFileResult&)> done);

/// Tests: stands in for the native dialogs. The hook is given what a dialog would be, and `done`,
/// which it may call from any thread and at any time, as a dialog's own thread does. Null puts
/// the native dialogs back.
using FileDialogHook = std::function<void(bool save, guitarfx::BrowseFileType type, const std::string& title,
                                          const std::string& defaultName,
                                          std::function<void(const guitarfx::BrowseFileResult&)> done)>;
void SetFileDialogHookForTesting(FileDialogHook hook);
} // namespace nanoq
