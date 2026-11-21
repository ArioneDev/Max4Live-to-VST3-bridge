# Changelog

## 0.8.4 — stable baseline

* Audio path reverted to the Max-PID shared-memory key (0.7.0 behaviour).
  Multi-instance was rolled back: the per-instance `bridgeid` handshake did not
  reach the shim reliably in FL Studio.
* MIDI and tempo travel over UDP/OSC (`udpreceive` → `ctl.js` → `amxd~` /
  `[transport]`), independent of the audio path.
* Tempo uses `[transport]`'s `tempo` attribute (the earlier `max transport`
  message was invalid).
* UI: white flat theme, resizable 560×400 – 1280×900, larger type, version
  hyperlink bottom-right.
* Diagnostics: `Tone to host` writes a tone straight to the output bus,
  `Test tone` feeds the device, jitter-buffer / ring / MIDI counters in the log.

## 0.8.0

* Multi-instance attempt: per-instance shared-memory key plus a `bridgeid`
  VST3 parameter carried by the generated patch (rolled back in 0.8.4).
* Host BPM forwarded to Max.

## 0.7.0

* MIDI moved from `vst~`'s MIDI outlet to a UDP/OSC control channel.
* `ctl.js` written next to the generated patch; logs every received message.

## 0.6.x

* Instrument build (`M4L Bridge Instrument`) for the channel rack.
* Max window-restore disabled so the previous patch no longer pops up.
* Max death detection; engine restart waits for the old process to exit.
* `amxd~` hosted instrument patches (Bassline) load correctly.

## 0.4.0 – 0.5.0

* Jitter buffer + host latency reporting; fixes the "bitcrush" underrun sound.
* `processBlock` no longer clears the shared input/output buffer before reading
  the host's audio (this was why `in` stayed at -100 dB).
* White flat UI, resizable, bigger fonts.

## 0.1 – 0.3

* Proof of concept: shared-memory ring, generated `.maxpat`, two shims,
  `amxd~` hosting, device UI window, `start` after `amxd~` load.
