#pragma once

#include <juce_core/juce_core.h>

namespace m4lb
{

struct PatchOptions
{
    juce::String shimOutPath; // absolute path to "M4L Bridge Out.vst3"
    juce::String shimInPath;  // absolute path to "M4L Bridge In.vst3"
    juce::String devicePath;  // absolute path to the .amxd
    bool loopback = false;    // test mode: shim out -> shim in, no amxd~
    bool openDeviceUi = true;
    bool midiProbe = false;   // test aid: log every message outlet of shimOut
    int udpPort = 0;          // OSC port the Max patch listens on
    uint32_t instanceId = 0;  // shared-memory key (0 = derive from Max pid)
    int feedbackPort = 0;     // OSC port the Max side sends parameter info to
    bool debugLog = false;    // write ctl.js diagnostics
};

// Builds the JSON text of a .maxpat that wires:
//   [vst~ out] -> [amxd~ device] -> [vst~ in] -> [*~ 0.] -> [dac~]
juce::String buildPatch (const PatchOptions& options);

// Companion script written next to the generated patch.
juce::String controlScript();

} // namespace m4lb
