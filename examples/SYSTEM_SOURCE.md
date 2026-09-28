# External system data demo

Start Wave with run.cmd. Start the independent producer with run-source.cmd.
Either order works. Closing either program does not close the other.
Wave waits for a connection; the source retries if Wave is unavailable.
Wave no longer generates simulated signals. The old joint launcher was removed.

CPU usage and physical memory usage are real Windows readings, in percent;
CPU temperature is not fabricated. CPU usage uses differences of GetSystemTimes
(kernel includes idle). On machines with more than 64 logical processors this
API is limited to the caller's primary processor group.

The initial window selects no channels. Receiving data never changes selection. Select CPU usage or Memory usage in the panel's
context menu or add another panel. Freeze and zoom work as before, reception
continues in the background. The title stays simply Wave; the selection menu
shows data names, with update status in hover hints. This source can run alongside
`run-audio-source.cmd` in the same Wave instance.

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
