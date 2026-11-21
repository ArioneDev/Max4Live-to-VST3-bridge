#include "BridgeProcessor.h"
#include "BridgeLog.h"

#include "BridgeEditor.h"
#include "BridgePaths.h"
#include "PatchGenerator.h"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <cstdio>
#include <vector>



static std::atomic<int> gInstanceCounter { 0 };

static void appendOscString (std::vector<uint8_t>& out, const char* s)
{
    const auto len = (size_t) std::strlen (s) + 1;
    out.insert (out.end(), s, s + len);

    while ((out.size() & 3) != 0)
        out.push_back (0);
}

static void appendOscFloat (std::vector<uint8_t>& out, float f)
{
    uint32_t bits = 0;
    std::memcpy (&bits, &f, sizeof (bits));

    out.push_back ((uint8_t) ((bits >> 24) & 0xff));
    out.push_back ((uint8_t) ((bits >> 16) & 0xff));
    out.push_back ((uint8_t) ((bits >> 8) & 0xff));
    out.push_back ((uint8_t) (bits & 0xff));
}

static void appendOscInt (std::vector<uint8_t>& out, int32_t v)
{
    out.push_back ((uint8_t) ((v >> 24) & 0xff));
    out.push_back ((uint8_t) ((v >> 16) & 0xff));
    out.push_back ((uint8_t) ((v >> 8) & 0xff));
    out.push_back ((uint8_t) (v & 0xff));
}

static int pickFreeUdpPort()
{
    for (int i = 0; i < 64; ++i)
    {
        const int port = 41000 + juce::Random::getSystemRandom().nextInt (19000);
        juce::DatagramSocket probe;

        if (probe.bindToPort (port))
        {
            probe.shutdown();
            return port;
        }
    }

    return 0;
}

static void rawLog (const char* msg)
{
    std::fprintf (stderr, "M4LB: %s\n", msg);
    std::fflush (stderr);
    std::ofstream f ("C:/Temp/m4lprobe/plugin_dbg.txt", std::ios::app);
    f << msg << "\n";
}

#if M4LBRIDGE_INSTRUMENT
 #define M4LB_BUSES BusesProperties().withOutput ("Out", juce::AudioChannelSet::stereo(), true)
#else
 #define M4LB_BUSES BusesProperties().withInput ("In", juce::AudioChannelSet::stereo(), true) \
                                   .withOutput ("Out", juce::AudioChannelSet::stereo(), true)
#endif

BridgeAudioProcessor::BridgeAudioProcessor()
    : juce::AudioProcessor (M4LB_BUSES)
{
    // Handy for tests and for hosts that restore state late.
    if (auto env = juce::SystemStats::getEnvironmentVariable ("M4LBRIDGE_DEVICE", {}); env.isNotEmpty())
        devicePath = env;

    if (auto env = juce::SystemStats::getEnvironmentVariable ("M4LBRIDGE_MAX_EXE", {}); env.isNotEmpty())
        maxExePath = env;

    rawLog ("Ctor"); m4lb::log ("ctor module=" + m4lb::moduleFile().getFullPathName()
               + " device=" + devicePath);

    m4lb::log ("ctor detected max=" + m4lb::findMaxExecutable (maxExePath).getFullPathName()
               + " shimOut=" + m4lb::findShimBundle ("M4L Bridge Out.vst3").getFullPathName());
}

BridgeAudioProcessor::~BridgeAudioProcessor()
{
    stopMaxSide();
}

//==============================================================================
void BridgeAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    // The jitter buffer adds a fixed latency; tell the host so it can compensate.
    setLatencySamples (primeFrames + 64);
    primed = false;
    rawLog ("PrepareToPlay");
    m4lb::log ("prepareToPlay sr=" + juce::String (sampleRate)
               + " in=" + juce::String (getTotalNumInputChannels())
               + " out=" + juce::String (getTotalNumOutputChannels())
               + " block=" + juce::String (samplesPerBlock)
               + " device=" + devicePath);
    hostSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    primeFrames = 1024;
    hostBlockSize = juce::jmax (1, samplesPerBlock);

    const int scratch = juce::jmax (8192, hostBlockSize * 8);
    inScratch.setSize (m4lb::kMaxChannels, scratch, false, true, true);
    outScratch.setSize (m4lb::kMaxChannels, scratch, false, true, true);
    monoScratch.setSize (1, scratch, false, true, true);

    inScratch.clear();
    outScratch.clear();
    inResampler.reset();
    outResampler.reset();
    primed = false;

    // Only launch here if the editor has not already done it; restarting on
    // every prepare would kill a Max instance that is already running.
    if (! isMaxAlive() || ! transport.isAttached())
        startMaxSide();
}

void BridgeAudioProcessor::releaseResources()
{
}

bool BridgeAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in  = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();

   #if M4LBRIDGE_INSTRUMENT
    juce::ignoreUnused (in);
    return out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
   #else
    if (in != out)
        return false;

    return in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo();
   #endif
}

//==============================================================================
void BridgeAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    // Max may live in another process than the one we spawned (window handoff),
    // so liveness is judged by the shim heartbeat rather than the process handle.
    if (transport.isAttached() && transport.valid())
    {
        const auto hb = transport.layout()->header.heartbeat.load();
        const auto now = juce::Time::currentTimeMillis();

        if (hb != lastHeartbeat)
        {
            lastHeartbeat = hb;
            lastHeartbeatMs = now;
        }
        else if (lastHeartbeatMs != 0 && now - lastHeartbeatMs > 6000)
        {
            stopMaxSide();
            lastError = "Max stopped responding - press Restart engine";
            errorUnreported = true;
            buffer.clear();
            return;
        }
    }

    // Diagnostic mode: let the host signal pass untouched.
    if (bypassDevice.load (std::memory_order_relaxed))
        return;

    // Diagnostic mode: write a tone straight to the output bus, bypassing the
    // bridge entirely.  Tells us whether the host is receiving our output.
    if (directTone.load (std::memory_order_relaxed))
    {
        const int n = buffer.getNumSamples();
        const double inc = 2.0 * 3.141592653589793 * 440.0 / hostSampleRate;

        for (int i = 0; i < n; ++i)
        {
            const auto s = (float) std::sin (testPhase) * 0.25f;
            testPhase += inc;
            if (testPhase > 2.0 * 3.141592653589793)
                testPhase -= 2.0 * 3.141592653589793;
            for (int c = 0; c < buffer.getNumChannels(); ++c)
                buffer.setSample (c, i, s);
        }

        return;
    }

    // NB: do NOT clear the buffer here.  Input and output share it, and
    // clearing would wipe the host's audio before we ever read it.

    // Diagnostic mode: feed a 440 Hz tone instead of the host input.  This runs
    // even when Max is not connected, so the meter always gives feedback.
    if (testTone.load (std::memory_order_relaxed))
    {
        const int numSamples = buffer.getNumSamples();
        const double inc = 2.0 * 3.141592653589793 * 440.0 / hostSampleRate;

        for (int i = 0; i < numSamples; ++i)
        {
            const auto s = (float) std::sin (testPhase) * 0.25f;
            testPhase += inc;

            if (testPhase > 2.0 * 3.141592653589793)
                testPhase -= 2.0 * 3.141592653589793;

            for (int c = 0; c < buffer.getNumChannels(); ++c)
                buffer.setSample (c, i, s);
        }
    }

    const int channels = juce::jmin (m4lb::kMaxChannels, buffer.getNumChannels());

    float inPeak = 0.0f;
    for (int c = 0; c < channels; ++c)
        inPeak = juce::jmax (inPeak, buffer.getMagnitude (c, 0, buffer.getNumSamples()));

    bool attached = false;

    {
        const juce::ScopedTryLock tryLock (transportLock);

        if (tryLock.isLocked() && transport.isAttached() && transport.valid())
        {
            attached = true;

            auto* layout = transport.layout();
            auto& h = layout->header;

            const auto reportedRate = h.sampleRate.load (std::memory_order_relaxed);
            shimSampleRate = reportedRate > 0 ? (double) reportedRate : hostSampleRate;

            pushInput (buffer);
            pushMidi (midi);
            pullOutput (buffer);
            pullMidi (midi);

            const auto toBits = [] (float v)
            {
                uint32_t b = 0;
                std::memcpy (&b, &v, sizeof (b));
                return b;
            };

            h.meterHostIn.store (toBits (inPeak), std::memory_order_relaxed);
        }
    }

    float outPeak = 0.0f;

    // No Max connection: output silence rather than pretending the device ran.
    if (! attached)
        buffer.clear();

    for (int c = 0; c < channels; ++c)
        outPeak = juce::jmax (outPeak, buffer.getMagnitude (c, 0, buffer.getNumSamples()));

    if (attached && oscSocket != nullptr && oscPort > 0)
    {
        if (auto* ph = getPlayHead())
        {
            if (auto pos = ph->getPosition())
            {
                if (auto bpm = pos->getBpm())
                {
                    const auto b = *bpm;
                    const auto nowMs = juce::Time::currentTimeMillis();

                    if (std::abs (b - lastSentBpm) > 0.01 || nowMs - lastTempoMs > 1000)
                    {
                        lastSentBpm = b;
                        lastTempoMs = nowMs;

                        std::vector<uint8_t> packet;
                        appendOscString (packet, "/t");
                        appendOscString (packet, ",f");
                        appendOscFloat (packet, (float) b);
                        oscSocket->write ("127.0.0.1", oscPort, packet.data(), (int) packet.size());
                    }
                }
            }
        }
    }

    uiInDb.store (juce::Decibels::gainToDecibels (inPeak), std::memory_order_relaxed);
    uiOutDb.store (juce::Decibels::gainToDecibels (outPeak), std::memory_order_relaxed);

    {
        const juce::ScopedTryLock tryLock (transportLock);

        if (tryLock.isLocked() && transport.isAttached() && transport.valid())
        {
            const auto toBits = [] (float v)
            {
                uint32_t b = 0;
                std::memcpy (&b, &v, sizeof (b));
                return b;
            };

            transport.layout()->header.meterHostOut.store (toBits (outPeak), std::memory_order_relaxed);
        }
    }

    if ((++meterBlocks % 100) == 0)
    {
        const auto line = "meters in=" + juce::String (juce::Decibels::gainToDecibels (inPeak), 1)
                        + "dB out=" + juce::String (juce::Decibels::gainToDecibels (outPeak), 1)
                        + "dB blocks=" + juce::String (meterBlocks)
                        + " chans=" + juce::String (channels)
                        + (attached ? " attached=1" : " attached=0")
                        + " n=" + juce::String (buffer.getNumSamples())
                        + " midiIn=" + juce::String ((int) midiInCount.load())
                        + " midiOut=" + juce::String ((int) midiCountOut.load())
                        + " inst=" + juce::String (instanceId)
                        + " inFill=" + juce::String (attached ? m4lb::ringFill (transport.layout()->header.inWrite.load(), transport.layout()->header.inRead.load()) : 0)
                        + " outFill=" + juce::String (attached ? m4lb::ringFill (transport.layout()->header.outWrite.load(), transport.layout()->header.outRead.load()) : 0);

        rawLog (line.toRawUTF8());
        m4lb::log (line);
    }
}

//==============================================================================
void BridgeAudioProcessor::pushInput (const juce::AudioBuffer<float>& buffer)
{
   #if M4LBRIDGE_INSTRUMENT
    // Instrument build: there is no audio input, feed silence so the device
    // sees a valid stream (and the ring does not report underruns).
    {
        auto* layout = transport.layout();
        auto& h = layout->header;
        const int numSamples = buffer.getNumSamples();

        if (numSamples <= 0 || numSamples > inScratch.getNumSamples())
            return;

        inScratch.clear (0, numSamples);
        const float* src[m4lb::kMaxChannels] = { inScratch.getReadPointer (0),
                                                 inScratch.getReadPointer (1) };
        float* rings[m4lb::kMaxChannels] = { layout->inAudio[0], layout->inAudio[1] };

        if (m4lb::ringFill (h.inWrite.load(), h.inRead.load()) + numSamples > m4lb::kRingFrames)
        {
            h.overruns.fetch_add (1, std::memory_order_relaxed);
            return;
        }

        m4lb::ringWrite (rings, 2, h.inWrite, src, numSamples);
        return;
    }
   #endif

    auto* layout = transport.layout();
    auto& h = layout->header;

    const int numSamples = buffer.getNumSamples();
    const int channels = juce::jmin (m4lb::kMaxChannels, buffer.getNumChannels());

    float* rings[m4lb::kMaxChannels] = { layout->inAudio[0], layout->inAudio[1] };

    const double ratio = hostSampleRate / shimSampleRate; // input per output

    if (std::abs (ratio - 1.0) < 1e-6)
    {
        if (m4lb::ringFill (h.inWrite.load (std::memory_order_relaxed),
                            h.inRead.load (std::memory_order_relaxed)) + numSamples > m4lb::kRingFrames)
        {
            h.overruns.fetch_add (1, std::memory_order_relaxed);
            return;
        }

        const float* src[m4lb::kMaxChannels] = { buffer.getReadPointer (0),
                                                 buffer.getReadPointer (channels > 1 ? 1 : 0) };

        m4lb::ringWrite (rings, channels, h.inWrite, src, numSamples);
        return;
    }

    const int capacity = inScratch.getNumSamples();
    const int wanted = juce::jmin (capacity, (int) std::ceil ((double) numSamples / ratio) + 1);

    const float* src[m4lb::kMaxChannels] = { buffer.getReadPointer (0),
                                             buffer.getReadPointer (channels > 1 ? 1 : 0) };
    float* dst[m4lb::kMaxChannels] = { inScratch.getWritePointer (0), inScratch.getWritePointer (1) };

    const int produced = inResampler.process (src, dst, channels, numSamples, wanted, ratio);
    if (produced <= 0)
        return;

    if (m4lb::ringFill (h.inWrite.load (std::memory_order_relaxed),
                        h.inRead.load (std::memory_order_relaxed)) + produced > m4lb::kRingFrames)
    {
        h.overruns.fetch_add (1, std::memory_order_relaxed);
        return;
    }

    m4lb::ringWrite (rings, channels, h.inWrite, dst, produced);
}

void BridgeAudioProcessor::pullOutput (juce::AudioBuffer<float>& buffer)
{
    auto* layout = transport.layout();
    auto& h = layout->header;

    const int numSamples = buffer.getNumSamples();
    const int channels = juce::jmin (m4lb::kMaxChannels, buffer.getNumChannels());

    float* rings[m4lb::kMaxChannels] = { layout->outAudio[0], layout->outAudio[1] };

    const double ratio = shimSampleRate / hostSampleRate; // input per output

    const int available = m4lb::ringFill (h.outWrite.load (std::memory_order_acquire),
                                          h.outRead.load (std::memory_order_relaxed));

    // Jitter buffer: wait until enough audio has accumulated before consuming.
    // Without this the read pointer runs ahead of Max's producer and every
    // block gets padded with zeros (the "bitcrush" sound).
    if (! primed)
    {
        if (available < primeFrames)
        {
            buffer.clear();
            h.underruns.fetch_add (1, std::memory_order_relaxed);
            return;
        }

        primed = true;
    }

    if (std::abs (ratio - 1.0) < 1e-6)
    {
        const int take = juce::jmin (numSamples, available);

        float* dst[m4lb::kMaxChannels] = { buffer.getWritePointer (0),
                                           channels > 1 ? buffer.getWritePointer (1)
                                                        : monoScratch.getWritePointer (0) };

        if (take > 0)
            m4lb::ringRead (rings, channels, h.outRead, dst, take);

        if (take < numSamples)
        {
            for (int c = 0; c < channels; ++c)
                buffer.clear (c, take, numSamples - take);

            h.underruns.fetch_add (1, std::memory_order_relaxed);
            primed = false; // re-prime instead of crackling every block
            primeFrames = juce::jmin (4096, primeFrames + 512);
            setLatencySamples (primeFrames + 64);
        }

        return;
    }

    const int wanted = juce::jmin (outScratch.getNumSamples(),
                                   (int) std::ceil ((double) numSamples * ratio) + 2);
    const int take = juce::jmin (wanted, available);

    if (take > 0)
    {
        float* tmp[m4lb::kMaxChannels] = { outScratch.getWritePointer (0), outScratch.getWritePointer (1) };
        m4lb::ringRead (rings, channels, h.outRead, tmp, take);
    }

    if (take < wanted)
    {
        for (int c = 0; c < m4lb::kMaxChannels; ++c)
            outScratch.clear (c, take, wanted - take);

        primed = false;
    }

    const float* src[m4lb::kMaxChannels] = { outScratch.getReadPointer (0), outScratch.getReadPointer (1) };
    float* dst[m4lb::kMaxChannels] = { buffer.getWritePointer (0),
                                       channels > 1 ? buffer.getWritePointer (1)
                                                    : monoScratch.getWritePointer (0) };

    const int produced = outResampler.process (src, dst, channels, wanted, numSamples, ratio);

    if (produced < numSamples)
    {
        for (int c = 0; c < channels; ++c)
            buffer.clear (c, produced, numSamples - produced);
    }
}

void BridgeAudioProcessor::pushMidi (const juce::MidiBuffer& midi)
{
    if (midi.isEmpty())
        return;

    for (const auto metadata : midi)
    {
        const auto& message = metadata.getMessage();
        const int size = juce::jlimit (1, 3, message.getRawDataSize());
        const auto* raw = message.getRawData();

        if (oscSocket != nullptr && oscPort > 0)
        {
            std::vector<uint8_t> packet;
            appendOscString (packet, "/m");
            appendOscString (packet, size == 3 ? ",iii" : (size == 2 ? ",ii" : ",i"));

            for (int i = 0; i < size; ++i)
                appendOscInt (packet, (int32_t) raw[i]);

            oscSocket->write ("127.0.0.1", oscPort, packet.data(), (int) packet.size());
        }
    }

    if (midiInCount.fetch_add ((uint32_t) midi.getNumEvents()) == 0)
    {
        const auto first = *midi.begin();
        const auto& m = first.getMessage();
        juce::String bytes;
        for (int i = 0; i < m.getRawDataSize(); ++i)
            bytes << juce::String ((int) m.getRawData()[i]) << " ";

        m4lb::log ("first host MIDI: " + bytes + " size=" + juce::String (m.getRawDataSize()));
    }

    auto* layout = transport.layout();
    auto& h = layout->header;

    for (const auto metadata : midi)
    {
        const auto& message = metadata.getMessage();
        const int size = message.getRawDataSize();

        if (size <= 0 || size > 3)
            continue;

        m4lb::MidiEvent ev;
        ev.offset = (uint32_t) juce::jmax (0, metadata.samplePosition);
        ev.size = (uint8_t) size;

        const auto* raw = message.getRawData();
        for (int i = 0; i < size; ++i)
            ev.data[i] = (uint8_t) raw[i];

        m4lb::pushMidi (layout->midiToMax, h.midiToMaxWrite, h.midiToMaxRead, ev);
    }
}

void BridgeAudioProcessor::pullMidi (juce::MidiBuffer& midi)
{
    // count what comes back from Max (device -> host)
    auto* layout = transport.layout();
    auto& h = layout->header;

    m4lb::MidiEvent ev;
    while (m4lb::popMidi (layout->midiToHost, h.midiToHostWrite, h.midiToHostRead, ev))
    {
        midiCountOut.fetch_add (1);
        midi.addEvent (ev.data, (int) ev.size, (int) ev.offset);
    }
}

//==============================================================================
juce::File BridgeAudioProcessor::writePatchFile() const
{
    const auto outBundle = m4lb::findShimBundle ("M4L Bridge Out.vst3");
    const auto inBundle  = m4lb::findShimBundle ("M4L Bridge In.vst3");

    rawLog (("shimOut=" + outBundle.getFullPathName()).toRawUTF8());
    rawLog (("shimIn=" + inBundle.getFullPathName()).toRawUTF8());

    if (outBundle.getFullPathName().isEmpty() || inBundle.getFullPathName().isEmpty())
    {
        lastError = "Shim plugins not found.\nLooked for \"M4L Bridge Out.vst3\" and \"M4L Bridge In.vst3\" in:\n  "
                    + m4lb::plugInFolder().getFullPathName()
                    + "\nThe bridge itself is loaded from:\n  " + m4lb::moduleFile().getFullPathName();
        return {};
    }

    // %APPDATA% is writable even when the host runs the plugin in a
    // restricted process; %TEMP% is only a fallback.
    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("M4LBridge");

    if (! dir.createDirectory())
        dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("M4LBridge");

    const bool created = dir.createDirectory();
    rawLog (("patchDir=" + dir.getFullPathName() + " created=" + (created ? "1" : "0")).toRawUTF8());

    const auto file = dir.getChildFile ("m4lbridge_" + juce::String (juce::Time::currentTimeMillis()) + ".maxpat");

    m4lb::PatchOptions options;
    options.shimOutPath = outBundle.getFullPathName();
    options.shimInPath = inBundle.getFullPathName();
    options.devicePath = devicePath;
    options.loopback = loopback;
    options.openDeviceUi = ! loopback;
    options.udpPort = oscPort;
    options.instanceId = 0; // revert: shared memory keyed by Max pid again

    dir.getChildFile ("ctl.js").replaceWithText (m4lb::controlScript());

    const auto json = m4lb::buildPatch (options);
    const bool wrote = file.replaceWithText (json);
    rawLog (("jsonLen=" + juce::String (json.length()) + " wrote=" + (wrote ? "1" : "0")
             + " exists=" + (file.existsAsFile() ? "1" : "0") + " path=" + file.getFullPathName()).toRawUTF8());

    return file;
}

// Max restores the previous session's windows on launch, which is why the old
// patch window reappears next to the new one.  Turn that off in its prefs.
static void disableMaxWindowRestore()
{
    for (const auto* appName : { "Max 9", "Max 8" })
    {
        auto prefs = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                         .getChildFile ("Cycling '74")
                         .getChildFile (appName)
                         .getChildFile ("Settings")
                         .getChildFile ("maxpreferences.maxpref");

        if (! prefs.existsAsFile())
            continue;

        auto text = prefs.loadFileAsString();

        if (! text.contains ("\"restorewindows\""))
            continue;

        auto updated = text.replace ("\"restorewindows\": 1", "\"restorewindows\": 0");
        updated = updated.replace ("\"restorewindows\":1", "\"restorewindows\":0");

        if (updated != text)
        {
            prefs.replaceWithText (updated);
            m4lb::log ("disabled Max window restore in " + prefs.getFullPathName());
        }
    }
}

void BridgeAudioProcessor::startMaxSide()
{
    rawLog ("StartMaxSide"); m4lb::log ("startMaxSide device=" + devicePath + " maxExe=" + maxExePath);
    stopMaxSide();

    lastError.clear();

    if (devicePath.isEmpty())
    {
        lastError = "No .amxd device selected";
        return;
    }

    if (! juce::File (devicePath).existsAsFile())
    {
        lastError = "Device file not found:\n" + devicePath
                  + "\n\nPick the .amxd again with the \"...\" button.";
        errorUnreported = true;
        m4lb::log ("device not found: " + devicePath);
        return;
    }

    disableMaxWindowRestore();

    if (shmKey == 0)
        shmKey = 1u + (uint32_t) juce::Random::getSystemRandom().nextInt (999998);

    if (oscPort == 0)
        oscPort = pickFreeUdpPort();

    if (oscSocket == nullptr && oscPort > 0)
    {
        oscSocket = std::make_unique<juce::DatagramSocket>();
        oscSocket->bindToPort (0);
    }

    // Max allows one instance per executable.  Try Max.exe, then the runtime
    // variants, until one stays alive - that is what lets several bridge
    // instances run at the same time.
    auto baseExe = m4lb::findMaxExecutable (maxExePath);
    if (! baseExe.existsAsFile())
    {
        lastError = "Max executable not found (set it in the plugin editor)";
        errorUnreported = true;
        m4lb::log ("max exe not found");
        return;
    }

    juce::StringArray candidates;
    candidates.add (baseExe.getFullPathName());

    for (const auto* name : { "Max.exe", "MaxRT.exe", "MaxRT_nocef.exe" })
    {
        const auto candidate = baseExe.getParentDirectory().getChildFile (name);
        if (candidate.existsAsFile())
            candidates.addIfNotAlreadyThere (candidate.getFullPathName());
    }

    currentPatchFile = writePatchFile();
    if (! currentPatchFile.existsAsFile())
    {
        if (lastError.isEmpty())
            lastError = "Could not write the Max patch";

        errorUnreported = true;
        m4lb::log ("patch write failed: " + lastError);
        return;
    }

    rawLog ("patch written"); m4lb::log ("patch=" + currentPatchFile.getFullPathName());

    for (const auto& candidatePath : candidates)
    {
        juce::String commandLine = "\"" + candidatePath + "\" -nosplash \""
                                 + currentPatchFile.getFullPathName() + "\"";

        auto wide = commandLine.toWideCharPointer();
        const std::wstring cmd (wide);
        std::vector<wchar_t> cmdBuffer (cmd.begin(), cmd.end());
        cmdBuffer.push_back (L'\0');

        const auto workingDir = juce::File (candidatePath).getParentDirectory().getFullPathName();
        auto wideDir = workingDir.toWideCharPointer();
        const std::wstring dirW (wideDir);

        STARTUPINFOW si {};
        si.cb = sizeof (si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_SHOWMINNOACTIVE;

        PROCESS_INFORMATION pi {};

        if (! CreateProcessW (nullptr, cmdBuffer.data(), nullptr, nullptr, FALSE, 0, nullptr,
                              dirW.empty() ? nullptr : dirW.c_str(), &si, &pi))
        {
            m4lb::log ("CreateProcess failed for " + candidatePath);
            continue;
        }

        CloseHandle (pi.hThread);

        // Max hands the patch to an already running instance and exits; give it
        // a moment and try the next executable if that happened.
        bool alive = true;

        for (int i = 0; i < 20; ++i) // up to ~4 s: Max may hand off and exit late
        {
            if (WaitForSingleObject (pi.hProcess, 200) != WAIT_TIMEOUT)
            {
                alive = false;
                break;
            }
        }

        if (alive)
        {
            maxProcessHandle = pi.hProcess;
            maxPid = pi.dwProcessId;
            rawLog ("spawned");
            m4lb::log ("spawned " + candidatePath + " pid=" + juce::String ((int) maxPid));
            primed = false;

            // Wait until a shim reports a sample rate: the instance may be
            // attached by id (multi-open) or, as a fallback, by Max pid.
            bool connected = false;

            for (int attempt = 0; attempt < 80 && ! connected; ++attempt)
            {
                const auto key = (uint32_t) maxPid;

                {
                    const juce::ScopedLock sl (transportLock);
                    transport.attach (key);
                }

                for (int k = 0; k < 5 && ! connected; ++k)
                {
                    Sleep (50);

                    const juce::ScopedLock sl (transportLock);
                    connected = transport.isAttached()
                             && transport.layout()->header.sampleRate.load() > 0;
                }
            }

            m4lb::log (connected ? "shim connected" : "shim did not connect");
            return;
        }

        CloseHandle (pi.hProcess);
        m4lb::log ("Max instance exited immediately: " + candidatePath);
    }

    lastError = "Could not start a Max instance (all variants are already running?)";
    errorUnreported = true;
}

void BridgeAudioProcessor::stopMaxSide()
{
    {
        const juce::ScopedLock sl (transportLock);
        transport.close();
    }

    if (auto* handle = static_cast<HANDLE> (maxProcessHandle))
    {
        if (WaitForSingleObject (handle, 0) == WAIT_TIMEOUT)
        {
            TerminateProcess (handle, 0);
            WaitForSingleObject (handle, 2000); // let it really die before relaunching
        }

        CloseHandle (handle);
        maxProcessHandle = nullptr;
    }

    maxPid = 0;
}

bool BridgeAudioProcessor::isMaxAlive() const
{
    if (maxProcessHandle == nullptr)
        return false;

    return WaitForSingleObject (static_cast<HANDLE> (maxProcessHandle), 0) == WAIT_TIMEOUT;
}

//==============================================================================
static float bitsToFloat (uint32_t bits)
{
    float f = 0.0f;
    std::memcpy (&f, &bits, sizeof (f));
    return f;
}

juce::String BridgeAudioProcessor::getLastError() const { return lastError; }

juce::String BridgeAudioProcessor::takeStartupWarning()
{
    if (maxPid == 0 || maxLaunchMs == 0)
        return {};

    if (getHeartbeat() != 0)
    {
        maxLaunchMs = 0;
        return {};
    }

    if (juce::Time::currentTimeMillis() - maxLaunchMs < 12000)
        return {};

    maxLaunchMs = 0;

    return "Max is running but no audio blocks arrived from the device.\\n\\n"
           "This usually means the .amxd failed to load (missing file, wrong path, "
           "or a missing external object). Check Max console (Window -> Max Console).";
}

juce::String BridgeAudioProcessor::getDiagnostics() const
{
    juce::String s;
    s << "version: " << kBridgeVersion << "\n";
    s << "module: " << m4lb::moduleFile().getFullPathName() << "\n";
    s << "max: " << m4lb::findMaxExecutable (maxExePath).getFullPathName() << "\n";
    s << "shim out: " << m4lb::findShimBundle ("M4L Bridge Out.vst3").getFullPathName() << "\n";
    s << "shim in: " << m4lb::findShimBundle ("M4L Bridge In.vst3").getFullPathName() << "\n";

    for (const auto& c : m4lb::shimCandidates ("M4L Bridge Out.vst3"))
        s << "  candidate " << c.getFullPathName() << (c.isDirectory() ? " [ok]" : " [missing]") << "\n";
    s << "device: " << devicePath << "\n";
    s << "max pid: " << (int) maxPid << "\n";
    s << "status: " << getStatusText() << "\n";
    s << "last error: " << (lastError.isEmpty() ? juce::String ("<none>") : lastError);
    return s;
}
bool BridgeAudioProcessor::hasUnreportedError() const { return errorUnreported.load(); }
void BridgeAudioProcessor::markErrorReported() { errorUnreported = false; }

juce::String BridgeAudioProcessor::getDevicePath() const { return devicePath; }

void BridgeAudioProcessor::setDevicePath (const juce::String& newPath)
{
    rawLog ("SetDevicePath"); m4lb::log ("setDevicePath " + newPath);
    devicePath = newPath;

    // Called from the editor (message thread).  Do the spawn inline: posting it
    // with callAsync is not guaranteed to be dispatched by every host's loop.
    startMaxSide();
}

juce::String BridgeAudioProcessor::getMaxExePath() const { return maxExePath; }

void BridgeAudioProcessor::setMaxExePath (const juce::String& newPath)
{
    maxExePath = newPath;
}

void BridgeAudioProcessor::restartMaxSide()
{
    startMaxSide();
}

void BridgeAudioProcessor::setDirectTone (bool shouldPlay) { directTone = shouldPlay; }
bool BridgeAudioProcessor::isDirectTone() const { return directTone.load(); }

void BridgeAudioProcessor::setTestTone (bool shouldPlay) { testTone = shouldPlay; }
bool BridgeAudioProcessor::isTestTone() const { return testTone.load(); }
void BridgeAudioProcessor::setBypassDevice (bool shouldBypass) { bypassDevice = shouldBypass; }
bool BridgeAudioProcessor::isBypassDevice() const { return bypassDevice.load(); }

void BridgeAudioProcessor::setLoopbackMode (bool shouldLoopback)
{
    loopback = shouldLoopback;
}

bool BridgeAudioProcessor::isLoopbackMode() const { return loopback; }

juce::String BridgeAudioProcessor::getStatusText() const
{
    const juce::ScopedTryLock tryLock (const_cast<juce::CriticalSection&> (transportLock));
    if (! tryLock.isLocked())
        return "busy";

    juce::String s;

    if (! transport.isAttached() || ! transport.valid())
    {
        s << "Max: not connected";
        if (lastError.isNotEmpty())
            s << " | " << lastError;
        return s;
    }

    const auto& h = transport.layout()->header;
    s << (isMaxAlive() ? "Max: running" : "Max: exited")
      << " | SR " << juce::String ((int) h.sampleRate.load())
      << " | host " << juce::String (hostSampleRate, 0)
      << " | xrun " << juce::String ((int) h.underruns.load())
      << "/" << juce::String ((int) h.overruns.load())
      << " | in " << juce::String (juce::Decibels::gainToDecibels (bitsToFloat (h.meterHostIn.load())), 1)
      << "dB out " << juce::String (juce::Decibels::gainToDecibels (bitsToFloat (h.meterHostOut.load())), 1)
      << "dB | hb " << juce::String ((int) h.heartbeat.load())
      << " | pid " << juce::String ((int) maxPid);

    return s;
}

double BridgeAudioProcessor::getMaxSampleRate() const
{
    if (transport.isAttached() && transport.valid())
        return (double) transport.layout()->header.sampleRate.load();

    return 0.0;
}

uint32_t BridgeAudioProcessor::getUnderruns() const
{
    return transport.isAttached() && transport.valid()
               ? transport.layout()->header.underruns.load() : 0u;
}

uint32_t BridgeAudioProcessor::getOverruns() const
{
    return transport.isAttached() && transport.valid()
               ? transport.layout()->header.overruns.load() : 0u;
}

uint32_t BridgeAudioProcessor::getHeartbeat() const
{
    return transport.isAttached() && transport.valid()
               ? transport.layout()->header.heartbeat.load() : 0u;
}

//==============================================================================
void BridgeAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto xml = std::make_unique<juce::XmlElement> ("M4LBridge");
    xml->setAttribute ("device", devicePath);
    xml->setAttribute ("maxExe", maxExePath);
    xml->setAttribute ("loopback", loopback ? 1 : 0);

    copyXmlToBinary (*xml, destData);
}

void BridgeAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    rawLog ("SetState entry");
    auto xml = getXmlFromBinary (data, sizeInBytes);
    rawLog (xml == nullptr ? "SetState xml null" : "SetState xml ok");
    if (xml == nullptr)
        return;

    rawLog (("SetState device=" + xml->getStringAttribute ("device")).toRawUTF8());
    const auto storedDevice = xml->getStringAttribute ("device");

    // Never let an empty/partial state wipe a device the user just picked.
    if (storedDevice.isNotEmpty())
        devicePath = storedDevice;
    maxExePath = xml->getStringAttribute ("maxExe");
    loopback = xml->getIntAttribute ("loopback", 0) != 0;
}

//==============================================================================
juce::AudioProcessorEditor* BridgeAudioProcessor::createEditor()
{
    return new BridgeAudioProcessorEditor (*this);
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new BridgeAudioProcessor();
}

