// Console loopback test for the shared-memory transport.
//
// Launches Max with a patch that wires [vst~ "M4L Bridge Out"] straight into
// [vst~ "M4L Bridge In"], pushes a known ramp into the input ring, waits for
// the shims to move it through Max, and verifies the output ring.
//
// This exercises the same transport the VST3 uses, without needing a host,
// an audio device, or a real .amxd.

#include "SharedProtocol.h"
#include "SharedTransport.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{

void pushFrameBlock (m4lb::SharedLayout* layout, int channels, const float* const* src, int frames)
{
    float* rings[m4lb::kMaxChannels] = { layout->inAudio[0], layout->inAudio[1] };
    m4lb::ringWrite (rings, channels, layout->header.inWrite, src, frames);
}

} // namespace

int main (int argc, char** argv)
{
    if (argc > 2 && std::strcmp (argv[1], "note") == 0)
    {
        const auto pid = (uint32_t) std::strtoul (argv[2], nullptr, 10);
        m4lb::SharedTransport transport;

        for (int i = 0; i < 200 && ! transport.valid(); ++i)
        {
            transport.attach (pid);
            if (! transport.valid())
                Sleep (50);
        }

        if (! transport.valid())
        {
            std::printf ("FAIL: no shared memory for pid %u\n", (unsigned) pid);
            return 1;
        }

        auto& h = transport.layout()->header;
        m4lb::MidiEvent ev;
        ev.size = 3;
        ev.data[0] = 0x90; ev.data[1] = 60; ev.data[2] = 100;
        m4lb::pushMidi (transport.layout()->midiToMax, h.midiToMaxWrite, h.midiToMaxRead, ev);

        Sleep (1500);

        float peak = 0.0f;
        const int avail = m4lb::ringFill (h.outWrite.load(), h.outRead.load());
        const int frames = std::min (4096, avail);

        if (frames > 0)
        {
            std::vector<float> l ((size_t) frames), r ((size_t) frames);
            float* dst[m4lb::kMaxChannels] = { l.data(), r.data() };
            float* rings[m4lb::kMaxChannels] = { transport.layout()->outAudio[0], transport.layout()->outAudio[1] };
            m4lb::ringRead (rings, 2, h.outRead, dst, frames);

            for (int i = 0; i < frames; ++i)
                peak = std::max (peak, std::abs (l[(size_t) i]));
        }

        std::printf ("frames=%d peak=%g heartbeat=%u\n", frames, (double) peak, h.heartbeat.load());
        return peak > 1.0e-4f ? 0 : 1;
    }

    if (argc > 2 && std::strcmp (argv[1], "midi") == 0)
    {
        const auto pid = (uint32_t) std::strtoul (argv[2], nullptr, 10);
        m4lb::SharedTransport transport;

        for (int i = 0; i < 200 && ! transport.valid(); ++i)
        {
            transport.attach (pid);
            if (! transport.valid())
                Sleep (50);
        }

        if (! transport.valid())
        {
            std::printf ("FAIL: no shared memory for pid %u\n", (unsigned) pid);
            return 1;
        }

        auto& h = transport.layout()->header;
        m4lb::MidiEvent ev;
        ev.size = 3;
        ev.data[0] = 0x90;
        ev.data[1] = 60;
        ev.data[2] = 100;
        m4lb::pushMidi (transport.layout()->midiToMax, h.midiToMaxWrite, h.midiToMaxRead, ev);

        Sleep (1500);
        std::printf ("pushed note-on to pid %u (heartbeat=%u)\n", (unsigned) pid, h.heartbeat.load());
        return 0;
    }

    if (argc > 3 && std::strcmp (argv[1], "attach") == 0)
    {
        const auto pid = (uint32_t) std::strtoul (argv[2], nullptr, 10);
        const int seconds = std::atoi (argv[3]);

        m4lb::SharedTransport transport;

        for (int i = 0; i < 200 && ! transport.valid(); ++i)
        {
            transport.attach (pid);
            if (! transport.valid())
                Sleep (50);
        }

        if (! transport.valid())
        {
            std::printf ("FAIL: no shared memory for pid %u\n", (unsigned) pid);
            return 1;
        }

        for (int i = 0; i < seconds * 10; ++i)
            Sleep (100);

        const auto& h = transport.layout()->header;
        std::printf ("pid=%u heartbeat=%u sr=%u underruns=%u overruns=%u\n",
                     (unsigned) pid, h.heartbeat.load(), h.sampleRate.load(),
                     h.underruns.load(), h.overruns.load());

        return h.heartbeat.load() > 0 ? 0 : 1;
    }

    if (argc < 3)
    {
        std::printf ("usage: M4LBTest <Max.exe> <loopback.maxpat> [waitSeconds] [flow]\n");
        std::printf ("       M4LBTest attach <pid> <seconds>\n");
        return 2;
    }

    const int waitSeconds = argc > 3 ? std::atoi (argv[3]) : 20;
    const bool flowOnly = argc > 4 && std::strcmp (argv[4], "flow") == 0;

    std::string commandLine = std::string ("\"") + argv[1] + "\" -nosplash \"" + argv[2] + "\"";
    std::vector<char> cmd (commandLine.begin(), commandLine.end());
    cmd.push_back ('\0');

    STARTUPINFOA si {};
    si.cb = sizeof (si);
    PROCESS_INFORMATION pi {};

    if (! CreateProcessA (nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
    {
        std::printf ("CreateProcess failed: %lu\n", GetLastError());
        return 1;
    }

    CloseHandle (pi.hThread);
    const auto pid = pi.dwProcessId;
    std::printf ("launched Max pid=%lu\n", (unsigned long) pid);

    m4lb::SharedTransport transport;
    bool attached = false;

    for (int i = 0; i < 200 && ! attached; ++i)
    {
        if (transport.attach (pid) && transport.valid())
            attached = true;
        else
            Sleep (50);
    }

    if (! attached)
    {
        std::printf ("FAIL: could not attach to shared memory\n");
        TerminateProcess (pi.hProcess, 1);
        CloseHandle (pi.hProcess);
        return 1;
    }

    auto* layout = transport.layout();
    auto& header = layout->header;

    std::printf ("attached (createdByUs=%d)\n", transport.createdByUs() ? 1 : 0);

    const int frames = 4096;
    std::vector<float> left ((size_t) frames), right ((size_t) frames);

    // A distinctive ramp: the non-zero offset means leading silence produced
    // before Max starts ticking can be told apart from the actual signal.
    for (int i = 0; i < frames; ++i)
    {
        left[(size_t) i]  = 0.25f + 0.5f * std::sin (2.0f * 3.14159265f * (float) i / 64.0f);
        right[(size_t) i] = left[(size_t) i] * 0.5f;
    }

    const float* src[m4lb::kMaxChannels] = { left.data(), right.data() };
    pushFrameBlock (layout, 2, src, frames);
    std::printf ("pushed %d frames\n", frames);

    // Wait for Max's DSP to consume the input and produce the same amount.
    int produced = 0;

    for (int i = 0; i < waitSeconds * 20; ++i)
    {
        const int fill = m4lb::ringFill (header.outWrite.load (std::memory_order_acquire),
                                         header.outRead.load (std::memory_order_relaxed));

        if (fill >= frames)
        {
            produced = fill;
            break;
        }

        Sleep (50);
    }

    std::printf ("out ring fill: %d (heartbeat=%u, underruns=%u)\n",
                 produced, header.heartbeat.load(), header.underruns.load());

    if (produced < frames)
    {
        std::printf ("FAIL: Max did not move the audio through (fill=%d)\n", produced);
        TerminateProcess (pi.hProcess, 1);
        CloseHandle (pi.hProcess);
        return 1;
    }

    if (flowOnly)
    {
        std::printf ("FLOW OK: %d frames came back through the chain\n", produced);
        TerminateProcess (pi.hProcess, 0);
        CloseHandle (pi.hProcess);
        return 0;
    }

    std::vector<float> outL ((size_t) produced), outR ((size_t) produced);
    float* dst[m4lb::kMaxChannels] = { outL.data(), outR.data() };
    float* rings[m4lb::kMaxChannels] = { layout->outAudio[0], layout->outAudio[1] };
    m4lb::ringRead (rings, 2, header.outRead, dst, produced);

    // Find where our ramp starts (silence may have been produced before the
    // ring was filled).
    int start = -1;

    for (int i = 0; i + 8 < produced; ++i)
    {
        bool match = true;

        for (int k = 0; k < 8 && match; ++k)
            match = std::abs (outL[(size_t) (i + k)] - left[(size_t) k]) < 1.0e-6f;

        if (match)
        {
            start = i;
            break;
        }
    }

    if (start < 0 || start + frames > produced)
    {
        std::printf ("FAIL: could not find the ramp in the output (fill=%d)\n", produced);
        TerminateProcess (pi.hProcess, 1);
        CloseHandle (pi.hProcess);
        return 1;
    }

    int mismatches = 0;
    float maxDiff = 0.0f;

    for (int i = 0; i < frames; ++i)
    {
        const float d = std::abs (outL[(size_t) (start + i)] - left[(size_t) i]);
        maxDiff = d > maxDiff ? d : maxDiff;

        if (d > 1.0e-6f)
        {
            if (mismatches == 0)
                std::printf ("first mismatch at %d: expected %g got %g\n",
                             i, (double) left[(size_t) i], (double) outL[(size_t) (start + i)]);

            ++mismatches;
        }
    }

    std::printf ("ramp at %d; compared %d frames: mismatches=%d maxDiff=%g\n",
                 start, frames, mismatches, (double) maxDiff);

    TerminateProcess (pi.hProcess, 0);
    CloseHandle (pi.hProcess);

    if (mismatches == 0)
    {
        std::printf ("PASS\n");
        return 0;
    }

    std::printf ("FAIL\n");
    return 1;
}
