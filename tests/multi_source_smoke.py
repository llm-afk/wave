"""Bounded native Wave + actual CPU/memory + WASAPI producers on a private port."""
import json
import os
from pathlib import Path
import socket
import subprocess
import time
import sys
import winsound

root = Path(__file__).resolve().parents[1]
output = root / "build" / "multi-source-smoke"
output.mkdir(exist_ok=True)
with socket.socket() as probe:
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
env = dict(os.environ, WAVE_PORT=str(port))
startup = subprocess.STARTUPINFO()
startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
startup.wShowWindow = 0
processes = []
logs = []

def start(executable, args):
    log = open(output / (executable + ".log"), "w", encoding="utf-8")
    logs.append(log)
    process = subprocess.Popen([str(root / "build" / (executable + ".exe")), *args],
                               cwd=output, env=env, startupinfo=startup,
                               stdout=log, stderr=subprocess.STDOUT)
    processes.append(process)
    return process

try:
    viewer = start("WaveDemo", ["--smoke-test"])
    for _ in range(60):
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                break
        except OSError:
            if viewer.poll() is not None:
                raise AssertionError("Viewer exited")
            time.sleep(0.1)
    else:
        raise AssertionError("Viewer did not listen")
    system = start("SystemSource", ["1", "20"])
    audio = start("AudioSource", ["--seconds", "7"])
    if "--play-sound" in sys.argv:
        time.sleep(0.6)
        sound = Path(os.environ["WINDIR"]) / "Media" / "Windows Notify System Generic.wav"
        winsound.PlaySound(str(sound), winsound.SND_FILENAME | winsound.SND_ASYNC | winsound.SND_LOOP)
    assert system.wait(timeout=12) == 0
    if "--play-sound" in sys.argv:
        winsound.PlaySound(None, 0)
    assert audio.wait(timeout=12) == 0
    assert viewer.wait(timeout=20) == 0
    report = json.loads((output / "smoke-test.json").read_text())
    assert report["sources"] == 2 and report["channels"] >= 3, report
    assert report["tcp_frames"] > 1000 and report["tcp_rejected"] == 0, report
    assert not report["render_failed"] and report["presented_frames"] > 0, report
    assert report["max_submitted_plot_points"] == 0, "Receiving must not auto-select signals"
    print("PASS native simultaneous system/audio reception:", report)
finally:
    if "--play-sound" in sys.argv:
        winsound.PlaySound(None, 0)
    for process in processes:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=5)
    for log in logs:
        log.close()
