"""In-process Wave DLL bindings. Python standard library only.

Supply a native parent window on its UI thread. Call render() from the host's
timer. Acquisition threads may submit contiguous float32 buffers independently.
Close windows on their UI thread, and stop writers before closing their handles.
"""
import array
import ctypes as c
from pathlib import Path
import threading


class _Channel(c.Structure):
    _fields_ = [("id", c.c_char_p), ("name", c.c_char_p), ("unit", c.c_char_p)]


class StreamState(c.Structure):
    _fields_ = [("next_frame", c.c_uint64), ("first_frame", c.c_uint64), ("frame_count", c.c_uint64),
                ("channel_count", c.c_uint32), ("capacity_frames", c.c_uint32),
                ("begin_seconds", c.c_double), ("end_seconds", c.c_double)]


class WindowState(c.Structure):
    _fields_ = [("x_min", c.c_double), ("x_max", c.c_double), ("y_min", c.c_double), ("y_max", c.c_double),
                ("plotted_points", c.c_uint64), ("paused", c.c_uint32), ("auto_y", c.c_uint32)]


class Library:
    def __init__(self, path=None):
        path = Path(path) if path else Path(__file__).resolve().parents[1] / "build" / "Wave.dll"
        self.dll = c.CDLL(str(path.resolve()))
        handle, u32, u64, number = c.c_void_p, c.c_uint32, c.c_uint64, c.c_double
        output, samples = c.POINTER(handle), c.POINTER(c.c_float)
        signatures = {
            "session_create": [u32, output], "session_destroy": [handle], "session_set_capacity": [handle, u32],
            "stream_open": [handle, c.c_char_p, c.c_char_p, number, c.POINTER(_Channel), u32, output],
            "stream_destroy": [handle], "stream_get_state": [handle, c.POINTER(StreamState)],
            "stream_append": [handle, u64, samples, u64, c.POINTER(u64)],
            "stream_append_received": [handle, samples, u32, number, c.POINTER(u64)],
            "window_create": [handle, c.c_size_t, output], "window_destroy": [handle],
            "window_get_handle": [handle, c.POINTER(c.c_size_t)],
            "window_set_bounds": [handle, c.c_int, c.c_int, c.c_int, c.c_int],
            "window_render": [handle], "window_get_state": [handle, c.POINTER(WindowState)],
            "window_set_paused": [handle, c.c_int], "window_set_time_span": [handle, number],
            "window_set_y_range": [handle, number, number], "window_set_auto_y": [handle, c.c_int],
            "window_set_channel_visible": [handle, u32, c.c_int], "window_set_reverse_scroll": [handle, c.c_int],
        }
        self.dll.wave_abi_version.argtypes = []
        self.dll.wave_abi_version.restype = u32
        self.dll.wave_last_error.argtypes = []
        self.dll.wave_last_error.restype = c.c_char_p
        if self.dll.wave_abi_version() != 1:
            raise RuntimeError("Unsupported Wave ABI version")
        for name, arguments in signatures.items():
            function = getattr(self.dll, "wave_" + name)
            function.argtypes, function.restype = arguments, c.c_int

    def check(self, status):
        if status:
            message = self.dll.wave_last_error().decode("utf-8", errors="replace")
            raise RuntimeError(message)

    def session(self, capacity_frames=50_000):
        if not 8 <= capacity_frames <= 2_000_000:
            raise ValueError("capacity_frames must be in 8..2000000")
        result = c.c_void_p()
        self.check(self.dll.wave_session_create(capacity_frames, c.byref(result)))
        return Session(self, result, "session_destroy")


class _Handle:
    def __init__(self, library, handle, destroy, *, ui=False):
        self.library, self._handle, self._destroy = library, handle, destroy
        self._lock = threading.RLock()
        self._owner = threading.get_ident() if ui else None

    def _call(self, name, *arguments):
        with self._lock:
            if self._owner is not None and self._owner != threading.get_ident():
                raise RuntimeError("Window calls must run on the creating UI thread")
            if not self._handle:
                raise RuntimeError("Wave handle is closed")
            self.library.check(getattr(self.library.dll, "wave_" + name)(self._handle, *arguments))

    def close(self):
        with self._lock:
            if self._handle:
                self._call(self._destroy)
                self._handle = None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


class Session(_Handle):
    def set_capacity(self, frames):
        if not 8 <= frames <= 2_000_000:
            raise ValueError("capacity_frames must be in 8..2000000")
        self._call("session_set_capacity", frames)

    def open_stream(self, stream_id, channels, sample_rate, *, name=None):
        """Channels are dicts with id, name and optional unit. IDs remain stable."""
        descriptors = [_Channel(ch["id"].encode("utf-8"), ch["name"].encode("utf-8"), ch.get("unit", "").encode("utf-8"))
                       for ch in channels]
        buffer = (_Channel * len(descriptors))(*descriptors)
        result = c.c_void_p()
        self._call("stream_open", stream_id.encode("utf-8"), (name or stream_id).encode("utf-8"),
                   sample_rate, buffer, len(buffer), c.byref(result))
        return Stream(self.library, result, "stream_destroy")

    def create_window(self, parent_handle):
        result = c.c_void_p()
        self._call("window_create", parent_handle, c.byref(result))
        return Window(self.library, result, "window_destroy", ui=True)


def _float32_buffer(values):
    try:
        view = memoryview(values)
    except TypeError:
        view = memoryview(array.array("f", values))
    if not view.c_contiguous or view.itemsize != 4 or view.format not in ("f", "<f", "=f", "@f"):
        raise ValueError("Expected a C-contiguous native float32 buffer")
    raw = view.cast("B")
    storage = c.c_float * (raw.nbytes // 4)
    return storage.from_buffer_copy(raw) if raw.readonly else storage.from_buffer(raw)


class Stream(_Handle):
    def append(self, values, first_frame=None):
        """Interleaved float32 frames. Returns the next accepted frame index."""
        first = (1 << 64) - 1 if first_frame is None else first_frame
        if not 0 <= first < (1 << 64):
            raise ValueError("first_frame is outside uint64")
        buffer = _float32_buffer(values)
        result = c.c_uint64()
        self._call("stream_append", first, buffer, len(buffer), c.byref(result))
        return result.value

    def append_received(self, values, seconds):
        buffer = _float32_buffer(values)
        result = c.c_uint64()
        self._call("stream_append_received", buffer, len(buffer), seconds, c.byref(result))
        return result.value

    def state(self):
        result = StreamState()
        self._call("stream_get_state", c.byref(result))
        return result


class Window(_Handle):
    @property
    def handle(self):
        result = c.c_size_t()
        self._call("window_get_handle", c.byref(result))
        return result.value

    def set_bounds(self, x, y, width, height):
        self._call("window_set_bounds", x, y, width, height)

    def render(self):
        self._call("window_render")

    def set_paused(self, paused):
        self._call("window_set_paused", bool(paused))

    def set_time_span(self, seconds):
        self._call("window_set_time_span", seconds)

    def set_y_range(self, minimum, maximum):
        self._call("window_set_y_range", minimum, maximum)

    def set_auto_y(self, enabled=True):
        self._call("window_set_auto_y", bool(enabled))

    def set_channel_visible(self, channel, visible):
        if not 0 <= channel < (1 << 32):
            raise ValueError("channel is outside uint32")
        self._call("window_set_channel_visible", channel, bool(visible))

    def set_reverse_scroll(self, enabled):
        self._call("window_set_reverse_scroll", bool(enabled))

    def state(self):
        result = WindowState()
        self._call("window_get_state", c.byref(result))
        return result
