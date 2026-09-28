# Wave TCP v2

Wave is an independent local display component. Start it with `run.cmd`; start
your producer separately. Neither process owns the other's lifetime. Wave is
empty before registration and never selects channels on data arrival.

## Lifecycle

1. Connect to 127.0.0.1:18765.
2. HELLO: identify the source; wait for success.
3. REGISTER: atomically declare all channel metadata; wait for handles and next index.
4. DATA / DATA_NOW / CLEAR_CHANNEL / CLEAR_SOURCE: send a request, then wait for its reply.

Wave accepts multiple independent sources concurrently. Each TCP connection owns
one source ID and one synchronous group of 1..64 float32 channels. Each group
chooses its own uniform rate (up to 1 MHz) or receiver timing. Different groups
may use different rates and modes. A second live connection claiming the same
source ID is rejected, without interrupting the existing owner. Channel identity
is the pair (source ID, channel ID); handles remain local to the connection's source.

Limits: 32 concurrent connections, 32 retained sources and 256 total channels per
Wave session. Buffer settings use an estimated 512 MiB sample-storage allocation
budget; actual process memory also includes summaries, snapshots and graphics.
Dynamic edits of a registered schema, different rates within one group, explicit
device timestamps and external window-control commands are not implemented.
Use another source ID for a new schema, or restart Wave to start fresh.

Reconnect with the same source ID and identical metadata in the same order.
Wave retains data, selections, pause and clear boundaries and returns the next
sample index. No batch is automatically replayed after an uncertain connection
failure: reconnect/register and use the returned index. Reconnecting after Wave
restarts creates a fresh session with index zero. In uniform mode disconnect downtime is not
represented unless the producer advances its sequence explicitly; a sequence gap
retains history within timeline capacity and connects the real samples across missing slots.
Missing slots consume capacity, are never zero-filled and are not counted as received samples.

## Framing

Little-endian integers and IEEE754 numbers, no native struct padding. All requests
and responses use this 20-byte header:

| Offset | Type | Meaning |
| --- | --- | --- |
| 0 | u32 | Magic 0x32564157 |
| 4 | u32 | Version 2 |
| 8 | u32 | Message type |
| 12 | u32 | Nonzero request ID, echoed in reply |
| 16 | u32 | Payload byte length, at most 1,048,576 |

Text is u32 byte length followed by UTF-8 bytes (no terminator). Maximum 256 bytes;
control characters, invalid UTF-8 and ImGui's reserved `##` delimiter are rejected.
IDs/names must be nonempty; unit can be empty. Display names may repeat; IDs must
be unique within the source. Units remain metadata and are shown in the channel
menu tooltip, not appended automatically to the legend.

### HELLO (1)

Payload: source ID text, source display-name text. Source name is currently
validated but not used for window titles. No source process is launched by Wave.

### REGISTER (2)

Payload: u32 channel count, followed by this descriptor for each channel:

`text id, text name, text unit, u32 value_type, u32 time_mode, f64 sample_rate_hz`

Supported value_type=1 (float32). time_mode=1 is uniform sampling with a positive
sample rate; time_mode=2 is receiver timing and requires sample_rate_hz=0.
All channels must use the same mode and rate. Registration is atomic; an error leaves the existing schema/data intact.
This constraint is per source connection; other sources may use different modes/rates.
On success the reply extra data is `u64 next_frame, u32 count, count * u32 handle`.
Handles start at 1 and are stable across compatible reconnects.

### DATA (3)

Payload: `u64 first_frame, u32 frame_count, interleaved float32 values`.
Count is 1..4096, also limited by the frame payload cap. Each frame includes all
channels in registration order. One point per channel is simply count=1.
Sample spacing is determined by frame_index / registered_sample_rate, plus the
source's fixed display offset described below; packet-to-packet arrival jitter
does not change it. Values must be finite; stale/backward indices are rejected.
Reply extra data is u64 next_frame. Validation completes before buffer mutation.
This message is only valid for time_mode=1.

### DATA_NOW (6)

Only valid for time_mode=2. Payload: exactly one float32 value per registered
channel, in registration order. There is no timestamp, frame index or count.
Thus a single-channel point has a 4-byte payload; two channels have 8 bytes.
One request represents one synchronous point per channel, never a batch.

Wave captures the system monotonic clock after the complete message is received,
before waiting for the GUI thread. The first accepted data message across all
sources defines the common time origin; a later source does not start again at zero.
Subsequent points retain their actual receive intervals. The clock is unaffected
by wall-clock adjustments; it is not a calendar/UTC timestamp or a device sampling
timestamp. TCP and sender delays affect it. Identical clock readings are advanced
by the smallest representable increment to preserve strict ordering.

Reply extra: `u64 next_frame, f64 receive_time_seconds`. The next index is assigned
by Wave. Reconnecting the same source/schema keeps the common time origin and includes
disconnected elapsed time; Wave restart creates a new origin. Clear commands do
not reset it. Values must be finite; wrong sizes/modes are rejected atomically.
Use uniform DATA batches for high-frequency acquisition; DATA_NOW is convenient
for irregular single-point telemetry. Automatic batch display smoothing is not
applied to irregular receiver-timed points.

Disconnect retains metadata and history for inspection/reconnection. The flat
signal menu only displays currently connected data names. Disconnected entries
are hidden and reappear on compatible reconnect, preserving selections. Existing
plots retain their history. No TCP addresses, protocol IDs or source groups appear in the normal UI.
Repeated names receive display-only numeric suffixes; registration order and
selection identity stay stable across reconnects.

### Common display timeline

Uniform source timestamps displayed by Wave are `offset + frame_index / rate`.
Wave anchors the last sample in a source's first accepted batch to the receive
time of that batch on the common clock. Earlier samples in that batch can have
negative times. The offset is then fixed, including across compatible reconnects;
later network jitter does not stretch sample spacing. The producer must advance
its sequence to represent unrecorded time, including downtime on reconnect.

This is software alignment, not hardware clock synchronization: capture latency,
transport latency and independent device clock drift remain. Receiver-timed
points directly use the common receive clock. External programs still send no
timestamps in either mode; the existing v2 layouts are unchanged.

Live following uses only signals selected in open windows when X linking is on,
or the current window's selection when it is off. If any selected source is
online with data, disconnected sources do not drive the live playhead. Unselected
producers never advance the visible timeline. Selected sources still retain
their true relative time offsets; different update rates are not time-shifted
just to force every signal endpoint onto the same pixel.

Each connection has its own worker and at most one pending request. The UI polls
all connections fairly, commits data, then returns ACK. One partial/idle peer
cannot block the others. All source stores are UI-committed; global pause takes a
coherent collection of immutable snapshots. Global buffer resizing prepares all
replacement stores before committing. Source clear commands affect only their
own source, local window clear keeps independent sequence cutoffs, and global UI
clear affects all sources. Default capacity is 100,000 points per channel.

### CLEAR_CHANNEL (4) / CLEAR_SOURCE (5)

CLEAR_CHANNEL payload: u32 handle. CLEAR_SOURCE payload: empty.

These logically remove history before the next sample index for one channel or
all channels of this source, respectively. Every window, including a frozen
window and its overview, stops showing those old samples. They do not delete
windows, change selections, reset indices, resume frozen views or affect another
channel when clearing one. New samples continue normally. Underlying ring memory
is reused, not securely erased. Independently held immutable snapshots remain
immutable; Wave applies the live clear boundaries when displaying them.

The UI's **清空数据** remains a window-local display action, not CLEAR_CHANNEL.

### REPLY (128)

`u32 status, text error_message, optional extra data`

status=0 means committed successfully; status=1 means rejection. An error carries
a human-readable reason and has no extra data. The request ID must match. Semantic
errors allow correction on the same connection. Invalid framing/version/oversize
headers close the connection because framing cannot be trusted. An incomplete
header/payload or handshake times out after 5 seconds; a registered idle connection
stays open. The sender must wait for each reply; bounded request/response flow is
the initial backpressure policy. At high sample rates use batches: one packet per
point is not intended as a high-throughput mode.

## Python example

`examples/wave_client.py` uses only the Python standard library:

```python
from wave_client import WaveClient

with WaveClient("motor-1", "Motor test") as wave:
    current, speed = wave.register_channels([
        {"id": "ia", "name": "A 相电流", "unit": "A", "sample_rate": 1000},
        {"id": "speed", "name": "转速", "unit": "rpm", "sample_rate": 1000},
    ])
    wave.append([1.2, 300])
    wave.append_batch([[1.3, 301], [1.4, 302]])
    wave.clear_channel(current)  # shared current history only
    wave.clear_source()          # all history for this source
```

The client reports transport and server errors; it does not spawn Wave or silently
retry uncertain commands. The real system-source executable separately implements
reconnection and always re-registers before sending.

Wire v2 replaces the fixed CPU/memory v1 prototype; old producers must be rebuilt.

Receiver-timed single points (can coexist with other source IDs):

```python
import time
from wave_client import WaveClient

with WaveClient("telemetry", "Telemetry") as wave:
    wave.register_channels([
        {"id": "temperature", "name": "温度", "unit": "°C"},
    ], time_mode="receiver")
    print(wave.append_now([25.1]))  # seconds on Wave's common timeline
    time.sleep(0.37)
    print(wave.append_now([25.2]))  # about 0.37 s later; no timestamp sent
```
