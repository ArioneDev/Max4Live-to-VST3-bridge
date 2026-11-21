# M4L Bridge (prototype)

Play **Ableton Max for Live devices (`.amxd`)** inside other VST3 hosts (FL Studio,
Cubase, Reaper, Bitwig, …) on Windows.

**Status: working prototype (Windows 11, Max 9.2).** Effect and instrument builds
are verified in FL Studio: audio runs through `amxd~` over shared memory, while
MIDI and tempo travel over a local UDP/OSC control channel. Device parameters are
not exposed to the host yet, and only one bridge instance can run at a time
(Max is single-instance per executable).

The bridge does **not** reimplement Max for Live. It drives the Max runtime that
you already have installed: the VST3 spawns a hidden `Max.exe`, which hosts the
device through Max's built-in `amxd~` object, and audio travels over a
shared-memory ring between the two processes.

```
┌────────────────────┐   shared memory (audio)   ┌──────────────────────────┐
│ host (FL Studio)   │ ◄───────────────────────► │ Max.exe (spawned)        │
│  M4L Bridge.vst3   │                           │  generated .maxpat:      │
│                    │   UDP / OSC (MIDI, tempo) │  [vst~ Out] → [amxd~] →  │
│                    │ ────────────────────────► │  [vst~ In] → [*~ 0] →    │
│                    │      [udpreceive] → js    │  [dac~]                  │
└────────────────────┘                           └──────────────────────────┘
```

## What works / what doesn't

| | |
|---|---|
| ✅ Self-contained devices (DSP + `live.dial`/`live.toggle` UI + MIDI) | yes |
| ✅ Device UI opens in its own window | yes (P0) |
| ⚠️ Devices that need the **Live API** (`live.object`, `live.path`, `live.observer`, `LiveAPI` in js) | no — the Live API does not exist outside Live |
| ⚠️ Devices that rely on Live's transport / clip engine | partial, they run on Max's own clock |
| ❌ `.adv` files | impossible — those are Live's *native device presets*, not Max patches |
| ⚠️ Devices shipping third-party externals (`.mxe64`) | the same externals must be installed for Max |
| ⚠️ Protected/encrypted devices | depends on what they depend on |

Known limitations:

* **one instance only** — Max accepts a single instance per executable, so a
  second bridge instance cannot get its own Max process. Multi-instance in one
  Max process needs a per-instance shared-memory key; that handshake is not
  working reliably yet and was rolled back.
* device parameters are not exposed to the host yet (no automation lanes); use
  the device UI window.
* **tempo** is forwarded to Max's global transport, so devices that read tempo
  from the Max transport follow the host. Devices that read `live_set tempo`
  through the Live API cannot (there is no Live).
* Max needs a working audio output device; it is only used as the DSP clock
  (the patch outputs silence). Clock drift between host and Max is not
  compensated beyond the jitter buffer.
* if Max dies the plugin stays silent until "Restart engine" is pressed.

## Requirements

* Windows 10/11 x64
* **Max 8 or 9** installed (the Max editor that ships with Live Suite works).
  `MaxRT.exe` works too, but the full editor is recommended.
* A VST3 host.
* To build: Visual Studio 2022/2026 + CMake ≥ 3.22 + JUCE 8 source.

## Build

```bat
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 ^
      -DJUCE_SOURCE_DIR="C:/path/to/JUCE"
cmake --build build --config Release
```

Without a local JUCE checkout, replace the `add_subdirectory` call in
`CMakeLists.txt` with `FetchContent` (JUCE 8.0.9 is known to work):

```cmake
include(FetchContent)
FetchContent_Declare(JUCE
    URL https://github.com/juce-framework/JUCE/archive/refs/tags/8.0.9.zip)
FetchContent_MakeAvailable(JUCE)
```

Artefacts:

```
build/M4LBridge_artefacts/Release/VST3/M4L Bridge.vst3
build/M4LBridgeInstrument_artefacts/Release/VST3/M4L Bridge Instrument.vst3
build/M4LBridgeOut_artefacts/Release/VST3/M4L Bridge Out.vst3
build/M4LBridgeIn_artefacts/Release/VST3/M4L Bridge In.vst3
build/Release/M4LBTest.exe          # transport test
build/Release/M4LBPatchGen.exe      # prints the generated .maxpat
```

Copy all four bundles into the **same** VST3 folder. The bridge finds the two
shims next to itself.

> Build in an ASCII-only path. JUCE's resource compiler chokes on non-ASCII
> build paths (e.g. `D:\文档\...`); source paths are fine.

## Usage

* **Effects**: load **M4L Bridge** in a mixer FX slot (not the channel rack).
* **Instruments**: load **M4L Bridge Instrument** in the channel rack.

Then click `...`, pick an `.amxd` device, and play. The device window opens after
a few seconds.

If Max is not found automatically, point the second field at `Max.exe`.
`M4LBRIDGE_MAX_EXE` also overrides the search and `M4LBRIDGE_DEVICE` pre-sets the
device.

## How it works

* The host-side VST3 creates a named shared-memory block,
  `Local\M4LBridge_<maxPid>`, and spawns Max with a generated `.maxpat`.
* Two tiny VST3 "shims" run inside Max via `[vst~]`. *M4L Bridge Out* turns the
  ring into a signal feeding `[amxd~]`; *M4L Bridge In* captures the device
  output back into the ring.
* MIDI and tempo go over a local **UDP/OSC** channel:
  `[udpreceive]` → `ctl.js` → `amxd~` (`midievent`) and `[transport]` (`tempo`).
  This avoids depending on `vst~`'s undocumented MIDI outlet.
* The patch sends `start` to `[dac~]` after loading, because instantiating
  `[amxd~]` turns Max's global audio processing off (verified experimentally).
* A jitter buffer (1024 frames, adaptive) absorbs producer/consumer jitter and
  its latency is reported to the host for delay compensation.

## Testing

`M4LBTest.exe` exercises the transport without a host or an audio device:

```bat
M4LBPatchGen.exe <out.vst3> <in.vst3> <device.amxd> > patch.maxpat
M4LBTest.exe "C:\Program Files\Cycling '74\Max 9\Max.exe" patch.maxpat 30
```

It prints `PASS` when a 4096-frame ramp comes back bit-exact.

## Troubleshooting

* **Silence, device window opens** — Max could not open an audio device. Open
  Max's Audio Status (double-click its `dac~`, or start Max manually), set the
  driver to `ad_portaudio Windows WASAPI` or `FL Studio ASIO`, then press
  "Restart Max" in the plugin. If your host uses ASIO in exclusive mode, Max
  cannot share that device.
* **"Max executable not found"** — point the field at `Max.exe`, or set
  `M4LBRIDGE_MAX_EXE`.
* **Nothing happens after picking a device** — press **Diagnose** in the plugin
  (the text is copied to the clipboard) and check
  `%TEMP%\M4LBridge\m4lbridge.log`. The plugin now also shows a message box when
  it cannot start Max.
* **Device window never appears** — check that a `Max` process appears in Task
  Manager after picking the device; if not, the error dialog explains why.
* **Plugin silent after closing Max** — press "Restart Max".

## License

AGPL-3.0 (required by the JUCE 8 AGPL option). A JUCE commercial license removes
that requirement.

This project ships no Cycling '74 or Ableton code. Max for Live and Max are
proprietary; each user must own a valid Max/Max for Live license.
