# External system data demo

Start Wave with run.cmd. Start the independent producer with run-source.cmd.
Either order works. Closing either program does not close the other.
Wave waits for a connection; the source retries if Wave is unavailable.
Wave no longer generates simulated signals. The old joint launcher was removed.

CPU usage and physical memory usage are real Windows readings, in percent;
CPU temperature is not fabricated. CPU usage uses differences of GetSystemTimes
(kernel includes idle). On machines with more than 64 logical processors this
API is limited to the caller's primary processor group.

Registered channels start at the left of a ten-second plot and scroll once it fills.
Auto Y fits the visible percentage readings to 80% of the plot height.
Wheel/right-drag zoom is available on the axes and plot, while live
following continues. Left drag pans Y live and both axes while paused. Double
click holds the display while reception continues, then resumes at the latest
cache. The time axis follows the original acquisition timeline in seconds.
There are no settings menus or additional panels. Embedding hosts set
ranges and channel visibility through WaveView's API. The title stays Wave.
This source can run alongside `run-audio-source.cmd` in the same Wave instance.

For batching: `run-source.cmd 5` sends five sample groups per
message (one second of data). Default batch size 1 sends every 200 ms.
The source runs in its own console; close it or press Ctrl+C to stop. Manual commands:

```
build\WaveDemo.exe
build\SystemSource.exe 5
```

Optional second producer argument limits samples, e.g. `SystemSource.exe 5 20`.
The producer reconnects automatically in unlimited mode. Samples while
disconnected are not recorded. A reconnect resumes after the receiver's last
index; downtime is omitted from this demo's X timeline. Sampling uses nominal
5 Hz slots, not hardware timestamps. Late slots are skipped, never filled with
invented readings; DataStore retains history within capacity and connects real samples across gaps.

## Protocol

The source now sends HELLO, registers CPU and memory metadata (IDs, names, units, float32 type, uniform time mode and 5 Hz rate), waits for confirmation, then sends data. Wave has no built-in channel names.

See [Wave TCP v2](PROTOCOL.md) for framing, acknowledgements, clear scopes, limits and the Python client API. Old v1 producers are not compatible.
