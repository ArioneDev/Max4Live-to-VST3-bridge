// Minimal VST3 host used to test M4L Bridge outside Max/FL.
//
// It loads the plugin through JUCE's VST3 hosting code (the same role a DAW
// plays), injects the device path through the plugin state, runs a sine
// through it for a few seconds and reports the output level.

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include <cstdio>
#include <fstream>

int main (int argc, char** argv)
{
    if (argc < 3)
    {
        std::printf ("usage: M4LHostTest <plugin.vst3> <device.amxd> [seconds]\n");
        return 2;
    }

    const juce::String pluginPath (argv[1]);
    const juce::String devicePath (argv[2]);
    const int seconds = argc > 3 ? std::atoi (argv[3]) : 8;

    {
        std::ofstream probe ("C:/Temp/m4lprobe/host_write.txt", std::ios::app);
        probe << "host started\n";
    }

    juce::ScopedJuceInitialiser_GUI juceInit;

    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> descriptions;

    format.findAllTypesForFile (descriptions, pluginPath);

    if (descriptions.isEmpty())
    {
        std::printf ("FAIL: no VST3 types found in %s\n", pluginPath.toRawUTF8());
        return 1;
    }

    juce::String error;
    std::unique_ptr<juce::AudioPluginInstance> plugin (
        format.createInstanceFromDescription (*descriptions[0], 44100.0, 512, error));

    if (plugin == nullptr)
    {
        std::printf ("FAIL: %s\n", error.toRawUTF8());
        return 1;
    }

    std::printf ("loaded: %s\n", plugin->getName().toRawUTF8());
    std::printf ("file: %s\n", plugin->getPluginDescription().fileOrIdentifier.toRawUTF8());
    std::printf ("temp: %s\n", juce::File::getSpecialLocation (juce::File::tempDirectory).getFullPathName().toRawUTF8());

    // Same XML the plugin writes in getStateInformation().
    juce::XmlElement state ("M4LBridge");
    state.setAttribute ("device", devicePath);
    state.setAttribute ("maxExe", "");
    state.setAttribute ("loopback", 0);

    {
        juce::MemoryBlock block;
        juce::AudioProcessor::copyXmlToBinary (state, block);
        plugin->setStateInformation (block.getData(), (int) block.getSize());
    }

    plugin->prepareToPlay (44100.0, 512);

    {
        juce::MemoryBlock check;
        plugin->getStateInformation (check);
        auto xml = juce::AudioProcessor::getXmlFromBinary (check.getData(), (int) check.getSize());
        std::printf ("state device: %s\n", xml != nullptr ? xml->getStringAttribute ("device").toRawUTF8() : "<none>");
    }

    juce::AudioBuffer<float> buffer (2, 512);
    juce::MidiBuffer midi;

    double phase = 0.0;
    const double inc = 2.0 * 3.14159265358979 * 440.0 / 44100.0;

    double outSum = 0.0;
    double inSum = 0.0;
    int blocks = 0;

    const auto start = juce::Time::getMillisecondCounter();

    while (juce::Time::getMillisecondCounter() - start < (juce::uint32) seconds * 1000)
    {
        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            const auto s = (float) std::sin (phase) * 0.25f;
            phase += inc;
            buffer.setSample (0, i, s);
            buffer.setSample (1, i, s);
            inSum += std::abs (s);
        }

        plugin->processBlock (buffer, midi);

        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                outSum += std::abs (buffer.getSample (c, i));

        ++blocks;
        juce::Thread::sleep (10);
    }

    std::printf ("blocks=%d inAvg=%f outAvg=%f\n", blocks,
                 inSum / juce::jmax (1, blocks * 512 * 2),
                 outSum / juce::jmax (1, blocks * 512 * 2));

    plugin->releaseResources();
    plugin.reset();

    std::printf (outSum > 0.0 ? "SIGNAL PRESENT\n" : "SILENT\n");
    return outSum > 0.0 ? 0 : 1;
}
