// Prints the .maxpat that the bridge plugin would generate (so the transport
// test can run against exactly the same patch), and can report what the plugin
// would auto-detect for Max / its shim bundles.

#include "BridgePaths.h"
#include "PatchGenerator.h"

#include <cstdio>
#include <windows.h>
#include <tlhelp32.h>

int main (int argc, char** argv)
{
    if (argc > 1 && juce::String (argv[1]) == "findmax")
    {
        {
            HANDLE snap = CreateToolhelp32Snapshot (TH32CS_SNAPPROCESS, 0);
            std::printf ("snap=%p err=%lu\n", (void*) snap, (unsigned long) GetLastError());
            PROCESSENTRY32W pe {}; pe.dwSize = sizeof (pe);
            int n = 0;
            if (Process32FirstW (snap, &pe)) do { if (n < 5) std::printf ("proc: %ls\n", pe.szExeFile); ++n; } while (Process32NextW (snap, &pe));
            std::printf ("procs=%d\n", n);
            CloseHandle (snap);
        }
        std::printf ("isRunning Max.exe=%d MaxRT.exe=%d MaxRT_nocef.exe=%d\n",
                     m4lb::isProcessRunning ("Max.exe") ? 1 : 0,
                     m4lb::isProcessRunning ("MaxRT.exe") ? 1 : 0,
                     m4lb::isProcessRunning ("MaxRT_nocef.exe") ? 1 : 0);
        const auto exe = m4lb::findFreeMaxExecutable();
        std::printf ("max: %s\n", exe.existsAsFile() ? exe.getFullPathName().toRawUTF8() : "<not found>");

        const auto out = m4lb::findShimBundle ("M4L Bridge Out.vst3");
        const auto in  = m4lb::findShimBundle ("M4L Bridge In.vst3");
        std::printf ("shim out: %s\n", out.isDirectory() ? out.getFullPathName().toRawUTF8() : "<not found>");
        std::printf ("shim in : %s\n", in.isDirectory() ? in.getFullPathName().toRawUTF8() : "<not found>");
        std::printf ("module  : %s\n", m4lb::moduleFile().getFullPathName().toRawUTF8());
        return 0;
    }

    if (argc < 4)
    {
        std::printf ("usage: M4LBPatchGen <shimOut.vst3> <shimIn.vst3> <device.amxd> [loopback]\n");
        std::printf ("       M4LBPatchGen findmax\n");
        return 2;
    }

    m4lb::PatchOptions options;
    options.shimOutPath = argv[1];
    options.shimInPath = argv[2];
    options.devicePath = argv[3];
    options.loopback = argc > 4 && juce::String (argv[4]) == "loopback";
    options.midiProbe = argc > 4 && juce::String (argv[4]) == "midiprobe";

    if (argc > 5)
        options.udpPort = juce::String (argv[5]).getIntValue();

    if (argc > 6)
        options.instanceId = (uint32_t) juce::String (argv[6]).getIntValue();

    const auto json = m4lb::buildPatch (options);
    std::printf ("%s", json.toRawUTF8());
    return 0;
}
