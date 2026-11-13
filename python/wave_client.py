"""Wave v2 local client, Python standard library only. No process management.

Each client connection owns one uniform or receiver-timed group; Wave accepts
multiple independent source IDs concurrently.
Transport failures are surfaced: never automatically replay an unacknowledged batch.
"""
import math
import socket
import struct


class WaveClient:
    def __init__(self, source_id, source_name, host="127.0.0.1", port=18765):
        self.socket = socket.create_connection((host, port), timeout=5)
        self.request_id = 0
        self.channels = []
        self.next_frame = 0
        try:
            self._request(1, self._text(source_id) + self._text(source_name))
        except Exception:
            self.close()
            raise

    @staticmethod
    def _text(value):
        encoded = value.encode("utf-8")
        if len(encoded) > 256 or "\0" in value or "##" in value:
            raise ValueError("Invalid protocol text")
        return struct.pack("<I", len(encoded)) + encoded

    def _read(self, length):
        data = bytearray()
        while len(data) < length:
            block = self.socket.recv(length - len(data))
            if not block:
                raise ConnectionError("Wave disconnected")
            data.extend(block)
        return bytes(data)

    def _request(self, kind, payload):
        if len(payload) > 1024 * 1024:
            raise ValueError("Payload exceeds 1 MiB")
        self.request_id += 1
        try:
            self.socket.sendall(struct.pack("<5I", 0x32564157, 2, kind,
                                            self.request_id, len(payload)) + payload)
            magic, version, kind, request_id, length = struct.unpack("<5I", self._read(20))
            if (magic, version, kind, request_id) != (0x32564157, 2, 128, self.request_id) or not 8 <= length <= 1024 * 1024:
                raise ConnectionError("Invalid Wave reply")
            response = self._read(length)
            status, text_length = struct.unpack_from("<II", response)
            if text_length > length - 8:
                raise ConnectionError("Truncated reply")
        except (OSError, ConnectionError):
            self.close()
            raise
        if status:
            raise ValueError(response[8:8 + text_length].decode("utf-8"))
        return response[8 + text_length:]

    def register_channels(self, channels, *, time_mode="uniform"):
        """List of dicts: id, name, unit (optional), sample_rate.

        time_mode="receiver" needs no sample_rate; use append_now(values).
        Uniform descriptors require matching sample_rate values.
        Returns stable integer handles in registration order.
        """
        if not 1 <= len(channels) <= 64:
            raise ValueError("Expected 1..64 channels")
        if time_mode not in ("uniform", "receiver"):
            raise ValueError("Unknown time mode")
        payload = struct.pack("<I", len(channels))
        for channel in channels:
            payload += self._text(channel["id"]) + self._text(channel["name"]) + self._text(channel.get("unit", ""))
            payload += struct.pack("<IId", 1, 2 if time_mode == "receiver" else 1,
                                   0 if time_mode == "receiver" else channel["sample_rate"])
        response = self._request(2, payload)
        self.next_frame, count = struct.unpack_from("<QI", response)
        handles = struct.unpack_from("<" + "I" * count, response, 12)
        self.channels = list(channels)
        self.time_mode = time_mode
        return handles

    def append_batch(self, rows, first_frame=None):
        """Rows of synchronous channel values, in registration order."""
        rows = list(rows)
        if getattr(self, "time_mode", "uniform") != "uniform":
            raise ValueError("Use append_now for receiver time mode")
        if not self.channels or not 1 <= len(rows) <= 4096:
            raise ValueError("Register channels first; batch size must be 1..4096")
        count = len(self.channels)
        if any(len(row) != count for row in rows):
            raise ValueError("Each row must contain every registered channel")
        values = [float(value) for row in rows for value in row]
        if not all(math.isfinite(value) for value in values):
            raise ValueError("Samples must be finite")
        first = self.next_frame if first_frame is None else first_frame
        payload = struct.pack("<QI", first, len(rows)) + struct.pack("<" + "f" * len(values), *values)
        self.next_frame, = struct.unpack("<Q", self._request(3, payload))

    def append(self, values, first_frame=None):
        self.append_batch([values], first_frame)

    def append_now(self, values):
        """One value per registered channel; Wave assigns receive time in seconds.

        Returns the receiver's timestamp, relative to the first accepted point.
        """
        if not self.channels or self.time_mode != "receiver":
            raise ValueError("Register channels with time_mode='receiver' first")
        values = [float(value) for value in values]
        if len(values) != len(self.channels) or not all(math.isfinite(v) for v in values):
            raise ValueError("Expected one finite value per registered channel")
        self.next_frame, timestamp = struct.unpack("<Qd", self._request(
            6, struct.pack("<" + "f" * len(values), *values)))
        return timestamp

    def clear_channel(self, handle):
        self._request(4, struct.pack("<I", handle))

    def clear_source(self):
        self._request(5, b"")

    def close(self):
        self.socket.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
