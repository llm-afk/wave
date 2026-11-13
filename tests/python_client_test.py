"""Real Wave process + Python client registration/clear/reconnect integration."""
from pathlib import Path
import json
import os
import socket
import subprocess
import sys
import time

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "examples"))
from wave_client import WaveClient

# Use a private port and per-mode reports, never an existing receiver.
receiver_time = "--receiver-time" in sys.argv
output = root / "build" / "python-client-smoke" / ("receiver" if receiver_time else "uniform")
output.mkdir(parents=True, exist_ok=True)
with socket.socket() as probe:
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
env = dict(os.environ, WAVE_PORT=str(port))
startup = subprocess.STARTUPINFO()
startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
startup.wShowWindow = 0
viewer = subprocess.Popen([str(root / "build/WaveDemo.exe"), "--smoke-test"],
                          cwd=output, env=env, startupinfo=startup)
channels = [dict(id="current", name="电流", unit="A", sample_rate=100),
            dict(id="voltage", name="电压", unit="V", sample_rate=100),
            dict(id="temp", name="温度", unit="℃", sample_rate=100)]
try:
    for _ in range(40):
        try:
            client = WaveClient("python-test", "Python source", port=port)
            break
        except OSError:
            time.sleep(0.1)
    else:
        raise AssertionError("Receiver did not start")
    with client:
        if receiver_time:
            channels = [{k:v for k,v in channel.items() if k != "sample_rate"} for channel in channels]
            client.register_channels(channels, time_mode="receiver")
            assert client.append_now([1,2,3]) == 0
            time.sleep(0.07)
            assert client.append_now([4,5,6]) >= 0.06
            client.clear_source()
    if receiver_time:
        with WaveClient("python-test", "Python source", port=port) as client:
            client.register_channels(channels, time_mode="receiver")
            assert client.next_frame == 2
            assert client.append_now([7,8,9]) >= 0.06
        assert viewer.wait(timeout=20) == 0
        report = json.loads((output / "smoke-test.json").read_text())
        assert report["channels"] == 3 and report["sample_rate_per_channel"] == 0
        assert report["tcp_frames"] == 3 and report["tcp_rejected"] == 0
        assert report["max_submitted_plot_points"] > 0 and not report["render_failed"]
        assert report["rendered_frames"] > 0 and report["presented_frames"] > 0
        print("PASS Python receiver timestamps, 3 channels, clear, reconnect and real Wave rendering")
        sys.exit(0)
    # The first connection above only handshakes in uniform mode.
    with WaveClient("python-test", "Python source", port=port) as client:
        try:
            client.register_channels([channels[0], channels[0]])
            raise AssertionError("Duplicate IDs accepted")
        except ValueError:
            pass
        handles = client.register_channels(channels)
        assert handles == (1, 2, 3)
        client.append([1, 2, 3])
        client.append_batch([[4, 5, 6], [7, 8, 9]])
        client.clear_channel(handles[0])
        client.clear_source()
        client.append([10, 11, 12])
    with WaveClient("python-test", "Python source", port=port) as client:
        client.register_channels(channels)
        assert client.next_frame == 4
        client.append([13, 14, 15])
    assert viewer.wait(timeout=20) == 0
    report = json.loads((output / "smoke-test.json").read_text())
    assert report["channels"] == 3 and report["sample_rate_per_channel"] == 100
    assert report["tcp_frames"] == 5 and report["tcp_rejected"] == 1
    assert report["max_submitted_plot_points"] > 0 and not report["render_failed"]
    assert report["rendered_frames"] > 0 and report["presented_frames"] > 0
    print("PASS Python client: UTF-8 metadata, 3 channels/100 Hz, error replies, single/batch, clear, reconnect, automatic live display")
finally:
    if viewer.poll() is None:
        viewer.terminate()
        viewer.wait(timeout=5)
