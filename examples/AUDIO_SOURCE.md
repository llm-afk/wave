# Playback audio source

1. When updating Wave, close the old Wave instance and reopen the new build.
2. Open `run.cmd` (Wave only).
3. Open `run-audio-source.cmd` (audio source only).
4. Play music, a video or another sound on the default Windows playback device.
5. In Wave's context menu, manually select Audio Left / Audio Right (or numbered
   Audio channels for other formats). Registration never selects channels.

The audio source and `run-source.cmd` can run together against the same Wave.
CPU usage, Memory usage and Audio signals appear in one flat selection menu.
Each source retains its own sample rate and buffer; start order is free.
Closing either process does not close the other. Ctrl+C or closing the source
console ends capture. Device removal/format errors stop the source with an error;
restart it after changing the default output device.

This uses WASAPI shared-mode loopback on the default **playback** endpoint. It
captures the actual Windows output mix, not a microphone, and saves no audio file.
The endpoint's mix rate and channel count are queried and registered dynamically.
The source does not generate a sine wave or synthetic input. When Windows delivers
a SILENT packet its samples are zero; when no packets arrive, nothing is invented.
Samples are normalized float32 amplitudes (usually near -1..1), not calibrated Pa/V.

For a 48 kHz stereo endpoint, the target batch is 1920 frames (~40 ms), giving
25 request/ACK exchanges per second rather than one per sample. Higher rates are
capped at 4096 frames and the protocol payload limit. Partial batches flush after
200 ms. Capture drains on its own thread; TCP waits do not block WASAPI reads.
The bounded queue retains up to two seconds. Overflow drops oldest pending frames
and reports the count. Device positions preserve sequence gaps, so the current
Wave data store starts a new contiguous segment on a gap (it does not stitch over
missing audio). Reconnect discards pending old samples and resumes from Wave's
acknowledged index. This is live visualization, not a lossless audio recorder.

Wave's initial buffer holds 100,000 points/channel: about 2.08 seconds at 48 kHz.
To change history, set **设置 → 缓冲区点数** to
`sample_rate * desired_seconds`. Then zoom the X axis inward to see
the oscillations. Empty windows retain their axes until a channel is selected.

Wave automatically smooths the live time axis for regularly batched data. It
learns the arrival cadence and keeps roughly two batches of display headroom
(typically 80–100 ms at 25 packets/second), then advances on each rendered frame.
The protocol, timestamps and sample values are unchanged. All live windows use
the same display clock; freezing still holds the view while reception continues.
When input stops, the display drains to the last real sample and stops there.
Very short user-selected histories cannot retain this headroom, so Wave falls
back to displaying the available data without delaying it. Slow telemetry and
large offline loads also continue to appear immediately.

The console reports captured/sent frames, packet count, queue drops, discontinuities
and maximum amplitude. A positive peak confirms nonzero samples; zero can mean
silence and does not by itself mean capture failed.

Diagnostics:

```
build\AudioSource.exe --probe
build\AudioSource.exe --seconds 8
```

`--probe` queries/initializes the device format without starting capture. The
bounded duration includes waiting for Wave. It is useful for tests, not required
for normal usage. The program never launches Wave.
