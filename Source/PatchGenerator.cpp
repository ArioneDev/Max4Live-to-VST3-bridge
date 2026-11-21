#include "PatchGenerator.h"

namespace m4lb
{

namespace
{

juce::String maxSymbol (const juce::String& path)
{
    // Max object boxes take a quoted symbol; use forward slashes so Max does
    // not treat backslashes as escapes.
    juce::String p = path.replaceCharacter ('\\', '/');
    p = p.replaceCharacter ('"', '\'');
    return "\"" + p + "\"";
}

juce::var makeBox (const juce::String& id,
                   const juce::String& maxclass,
                   const juce::String& text,
                   int numInlets,
                   int numOutlets,
                   const juce::StringArray& outletTypes,
                   double x, double y, double w, double h)
{
    auto* obj = new juce::DynamicObject();

    obj->setProperty ("id", id);
    obj->setProperty ("maxclass", maxclass);
    obj->setProperty ("text", text);
    obj->setProperty ("numinlets", numInlets);
    obj->setProperty ("numoutlets", numOutlets);

    juce::Array<juce::var> types;
    for (const auto& t : outletTypes)
        types.add (t);

    obj->setProperty ("outlettype", types);
    obj->setProperty ("patching_rect", juce::Array<juce::var> { x, y, w, h });

    return juce::var (obj);
}

juce::var makeLine (const juce::String& srcId, int srcOutlet,
                    const juce::String& dstId, int dstInlet)
{
    juce::Array<juce::var> source;
    source.add (srcId);
    source.add (srcOutlet);

    juce::Array<juce::var> destination;
    destination.add (dstId);
    destination.add (dstInlet);

    auto* obj = new juce::DynamicObject();
    obj->setProperty ("source", juce::var (source));
    obj->setProperty ("destination", juce::var (destination));

    auto* line = new juce::DynamicObject();
    line->setProperty ("patchline", juce::var (obj));
    return juce::var (line);
}

} // namespace

juce::String controlScript()
{
    // Receives OSC-style messages from the bridge over UDP and turns them into
    // amxd~ messages.  Also logs what arrived, which makes field debugging
    // possible without a console.
    return "autowatch = 1;\n"
           "inlets = 1;\n"
           "outlets = 2;\n"
           "var logDir = jsarguments.length > 1 ? jsarguments[1] : \"\";\n"
           "var tag = jsarguments.length > 2 ? jsarguments[2] : \"ctl\";\n"
           "var n = 0;\n"
           "function log(s) {\n"
           "  if (!logDir) return;\n"
           "  var f = new File(logDir + \"/\" + tag + \"_\" + (n++) + \".txt\", \"write\");\n"
           "  f.open(); f.writeline(s); f.close();\n"
           "}\n"
           "function anything() {\n"
           "  var name = messagename;\n"
           "  if (name.charAt(0) == \"/\") name = name.substring(1);\n"
           "  var a = arrayfromargs(arguments);\n"
           "  log(name + \" \" + a.join(\" \"));\n"
           "  if (name == \"m\" && a.length >= 2) {\n"
           "    var b2 = a.length > 2 ? a[2] : 0;\n"
           "    outlet(0, [\"midievent\", a[0], a[1], b2]);\n"
           "  } else if (name == \"p\" && a.length >= 2) {\n"
           "    outlet(0, [a[0], a[1]]);\n"
           "  } else if (name == \"t\" && a.length >= 1) {\n"
           "    outlet(1, [\"tempo\", a[0]]);\n"
           "  }\n"
           "}\n";
}

juce::String buildPatch (const PatchOptions& options)
{
    juce::Array<juce::var> boxes;
    juce::Array<juce::var> lines;

    const juce::String outBox ("obj-10");
    const juce::String devBox ("obj-20");
    const juce::String inBox  ("obj-30");

    // [vst~ "<shim out>" 2 2] : produces the host's input audio as a signal.
    // vst~ exposes 9 outlets for a 2-in/2-out plug-in; the raw MIDI from the
    // plug-in comes out the fourth-from-right one (index 5).
    boxes.add (makeBox (outBox, "newobj",
                        "vst~ " + maxSymbol (options.shimOutPath) + " 2 2",
                        1, 9, { "signal", "signal", "", "", "", "", "", "", "" }, 20, 20, 360, 22));

    if (! options.loopback)
    {

        // [amxd~ "<device>"] : the actual Max for Live device.
        boxes.add (makeBox (devBox, "newobj",
                            "amxd~ " + maxSymbol (options.devicePath),
                            3, 4, { "signal", "signal", "", "" }, 20, 80, 360, 22));
    }

    // [vst~ "<shim in>" 2 2] : captures the processed signal into the ring.
    boxes.add (makeBox (inBox, "newobj",
                        "vst~ " + maxSymbol (options.shimInPath) + " 2 2",
                        3, 3, { "signal", "signal", "" }, 20, 140, 360, 22));

    // Silent sink so the DSP graph is actually pulled by dac~.
    // [*~] has a single outlet, so use one per channel.
    boxes.add (makeBox ("obj-40", "newobj", "*~ 0.", 2, 1, { "signal" }, 20, 200, 80, 22));
    boxes.add (makeBox ("obj-42", "newobj", "*~ 0.", 2, 1, { "signal" }, 120, 200, 80, 22));
    boxes.add (makeBox ("obj-41", "newobj", "dac~ 1 2", 2, 0, {}, 20, 240, 80, 22));

    // Loading an amxd~ turns global audio processing off in Max, so audio has
    // to be explicitly restarted once the device is in place.  The device
    // window is opened a little later, when its patcher is fully loaded.
    boxes.add (makeBox ("obj-50", "newobj", "loadbang", 1, 1, { "bang" }, 420, 20, 80, 22));
    boxes.add (makeBox ("obj-51", "newobj", "delay 2500", 1, 1, { "bang" }, 420, 60, 80, 22));
    boxes.add (makeBox ("obj-52", "message", "start", 2, 1, { "" }, 420, 100, 80, 22));
    boxes.add (makeBox ("obj-54", "newobj", "delay 3500", 1, 1, { "bang" }, 520, 60, 80, 22));
    boxes.add (makeBox ("obj-55", "message", "open", 2, 1, { "" }, 520, 100, 80, 22));

    juce::String idBox, idBox2, idFan;

    if (options.instanceId > 0)
    {
        const auto idText = "bridgeid " + juce::String ((double) options.instanceId / 999999.0, 6);

        boxes.add (makeBox ("obj-63", "newobj", "t b b", 1, 2, { "bang", "bang" }, 700, 40, 60, 22));
        boxes.add (makeBox ("obj-64", "message", idText, 2, 1, { "" }, 700, 80, 120, 22));
        boxes.add (makeBox ("obj-65", "message", idText, 2, 1, { "" }, 700, 120, 120, 22));
        idBox = "obj-64";
        idBox2 = "obj-65";
        idFan = "obj-63";
    }

    // Audio wiring.
    if (options.loopback)
    {
        lines.add (makeLine (outBox, 0, inBox, 0));
        lines.add (makeLine (outBox, 1, inBox, 1));
    }
    else
    {
        lines.add (makeLine (outBox, 0, devBox, 0));
        lines.add (makeLine (outBox, 1, devBox, 1));
        // Host MIDI -> shimOut -> raw MIDI outlet -> one byte per message
        // (amxd~ expects the byte stream that [midiin] would produce).
        // MIDI / control arrives over OSC (UDP).  [udpreceive] decodes the
        // packet into a Max message, ctl.js turns it into amxd~ messages.
        if (options.udpPort > 0)
        {
            const auto logDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                    .getChildFile ("M4LBridge").getFullPathName();

            boxes.add (makeBox ("obj-62", "newobj", "udpreceive " + juce::String (options.udpPort),
                                1, 1, { "" }, 420, 40, 140, 22));
            boxes.add (makeBox ("obj-61", "newobj",
                                "js ctl.js " + maxSymbol (logDir) + " " + juce::String (options.udpPort),
                                1, 2, { "", "" }, 420, 80, 220, 22));

            // Setting the global transport tempo is done through the
            // [transport] object's "tempo" attribute.
            boxes.add (makeBox ("obj-66", "newobj", "transport", 2, 9,
                                { "", "", "", "", "", "", "", "", "" }, 660, 80, 80, 22));

            lines.add (makeLine ("obj-62", 0, "obj-61", 0));
            lines.add (makeLine ("obj-61", 0, devBox, 0));  // midievent / param messages
            lines.add (makeLine ("obj-61", 1, "obj-66", 0)); // tempo
        }
        lines.add (makeLine (devBox, 0, inBox, 0));
        lines.add (makeLine (devBox, 1, inBox, 1));
    }

    lines.add (makeLine (inBox, 0, "obj-40", 0));
    lines.add (makeLine (inBox, 1, "obj-42", 0));
    lines.add (makeLine ("obj-40", 0, "obj-41", 0));
    lines.add (makeLine ("obj-42", 0, "obj-41", 1));

    // UI opening.
    lines.add (makeLine ("obj-50", 0, "obj-51", 0));
    lines.add (makeLine ("obj-51", 0, "obj-52", 0));
    lines.add (makeLine ("obj-52", 0, "obj-41", 0)); // start audio
    lines.add (makeLine ("obj-50", 0, "obj-54", 0));
    lines.add (makeLine ("obj-54", 0, "obj-55", 0));

    if (! options.loopback && options.openDeviceUi)
        lines.add (makeLine ("obj-55", 0, devBox, 0));

    if (idFan.isNotEmpty())
    {
        lines.add (makeLine ("obj-50", 0, idFan, 0));
        lines.add (makeLine (idFan, 0, idBox, 0));
        lines.add (makeLine (idFan, 1, idBox2, 0));
        lines.add (makeLine (idBox, 0, outBox, 0));
        lines.add (makeLine (idBox2, 0, inBox, 0));
    }

    if (options.midiProbe)
    {
        boxes.add (makeBox ("obj-70", "newobj", "js midilog.js", 1, 0, {}, 700, 20, 120, 22));

        for (int i = 2; i <= 8; ++i)
        {
            const auto id = "obj-7" + juce::String (i);
            boxes.add (makeBox (id, "newobj", "prepend o" + juce::String (i), 1, 1, { "" }, 560, 40 + i * 26, 120, 22));
            lines.add (makeLine (outBox, i, id, 0));
            lines.add (makeLine (id, 0, "obj-70", 0));
        }
    }

    auto* patcher = new juce::DynamicObject();
    patcher->setProperty ("fileversion", 1);

    auto* appversion = new juce::DynamicObject();
    appversion->setProperty ("major", 9);
    appversion->setProperty ("minor", 2);
    appversion->setProperty ("revision", 0);
    appversion->setProperty ("architecture", "x64");
    patcher->setProperty ("appversion", juce::var (appversion));

    patcher->setProperty ("rect", juce::Array<juce::var> { 0.0, 0.0, 640.0, 480.0 });
    patcher->setProperty ("gridsize", juce::Array<juce::var> { 15.0, 15.0 });
    patcher->setProperty ("boxes", juce::var (boxes));
    patcher->setProperty ("lines", juce::var (lines));

    auto* root = new juce::DynamicObject();
    root->setProperty ("patcher", juce::var (patcher));

    return juce::JSON::toString (juce::var (root), false);
}

} // namespace m4lb
