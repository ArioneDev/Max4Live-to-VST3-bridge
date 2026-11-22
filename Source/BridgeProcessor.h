#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

inline constexpr const char* kBridgeVersion = "0.9.5";

#include "SharedProtocol.h"
#include "SharedTransport.h"

#include <array>
#include <atomic>

// Host-side plugin.  It owns the shared-memory ring, spawns a Max process that
// hosts the device, and pumps audio/MIDI between the host and that process.
class BridgeAudioProcessor final : public juce::AudioProcessor
{
public:
    BridgeAudioProcessor();
    ~BridgeAudioProcessor() override;

    //==========================================================================
    const juce::String getName() const override { return "M4L Bridge"; }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getProgramName (int) override { return {}; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==========================================================================
    // Editor-facing API (message thread only).
    juce::String getDevicePath() const;
    void setDevicePath (const juce::String& newPath);
    juce::String getMaxExePath() const;
    void setMaxExePath (const juce::String& newPath);
    void restartMaxSide();
    void setTestTone (bool shouldPlay);
    bool isTestTone() const;
    void setBypassDevice (bool shouldBypass);
    void setDirectTone (bool shouldPlay);
    bool isDirectTone() const;
    bool isBypassDevice() const;
    void setLoopbackMode (bool shouldLoopback);
    bool isLoopbackMode() const;

    juce::String getStatusText() const;
    juce::String getLastError() const;
    juce::String getDiagnostics() const;
    float getInputDb() const { return uiInDb.load(); }
    float getOutputDb() const { return uiOutDb.load(); }
    int getLatencyFrames() const { return primeFrames + 64; }
    double getHostSampleRate() const { return hostSampleRate; }
    bool isEngineRunning() const { return isMaxAlive(); }
    juce::String takeStartupWarning();
    bool hasUnreportedError() const;
    void markErrorReported();
    double getMaxSampleRate() const;
    uint32_t getUnderruns() const;
    uint32_t getOverruns() const;
    uint32_t getHeartbeat() const;

private:
    struct LinearResampler
    {
        double pos = 0.0;
        float prev[m4lb::kMaxChannels] {};
        bool hasPrev = false;

        void reset()
        {
            pos = 0.0;
            hasPrev = false;
            for (auto& p : prev)
                p = 0.0f;
        }

        // ratio = input samples consumed per output sample.  Both channels
        // share the fractional position so they stay sample-aligned.
        int process (const float* const* in, float* const* out, int channels,
                     int numIn, int maxOut, double ratio)
        {
            if (numIn <= 0 || maxOut <= 0)
                return 0;

            if (! hasPrev)
            {
                for (int c = 0; c < channels; ++c)
                    prev[c] = in[c][0];

                hasPrev = true;
            }

            int produced = 0;

            while (produced < maxOut && pos <= (double) (numIn - 1))
            {
                const int i = (int) std::floor (pos);
                const double f = pos - (double) i;

                for (int c = 0; c < channels; ++c)
                {
                    const float a = (i == 0) ? prev[c] : in[c][i - 1];
                    const float b = in[c][i];
                    out[c][produced] = (float) ((double) a + ((double) b - (double) a) * f);
                }

                ++produced;
                pos += ratio;
            }

            pos -= (double) numIn;

            for (int c = 0; c < channels; ++c)
                prev[c] = in[c][numIn - 1];

            if (pos < 0.0)
                pos = 0.0;

            return produced;
        }
    };

    void startMaxSide();
    void stopMaxSide();
    bool isMaxAlive() const;

    juce::File writePatchFile() const;
    void pushInput (const juce::AudioBuffer<float>& buffer);
    void pullOutput (juce::AudioBuffer<float>& buffer);
    void pushMidi (const juce::MidiBuffer& midi);
    void pullMidi (juce::MidiBuffer& midi);

    juce::String devicePath;
    juce::String maxExePath;

    uint32_t maxPid = 0;
    void* maxProcessHandle = nullptr; // HANDLE
    juce::File currentPatchFile;
    mutable juce::String lastError;
    std::atomic<bool> errorUnreported { false };

    m4lb::SharedTransport transport;
    static constexpr int numParams = 32;

    std::unique_ptr<juce::DatagramSocket> oscSocket;
    int oscPort = 0;
    uint32_t shmKey = 0;
    uint32_t lastHeartbeat = 0;
    juce::int64 lastHeartbeatMs = 0;
    int feedbackPort = 0;
    std::unique_ptr<juce::Thread> feedbackThread;
    std::array<std::atomic<float>, numParams> paramMin {};
    std::array<std::atomic<float>, numParams> paramMax {};
    std::array<float, numParams> lastSentParam {};
    std::atomic<bool> paramsRequested { false };
    std::atomic<bool> gotParamInfo { false };
    juce::int64 lastGpMs = 0;
    double lastSentBpm = 0.0;
    juce::int64 lastTempoMs = 0;
    juce::CriticalSection transportLock;

    double hostSampleRate = 48000.0;
    int hostBlockSize = 512;
    double shimSampleRate = 0.0;
    int shimChannels = m4lb::kMaxChannels;

    juce::AudioBuffer<float> inScratch;
    juce::AudioBuffer<float> outScratch;
    juce::AudioBuffer<float> monoScratch;
    LinearResampler inResampler;
    LinearResampler outResampler;

    bool loopback = false;
    bool primed = false;
    int primeFrames = 1024;      // adaptive jitter buffer, grows on underruns
    std::atomic<float> uiInDb { -100.0f };
    std::atomic<float> uiOutDb { -100.0f };
    int meterBlocks = 0;
    int instanceId = 0;
    std::atomic<uint32_t> midiInCount { 0 };
    std::atomic<uint32_t> midiCountOut { 0 };
    juce::int64 maxLaunchMs = 0;
    std::atomic<bool> testTone { false };
    std::atomic<bool> bypassDevice { false };
    std::atomic<bool> directTone { false };
    double testPhase = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BridgeAudioProcessor)
};
