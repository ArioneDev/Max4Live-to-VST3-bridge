// Max-side shim plugins.
//
// Two builds of this file are produced:
//   M4LB_SHIM_MODE = 0  -> "M4L Bridge Out": turns the host's audio (from the
//                          shared ring) into a Max signal that feeds amxd~.
//   M4LB_SHIM_MODE = 1  -> "M4L Bridge In":  captures the amxd~ output signal
//                          into the shared ring for the host.
//
// They are loaded by Max's [vst~] using an absolute path, so no plug-in scan
// or installation into a system VST3 folder is required.

#include <juce_audio_processors/juce_audio_processors.h>

#include "SharedProtocol.h"
#include "SharedTransport.h"

#include <windows.h>
#include <cmath>

#ifndef M4LB_SHIM_MODE
 #error "M4LB_SHIM_MODE must be defined (0 = out, 1 = in)"
#endif

namespace
{

class ShimProcessor final : public juce::AudioProcessor
{
public:
    ShimProcessor()
        : juce::AudioProcessor (BusesProperties()
              .withInput  ("In",  juce::AudioChannelSet::stereo(), true)
              .withOutput ("Out", juce::AudioChannelSet::stereo(), true))
    {
        idParam = new juce::AudioParameterFloat ("bridgeid", "bridgeid", 0.0f, 1.0f, 0.0f);
        addParameter (idParam);
    }

    const juce::String getName() const override
    {
       #if M4LB_SHIM_MODE == 0
        return "M4L Bridge Out";
       #else
        return "M4L Bridge In";
       #endif
    }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override
    {
        ensureAttached();

        if (auto* layout = transport.layout())
        {
            auto& h = layout->header;
            h.sampleRate.store ((uint32_t) juce::jmax (1.0, sampleRate), std::memory_order_relaxed);
            h.blockSize.store ((uint32_t) samplesPerBlock, std::memory_order_relaxed);
            h.channels.store ((uint32_t) m4lb::kMaxChannels, std::memory_order_relaxed);
            h.state.fetch_or (m4lb::kStateRunning, std::memory_order_release);
        }
    }

    void releaseResources() override
    {
        if (auto* layout = transport.layout())
            layout->header.state.fetch_and (~m4lb::kStateRunning, std::memory_order_release);
    }

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        return layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
            && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        ensureAttached();

        auto* layout = transport.layout();
        if (layout == nullptr)
        {
            buffer.clear();
            return;
        }

        auto& h = layout->header;
        h.heartbeat.fetch_add (1, std::memory_order_relaxed);

        const int numSamples = buffer.getNumSamples();
        const int numChans = juce::jmin (m4lb::kMaxChannels, buffer.getNumChannels());

        float* rings[m4lb::kMaxChannels] = { layout->inAudio[0], layout->inAudio[1] };
        float* outRings[m4lb::kMaxChannels] = { layout->outAudio[0], layout->outAudio[1] };
        float* dst[m4lb::kMaxChannels] = {};
        const float* src[m4lb::kMaxChannels] = {};

        for (int c = 0; c < numChans; ++c)
        {
            dst[c] = buffer.getWritePointer (c);
            src[c] = buffer.getReadPointer (c);
        }

       #if M4LB_SHIM_MODE == 0
        // Host -> Max: drain the input ring into our output signal.
        {
            const int available = m4lb::ringFill (h.inWrite.load (std::memory_order_acquire),
                                                  h.inRead.load (std::memory_order_relaxed));
            const int take = juce::jlimit (0, numSamples, available);

            if (take > 0)
                m4lb::ringRead (rings, numChans, h.inRead, dst, take);

            if (take < numSamples)
            {
                for (int c = 0; c < numChans; ++c)
                    buffer.clear (c, take, numSamples - take);

                h.underruns.fetch_add (1, std::memory_order_relaxed);
            }
        }

        // Commands / MIDI from the host.
        {
            m4lb::MidiEvent ev;
            while (m4lb::popMidi (layout->midiToMax, h.midiToMaxWrite, h.midiToMaxRead, ev))
                midi.addEvent (ev.data, (int) ev.size, (int) juce::jlimit (0u, (uint32_t) numSamples, ev.offset));
        }
       #else
        // Max -> Host: capture the signal that came out of amxd~.
        {
            float peak = 0.0f;

            for (int c = 0; c < numChans; ++c)
                peak = juce::jmax (peak, buffer.getMagnitude (c, 0, numSamples));

            uint32_t bits = 0;
            std::memcpy (&bits, &peak, sizeof (bits));
            h.meterDeviceOut.store (bits, std::memory_order_relaxed);

            const int fill = m4lb::ringFill (h.outWrite.load (std::memory_order_relaxed),
                                             h.outRead.load (std::memory_order_relaxed));

            if (fill + numSamples <= m4lb::kRingFrames)
                m4lb::ringWrite (outRings, numChans, h.outWrite, src, numSamples);
            else
                h.overruns.fetch_add (1, std::memory_order_relaxed);
        }

        // MIDI / events from Max (sent as midievent messages to this vst~).
        {
            for (const auto metadata : midi)
            {
                const auto& m = metadata.getMessage();
                const int size = m.getRawDataSize();
                if (size <= 0 || size > 3)
                    continue;

                m4lb::MidiEvent ev;
                ev.offset = (uint32_t) juce::jmax (0, metadata.samplePosition);
                ev.size = (uint8_t) size;
                const auto* raw = m.getRawData();
                for (int i = 0; i < size; ++i)
                    ev.data[i] = (uint8_t) raw[i];

                m4lb::pushMidi (layout->midiToHost, h.midiToHostWrite, h.midiToHostRead, ev);
            }
        }

        // This shim only exists to be pulled by the DSP graph.
        buffer.clear();
       #endif
    }

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }

    const juce::String getProgramName (int) override { return {}; }
    bool acceptsMidi() const override
    {
       #if M4LB_SHIM_MODE == 0
        return false;
       #else
        return true;
       #endif
    }
    bool producesMidi() const override
    {
       #if M4LB_SHIM_MODE == 0
        return true;
       #else
        return false;
       #endif
    }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

private:
    void ensureAttached()
    {
        const uint32_t id = (uint32_t) std::lround ((double) idParam->get() * 999999.0);
        const uint32_t key = id != 0 ? id : (uint32_t) GetCurrentProcessId();

        if (transport.isAttached() && attachedKey == key)
            return;

        transport.attach (key);
        attachedKey = key;
    }

    juce::AudioParameterFloat* idParam = nullptr;
    uint32_t attachedKey = 0;
    m4lb::SharedTransport transport;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ShimProcessor)
};

} // namespace

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ShimProcessor();
}

