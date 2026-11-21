# M4L Bridge (prototype)

Play **Ableton Max for Live devices (`.amxd`)** inside other VST3 hosts (FL Studio,
Cubase, Reaper, Bitwig, …) on Windows.

**Status: P0 prototype.** The shared-memory transport and the `amxd~` hosting
path are verified on Windows 11 with Max 9.2 (see `docs/test-report.md`). Loading
the plugin in FL Studio still needs a manual pass, and device parameters are not
exposed to the host yet.

The bridge does **not** reimplement Max for Live. It drives the Max runtime that
you already have installed: the VST3 spawns a hidden `Max.exe`, which hosts the
device through Max's built-in `amxd~` object, and audio/MIDI travel over a
shared-memory ring between the two processes.

```
┌────────────────────┐   shared memory ring    ┌──────────────────────────┐
│ host (FL Studio)   │  audio in / out, MIDI   │ Max.exe (spawned)        │
│  M4L Bridge.vst3   │ ◄─────────────────────► │  generated .maxpat:      │
│                    │                         │  [vst~ Out] → [amxd~] →  │
│                    │                         │  [vst~ In] → [*~ 0] →    │
└────────────────────┘                         │  [dac~]                  │
                                               └──────────────────────────┘
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

Known P0 limitations:

* device parameters are not exposed to the host yet (no automation lanes); use
  the device UI window.
* Max needs a working audio output device; it is only used as the DSP clock
  (the patch outputs silence). Clock drift between host and Max is not
  compensated yet.
* Max is effectively single-instance: if Max is already running, the spawned
  instance may hand the patch to it and exit. Close Max before using the bridge.
* one Max process per plugin instance; if Max dies the plugin stays silent until
  "Restart Max" is pressed.

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
build/M4LBridgeOut_artefacts/Release/VST3/M4L Bridge Out.vst3
build/M4LBridgeIn_artefacts/Release/VST3/M4L Bridge In.vst3
build/Release/M4LBTest.exe          # transport test
build/Release/M4LBPatchGen.exe      # prints the generated .maxpat
```

Copy all three bundles into the **same** VST3 folder. The bridge finds the two
shims next to itself.

> Build in an ASCII-only path. JUCE's resource compiler chokes on non-ASCII
> build paths (e.g. `D:\文档\...`); source paths are fine.

## Usage

1. Load **M4L Bridge** on a track in your host.
2. Click `...` and pick an `.amxd` device.
3. The device window opens after a few seconds. Play audio through the track.

If Max is not found automatically, point the second field at `Max.exe`.
`M4LBRIDGE_MAX_EXE` also overrides the search, `M4LBRIDGE_DEVICE` pre-sets the
device, and `M4LBRIDGE_DEBUG=1` writes `%TEMP%\M4LBridge\debug.log`.

## How it works

* The host-side VST3 creates a named shared-memory block,
  `Local\M4LBridge_<maxPid>`, and spawns Max with a generated `.maxpat`.
* Two tiny VST3 "shims" run inside Max via `[vst~]`. *M4L Bridge Out* turns the
  ring into a signal feeding `[amxd~]`; *M4L Bridge In* captures the device
  output back into the ring.
* The patch sends `start` to `[dac~]` after loading, because instantiating
  `[amxd~]` turns Max's global audio processing off (verified experimentally).
* Sample-rate mismatch is handled with a linear resampler in the bridge.

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
