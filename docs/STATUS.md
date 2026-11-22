# Current status (0.8.4)

Verified in FL Studio on Windows 11 with Max 9.2 and Ableton Live 12 Suite
installed.

## Works

| | |
|---|---|
| Effects in a mixer FX slot | yes |
| Instruments in the channel rack | yes (e.g. `Bassline.amxd`) |
| Device UI window | yes |
| Host MIDI 鈫?device | yes (UDP/OSC 鈫?`amxd~` `midievent`) |
| Host tempo 鈫?Max transport | yes, for devices that read Max's transport |
| Audio return | yes, jitter-buffered, host latency reported |
| Max window restore | disabled (no more duplicated previous patch) |

## Does not work

* **Multiple instances.** Max accepts one instance per executable and the
  per-instance shared-memory handshake was not reliable; rolled back to one
  instance per host.
* **Host automation of device parameters** is implemented (0.9.4): the device
  parameter table and ranges are read through `getparams`/`getinfo` and 32 host
  parameters (P01–P32) are mapped onto them in order. Parameter names in the host
  stay generic, there is no device→host value feedback yet, and the mapping is
  listed in the debug log.
* **Devices using the Live API** (`live.object`, `live.path`, `live.observer`,
  `LiveAPI`): the API does not exist outside Live. This includes the
  `ap_timbre resynth` family.
* **`.adv` files**: Live-native device presets, not Max patches.
* Devices that need third-party externals (`.mxe64`) require those externals to
  be installed for Max as well.

## Architecture

```
host VST3 鈹€鈹€ shared memory (audio) 鈹€鈹€ Max process (amxd~ device)
          鈹斺攢 UDP/OSC (MIDI, tempo) 鈹€鈹€鈻?udpreceive 鈫?ctl.js 鈫?amxd~/[transport]
```

* Shared memory name: `Local\M4LBridge_<maxPid>`.
* Shims: `M4L Bridge Out` / `M4L Bridge In` are plain VST3s loaded by `[vst~]`.
* Generated patch and `ctl.js` live in `%APPDATA%\M4LBridge`.
* Log: `%TEMP%\M4LBridge\m4lbridge.log` (rotates at 512 KB).

## Field notes

* Put the effect in a **mixer FX slot**; a VST added to the channel rack gets no
  audio input.
* `STARTING` forever means the shims never connected 鈥?check the log for
  `shim connected` and the `hb` counter.
* `Tone to host` bypasses the bridge and writes a sine to the output bus; use it
  to tell "host routing" apart from "bridge silence".

