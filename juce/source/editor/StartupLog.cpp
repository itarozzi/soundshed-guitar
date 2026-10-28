#include "StartupLog.h"

#include "ProfileFolder.h"

namespace soundshed::editor
{
void writeStartupLog(const juce::String& message)
{
    const auto logDir = soundshed::profileFolder().getChildFile("logs");
    logDir.createDirectory();
    const auto logFile = logDir.getChildFile("soundshed-startup.log");

    // Every editor open adds lines and nothing else trims the file, so keep the newest
    // megabyte or two: the current file, plus the one before it as soundshed-startup.old.log.
    constexpr juce::int64 maxLogBytes = 1024 * 1024;

    if (logFile.getSize() > maxLogBytes)
    {
        const auto previous = logDir.getChildFile("soundshed-startup.old.log");
        previous.deleteFile();
        logFile.moveFileTo(previous);
    }

    juce::FileOutputStream stream(logFile);

    if (stream.openedOk())
    {
        stream.setPosition(stream.getFile().getSize()); // append
        const auto line = juce::Time::getCurrentTime().formatted("%Y-%m-%d %H:%M:%S") + "  " + message + "\n";
        stream.writeText(line, false, false, nullptr);
    }

#if !JUCE_LINUX
    juce::Logger::writeToLog(message);
#endif
}
} // namespace soundshed::editor
