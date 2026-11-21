// M4L Bridge - shared memory protocol between the VST3 (host side) and the
// two shim plugins that run inside Max (Max side).
//
// Layout: one named file mapping per bridge instance, named
//   Local\M4LBridge_<maxPid>
// The Max-side shim derives the PID from GetCurrentProcessId(); the host side
// gets it from the process handle returned by CreateProcess().
//
// Audio is stored as per-channel planar float rings.  MIDI is a small ring of
// fixed-size events.  All indices are free-running 32-bit counters masked to
// the ring size; the difference (write - read) is the fill level.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

namespace m4lb
{
inline constexpr uint32_t kMagic = 0x4D344C42u; // 'M4LB'
inline constexpr uint32_t kVersion = 1u;

inline constexpr int kMaxChannels = 2;
inline constexpr int kRingFrames = 1 << 15; // 32768 frames (~0.74 s @ 44.1 kHz)
inline constexpr int kMidiEvents = 4096;
inline constexpr int kMaxBlock = 4096;

enum StateFlags : uint32_t
{
    kStateRunning = 1u << 0,
    kStateDeviceLoaded = 1u << 1,
    kStateError = 1u << 2,
    kStateMaxGone = 1u << 3
};

struct MidiEvent
{
    uint32_t offset = 0; // sample offset inside the producing block
    uint8_t size = 0;
    uint8_t data[3] = {0, 0, 0};
    uint8_t pad = 0;
};

struct SharedHeader
{
    std::atomic<uint32_t> magic {0};
    std::atomic<uint32_t> version {0};
    std::atomic<uint32_t> sampleRate {0};
    std::atomic<uint32_t> blockSize {0};
    std::atomic<uint32_t> channels {0};
    std::atomic<uint32_t> state {0};

    // "in"  = host -> Max (audio the device should process)
    // "out" = Max -> host (audio the device produced)
    std::atomic<uint32_t> inWrite {0};
    std::atomic<uint32_t> inRead {0};
    std::atomic<uint32_t> outWrite {0};
    std::atomic<uint32_t> outRead {0};

    std::atomic<uint32_t> midiToMaxWrite {0};
    std::atomic<uint32_t> midiToMaxRead {0};
    std::atomic<uint32_t> midiToHostWrite {0};
    std::atomic<uint32_t> midiToHostRead {0};

    std::atomic<uint32_t> latencyFrames {0};
    std::atomic<uint32_t> heartbeat {0};
    std::atomic<uint32_t> underruns {0};
    std::atomic<uint32_t> overruns {0};
    std::atomic<uint32_t> deviceSampleRate {0};

    // Peak meters (float bits): host input, device output, host output.
    std::atomic<uint32_t> meterHostIn {0};
    std::atomic<uint32_t> meterDeviceOut {0};
    std::atomic<uint32_t> meterHostOut {0};
    std::atomic<uint32_t> reserved[5] {};
};

struct SharedLayout
{
    SharedHeader header;
    float inAudio[kMaxChannels][kRingFrames];
    float outAudio[kMaxChannels][kRingFrames];
    MidiEvent midiToMax[kMidiEvents];
    MidiEvent midiToHost[kMidiEvents];
};

inline constexpr uint64_t kSharedBytes = sizeof (SharedLayout);

// ---------------------------------------------------------------------------
// Ring helpers.  Single producer / single consumer; positions are the only
// shared state and are updated with release stores / acquire loads.
// ---------------------------------------------------------------------------

inline int ringFill (uint32_t writePos, uint32_t readPos) noexcept
{
    return (int) (int32_t) (writePos - readPos);
}

inline void ringWrite (float* const* rings, int channels, std::atomic<uint32_t>& pos,
                       const float* const* srcs, int numSamples) noexcept
{
    const uint32_t p0 = pos.load (std::memory_order_relaxed);
    const int start = (int) (p0 & (uint32_t) (kRingFrames - 1));
    const int first = numSamples < kRingFrames - start ? numSamples : kRingFrames - start;

    for (int c = 0; c < channels; ++c)
    {
        std::memcpy (rings[c] + start, srcs[c], sizeof (float) * (size_t) first);
        if (numSamples > first)
            std::memcpy (rings[c], srcs[c] + first, sizeof (float) * (size_t) (numSamples - first));
    }

    pos.store (p0 + (uint32_t) numSamples, std::memory_order_release);
}

inline void ringRead (float* const* rings, int channels, std::atomic<uint32_t>& pos,
                      float* const* dsts, int numSamples) noexcept
{
    const uint32_t p0 = pos.load (std::memory_order_relaxed);
    const int start = (int) (p0 & (uint32_t) (kRingFrames - 1));
    const int first = numSamples < kRingFrames - start ? numSamples : kRingFrames - start;

    for (int c = 0; c < channels; ++c)
    {
        std::memcpy (dsts[c], rings[c] + start, sizeof (float) * (size_t) first);
        if (numSamples > first)
            std::memcpy (dsts[c] + first, rings[c], sizeof (float) * (size_t) (numSamples - first));
    }

    pos.store (p0 + (uint32_t) numSamples, std::memory_order_release);
}

inline bool pushMidi (MidiEvent* ring, std::atomic<uint32_t>& writePos, std::atomic<uint32_t>& readPos,
                      const MidiEvent& ev) noexcept
{
    const uint32_t w = writePos.load (std::memory_order_relaxed);
    const uint32_t r = readPos.load (std::memory_order_acquire);
    if (ringFill (w, r) >= kMidiEvents)
        return false;

    ring[w & (uint32_t) (kMidiEvents - 1)] = ev;
    writePos.store (w + 1, std::memory_order_release);
    return true;
}

inline bool popMidi (MidiEvent* ring, std::atomic<uint32_t>& writePos, std::atomic<uint32_t>& readPos,
                     MidiEvent& ev) noexcept
{
    const uint32_t r = readPos.load (std::memory_order_relaxed);
    const uint32_t w = writePos.load (std::memory_order_acquire);
    if (w == r)
        return false;

    ev = ring[r & (uint32_t) (kMidiEvents - 1)];
    readPos.store (r + 1, std::memory_order_release);
    return true;
}

} // namespace m4lb
