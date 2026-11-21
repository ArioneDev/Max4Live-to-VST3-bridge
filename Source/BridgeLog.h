#pragma once

#include <juce_core/juce_core.h>

namespace m4lb
{

// Always-on log at %TEMP%\M4LBridge\m4lbridge.log.  This is the only window
// into a plugin that a host has loaded, so it is intentionally not opt-in.
inline void log (const juce::String& line)
{
    static juce::CriticalSection lock;
    const juce::ScopedLock sl (lock);

    auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("M4LBridge")
                    .getChildFile ("m4lbridge.log");

    file.getParentDirectory().createDirectory();

    // Keep the log bounded: rotate once at 512 KB, then start over.
    if (file.getSize() > 512 * 1024)
    {
        auto previous = file.getSiblingFile ("m4lbridge.1.log");
        previous.deleteFile();
        file.moveFileTo (previous);
    }

    file.appendText (juce::Time::getCurrentTime().toString (false, true, true, true)
                     + " " + line + "\n");
}

} // namespace m4lb
