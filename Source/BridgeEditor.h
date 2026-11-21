#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "BridgeLog.h"
#include "BridgeProcessor.h"

//==============================================================================
// Small dark look-and-feel: rounded, flat, modern.
class BridgeLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    BridgeLookAndFeel()
    {
        setColour (juce::ResizableWindow::backgroundColourId, juce::Colour (0xfff7f7f8));
        setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xffffffff));
        setColour (juce::TextEditor::outlineColourId, juce::Colour (0xffd9dce1));
        setColour (juce::TextEditor::focusedOutlineColourId, juce::Colour (0xff111111));
        setColour (juce::TextEditor::textColourId, juce::Colour (0xff111111));
        setColour (juce::TextEditor::highlightColourId, juce::Colour (0x22111111));
        setColour (juce::CaretComponent::caretColourId, juce::Colour (0xff111111));
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& b,
                               const juce::Colour&, bool over, bool down) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        auto base = b.getToggleState() ? juce::Colour (0xff111111) : juce::Colour (0xfff3f4f6);

        if (down)       base = base.contrasting (0.08f);
        else if (over)  base = base.contrasting (0.04f);

        g.setColour (base);
        g.fillRoundedRectangle (r, 7.0f);
        g.setColour (b.getToggleState() ? juce::Colour (0xff111111) : juce::Colour (0xffd9dce1));
        g.drawRoundedRectangle (r, 7.0f, 1.0f);
    }

    juce::Font getTextButtonFont (juce::TextButton&, int height) override
    {
        return juce::FontOptions (juce::jmin (16.0f, (float) height * 0.60f), juce::Font::plain);
    }

    void drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool) override
    {
        g.setColour (b.getToggleState() ? juce::Colours::white : juce::Colour (0xff111111));
        g.setFont (getTextButtonFont (b, b.getHeight()));
        g.drawFittedText (b.getButtonText(), b.getLocalBounds().reduced (6, 0),
                          juce::Justification::centred, 1);
    }
};

//==============================================================================
class BridgeAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                         private juce::Timer
{
public:
    explicit BridgeAudioProcessorEditor (BridgeAudioProcessor& p)
        : juce::AudioProcessorEditor (&p), processor (p)
    {
        setLookAndFeel (&lnf);
        setSize (700, 470);
        setResizable (true, true);
        setResizeLimits (560, 400, 1280, 900);

        addAndMakeVisible (deviceEditor);
        deviceEditor.setText (processor.getDevicePath(), juce::dontSendNotification);
        deviceEditor.setFont (juce::FontOptions (15.0f));
        deviceEditor.setJustification (juce::Justification::centredLeft);
        deviceEditor.onReturnKey = [this] { applyDevicePath(); };
        deviceEditor.onFocusLost = [this] { applyDevicePath(); };

        addAndMakeVisible (browseDevice);
        browseDevice.setButtonText ("Browse");
        browseDevice.onClick = [this] { chooseDevice(); };

        addAndMakeVisible (maxEditor);
        maxEditor.setText (processor.getMaxExePath(), juce::dontSendNotification);
        maxEditor.setFont (juce::FontOptions (15.0f));
        maxEditor.setJustification (juce::Justification::centredLeft);
        maxEditor.onReturnKey = [this] { applyMaxPath(); };
        maxEditor.onFocusLost = [this] { applyMaxPath(); };

        addAndMakeVisible (browseMax);
        browseMax.setButtonText ("Locate");
        browseMax.onClick = [this] { chooseMaxExe(); };

        addAndMakeVisible (restartButton);
        restartButton.setButtonText ("Restart engine");
        restartButton.onClick = [this] { processor.restartMaxSide(); };

        addAndMakeVisible (testToneButton);
        testToneButton.setButtonText ("Test tone");
        testToneButton.setClickingTogglesState (true);
        testToneButton.setToggleState (processor.isTestTone(), juce::dontSendNotification);
        testToneButton.onClick = [this] { processor.setTestTone (testToneButton.getToggleState()); };

        addAndMakeVisible (directToneButton);
        directToneButton.setButtonText ("Tone to host");
        directToneButton.setClickingTogglesState (true);
        directToneButton.onClick = [this] { processor.setDirectTone (directToneButton.getToggleState()); };

        addAndMakeVisible (bypassButton);
        bypassButton.setButtonText ("Bypass");
        bypassButton.setClickingTogglesState (true);
        bypassButton.setToggleState (processor.isBypassDevice(), juce::dontSendNotification);
        bypassButton.onClick = [this] { processor.setBypassDevice (bypassButton.getToggleState()); };

        addAndMakeVisible (loopbackButton);
        loopbackButton.setButtonText ("Loopback test");
        loopbackButton.setClickingTogglesState (true);
        loopbackButton.setToggleState (processor.isLoopbackMode(), juce::dontSendNotification);
        loopbackButton.onClick = [this]
        {
            processor.setLoopbackMode (loopbackButton.getToggleState());
            processor.restartMaxSide();
        };

        addAndMakeVisible (diagnoseButton);
        diagnoseButton.setButtonText ("Diagnose");
        diagnoseButton.onClick = [this]
        {
            const auto text = processor.getDiagnostics();
            m4lb::log ("diagnostics:\n" + text);
            juce::SystemClipboard::copyTextToClipboard (text);
            juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon,
                                                         "M4L Bridge diagnostics", text);
        };

        addAndMakeVisible (versionLink);
        versionLink.setButtonText ("v" + juce::String (kBridgeVersion));
        versionLink.setURL (juce::URL ("https://github.com/ArioneDev/Max4Live-to-VST3-bridge"));
        versionLink.setFont (juce::FontOptions (12.5f), false, juce::Justification::centredRight);
        versionLink.setColour (juce::HyperlinkButton::textColourId, juce::Colour (0xff6b7280));

        startTimerHz (30);
    }

    ~BridgeAudioProcessorEditor() override
    {
        stopTimer();
        setLookAndFeel (nullptr);
    }

    //==========================================================================
    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xfff7f7f8));

        auto area = getLocalBounds().reduced (18);

        // ---- header -------------------------------------------------------
        auto header = area.removeFromTop (54);

        g.setColour (juce::Colour (0xff111111));
        g.setFont (juce::FontOptions (32.0f, juce::Font::bold));
        g.drawText ("M4L BRIDGE", header.removeFromLeft (280), juce::Justification::centredLeft);

        drawStatusPill (g, header.removeFromRight (200).reduced (0, 12));

        area.removeFromTop (6);

        // ---- meters -------------------------------------------------------
        auto meters = area.removeFromTop (58);
        drawMeter (g, meters.removeFromTop (26), "IN", inDb, juce::Colour (0xff111111));
        meters.removeFromTop (6);
        drawMeter (g, meters.removeFromTop (26), "OUT", outDb, juce::Colour (0xff111111));

        area.removeFromTop (12);

        // ---- device card --------------------------------------------------
        drawCard (g, deviceCard = area.removeFromTop (78), "DEVICE  .amxd");
        area.removeFromTop (10);

        // ---- engine card --------------------------------------------------
        drawCard (g, engineCard = area.removeFromTop (78), "MAX RUNTIME");
        area.removeFromTop (12);

        // ---- footer -------------------------------------------------------
        g.setColour (juce::Colour (0xff8a8f98));
        g.setFont (juce::FontOptions (12.5f));
        g.drawText (latencyLine, area.removeFromBottom (18), juce::Justification::centredLeft);

        auto bottom = area.removeFromBottom (20);
        g.drawText ("Drag the device window into your layout - all audio goes through the bridge.",
                    bottom.withTrimmedRight (110), juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (18);
        area.removeFromTop (54 + 6 + 58 + 12); // header + meters

        auto deviceRow = area.removeFromTop (78).reduced (14, 26);
        browseDevice.setBounds (deviceRow.removeFromRight (88).reduced (0, 2));
        deviceRow.removeFromRight (8);
        deviceEditor.setBounds (deviceRow.reduced (0, 2));

        area.removeFromTop (10);

        auto engineRow = area.removeFromTop (78).reduced (14, 26);
        restartButton.setBounds (engineRow.removeFromRight (118).reduced (0, 2));
        engineRow.removeFromRight (6);
        browseMax.setBounds (engineRow.removeFromRight (78).reduced (0, 2));
        engineRow.removeFromRight (6);
        maxEditor.setBounds (engineRow.reduced (0, 2));

        area.removeFromTop (12);

        auto buttons = area.removeFromTop (30);
        testToneButton.setBounds (buttons.removeFromLeft (104));
        buttons.removeFromLeft (8);
        bypassButton.setBounds (buttons.removeFromLeft (92));
        buttons.removeFromLeft (8);
        directToneButton.setBounds (buttons.removeFromLeft (110));
        buttons.removeFromLeft (8);
        loopbackButton.setBounds (buttons.removeFromLeft (120));
        buttons.removeFromLeft (8);
        diagnoseButton.setBounds (buttons.removeFromLeft (96));

        auto footer = getLocalBounds().reduced (18).removeFromBottom (20);
        versionLink.setBounds (footer.removeFromRight (100));
    }

private:
    //==========================================================================
    void drawCard (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& title)
    {
        g.setColour (juce::Colour (0xffffffff));
        g.fillRoundedRectangle (r.toFloat(), 10.0f);
        g.setColour (juce::Colour (0xffe5e7eb));
        g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 10.0f, 1.0f);

        g.setColour (juce::Colour (0xff8a8f98));
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText (title, r.reduced (14, 8).removeFromTop (14), juce::Justification::topLeft);
    }

    void drawStatusPill (juce::Graphics& g, juce::Rectangle<int> r)
    {
        const bool running = processor.getHeartbeat() > 0;
        const bool engine = processor.isEngineRunning();
        const bool failed = statusKey.containsIgnoreCase ("not found")
                         || statusKey.containsIgnoreCase ("failed")
                         || statusKey.containsIgnoreCase ("No .amxd");

        juce::Colour c = failed ? juce::Colour (0xffe05a5a)
                               : (running ? juce::Colour (0xff35c48a)
                                          : (engine ? juce::Colour (0xffe0a84a)
                                                    : juce::Colour (0xff6b7280)));

        auto pill = r.toFloat();
        g.setColour (c.withAlpha (0.10f));
        g.fillRoundedRectangle (pill, pill.getHeight() * 0.5f);
        g.setColour (c.withAlpha (0.45f));
        g.drawRoundedRectangle (pill.reduced (0.5f), pill.getHeight() * 0.5f, 1.0f);

        g.setColour (c);
        g.fillEllipse (pill.getX() + 14.0f, pill.getCentreY() - 3.5f, 7.0f, 7.0f);

        g.setColour (juce::Colour (0xff111111));
        g.setFont (juce::FontOptions (11.5f, juce::Font::bold));
        g.drawText (running ? "RUNNING" : (engine ? "STARTING" : "OFFLINE"),
                    pill.withTrimmedLeft (28.0f), juce::Justification::centredLeft);
    }

    void drawMeter (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& label,
                    float db, juce::Colour colour)
    {
        auto labelArea = r.removeFromLeft (46);
        g.setColour (juce::Colour (0xff8a8f98));
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText (label, labelArea, juce::Justification::centredLeft);

        auto valueArea = r.removeFromRight (62);
        g.setColour (juce::Colour (0xff6b7280));
        g.setFont (juce::FontOptions (12.5f));
        g.drawText (db <= -99.0f ? juce::String ("-inf dB")
                                 : juce::String (db, 1) + " dB",
                    valueArea, juce::Justification::centredRight);

        auto track = r.reduced (0, 6).toFloat();
        g.setColour (juce::Colour (0xffeceef1));
        g.fillRoundedRectangle (track, 4.0f);

        const float norm = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);

        if (norm > 0.001f)
        {
            auto fill = track.withWidth (track.getWidth() * norm);
            juce::ColourGradient grad (colour.darker (0.25f), fill.getX(), 0.0f,
                                       colour.brighter (0.15f), fill.getRight(), 0.0f, false);
            g.setGradientFill (grad);
            g.fillRoundedRectangle (fill, 4.0f);
        }

        g.setColour (juce::Colour (0xffdfe2e6));
        for (int i = 1; i < 6; ++i)
        {
            const auto x = track.getX() + track.getWidth() * (float) i / 6.0f;
            g.drawVerticalLine ((int) x, track.getY() + 2.0f, track.getBottom() - 2.0f);
        }
    }

    //==========================================================================
    void timerCallback() override
    {
        if (const auto warning = processor.takeStartupWarning(); warning.isNotEmpty())
        {
            m4lb::log ("startup warning: " + warning);
            juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                         "M4L Bridge", warning);
        }

        if (processor.hasUnreportedError())
        {
            processor.markErrorReported();
            const auto message = processor.getLastError();
            m4lb::log ("showing error: " + message);
            juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                         "M4L Bridge", message);
        }

        inDb = processor.getInputDb();
        outDb = processor.getOutputDb();

        const auto text = processor.getStatusText();
        statusKey = text.upToFirstOccurrenceOf (" | ", false, false);

        const auto latencyMs = 1000.0 * (double) processor.getLatencyFrames()
                                        / juce::jmax (1.0, processor.getHostSampleRate());

        latencyLine = "Engine buffer " + juce::String (processor.getLatencyFrames())
                    + " frames  ·  ~" + juce::String (latencyMs, 1) + " ms  ·  "
                    + (processor.isEngineRunning() ? "Max connected" : "Max offline");

        const auto now = juce::Time::getMillisecondCounter();

        if (statusKey != lastLoggedState || now - lastStatusLogMs > 5000)
        {
            lastLoggedState = statusKey;
            lastStatusLogMs = now;
            m4lb::log ("status: " + text);
        }

        repaint();
    }

    void applyDevicePath()
    {
        const auto text = deviceEditor.getText().trim();
        m4lb::log ("applyDevicePath text=" + text);

        if (text.isEmpty() && processor.getDevicePath().isNotEmpty())
            return;

        if (text != processor.getDevicePath())
            processor.setDevicePath (text);
    }

    void applyMaxPath()
    {
        processor.setMaxExePath (maxEditor.getText().trim());
    }

    void chooseDevice()
    {
        m4lb::log ("chooseDevice clicked");
        chooser = std::make_unique<juce::FileChooser> ("Select a Max for Live device",
                                                       juce::File (processor.getDevicePath()),
                                                       "*.amxd");

        chooser->launchAsync (juce::FileBrowserComponent::openMode
                                  | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc)
                              {
                                  const auto file = fc.getResult();
                                  m4lb::log ("chooser result=" + file.getFullPathName()
                                             + " exists=" + (file.existsAsFile() ? "1" : "0"));
                                  if (file.existsAsFile())
                                  {
                                      deviceEditor.setText (file.getFullPathName(), juce::dontSendNotification);
                                      processor.setDevicePath (file.getFullPathName());
                                  }
                              });
    }

    void chooseMaxExe()
    {
        chooser = std::make_unique<juce::FileChooser> ("Select Max.exe",
                                                       juce::File (processor.getMaxExePath()),
                                                       "*.exe");

        chooser->launchAsync (juce::FileBrowserComponent::openMode
                                  | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc)
                              {
                                  const auto file = fc.getResult();
                                  if (file.existsAsFile())
                                  {
                                      maxEditor.setText (file.getFullPathName(), juce::dontSendNotification);
                                      processor.setMaxExePath (file.getFullPathName());
                                  }
                              });
    }

    BridgeAudioProcessor& processor;
    BridgeLookAndFeel lnf;

    juce::TextEditor deviceEditor, maxEditor;
    juce::TextButton browseDevice, browseMax, restartButton, diagnoseButton;
    juce::TextButton testToneButton, bypassButton, loopbackButton, directToneButton;

    juce::Rectangle<int> deviceCard, engineCard;
    juce::HyperlinkButton versionLink;

    std::unique_ptr<juce::FileChooser> chooser;

    float inDb = -100.0f, outDb = -100.0f;
    juce::String statusKey, lastLoggedState, latencyLine;
    juce::int64 lastStatusLogMs = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BridgeAudioProcessorEditor)
};
