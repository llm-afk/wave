"""A Tkinter host embedding the same Wave.dll used by the C++ demo."""
import argparse
from array import array
import ctypes
import json
import math
from pathlib import Path
import threading
import time
import sys
import tkinter as tk
from tkinter import ttk, messagebox

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from wave_native import Library


class Demo:
    def __init__(self, root, smoke=False):
        self.root, self.smoke = root, smoke
        self.stop = threading.Event()
        self.errors = []
        self.started = time.perf_counter()
        self.timer = None
        self.worker = None
        self.plot = self.stream = self.session = None
        root.title("Python 宿主 · Wave 嵌入演示")
        root.geometry("1160x720+120+120")
        root.protocol("WM_DELETE_WINDOW", self.close)
        toolbar = ttk.Frame(root, padding=8)
        toolbar.pack(fill="x")
        self.pause_button = ttk.Button(toolbar, text="暂停显示", command=self.pause)
        self.pause_button.pack(side="left", padx=(0, 12))
        self.auto_y, self.reverse = tk.BooleanVar(value=True), tk.BooleanVar(value=True)
        ttk.Checkbutton(toolbar, text="自动 Y", variable=self.auto_y, command=self.change_y).pack(side="left", padx=6)
        ttk.Checkbutton(toolbar, text="反转滚动", variable=self.reverse,
                        command=lambda: self.plot.set_reverse_scroll(self.reverse.get())).pack(side="left", padx=6)
        for label, seconds in [("1 s", 1), ("100 ms", .1), ("10 ms", .01)]:
            ttk.Button(toolbar, text=label, width=8,
                       command=lambda span=seconds: self.plot.set_time_span(span)).pack(side="left", padx=3)
        self.status = ttk.Label(toolbar, text="模拟数据 · 2 通道 · 20 kSa/s")
        self.status.pack(side="left", padx=12)
        self.surface = tk.Frame(root, background="black")
        self.surface.pack(fill="both", expand=True)
        root.update_idletasks()
        try:
            self.session = Library().session()
            self.stream = self.session.open_stream("demo", [
                {"id": "a", "name": "Channel A", "unit": "V"},
                {"id": "b", "name": "Channel B", "unit": "V"}], 20_000)
            self.plot = self.session.create_window(self.surface.winfo_id())
            self.plot.set_reverse_scroll(True)
            self.resize()
            self.surface.bind("<Configure>", self.resize)
            self.worker = threading.Thread(target=self.produce, name="Wave demo acquisition", daemon=True)
            self.worker.start()
            self.timer = root.after(16, self.frame)
        except Exception:
            self.close()
            raise

    def resize(self, event=None):
        if self.plot:
            self.plot.set_bounds(0, 0, self.surface.winfo_width(), self.surface.winfo_height())

    def produce(self):
        frame = 0
        deadline = time.perf_counter()
        try:
            while not self.stop.is_set():
                samples = array("f")
                for i in range(200):
                    t = (frame + i) / 20_000
                    samples.extend((math.sin(2 * math.pi * 8 * t), .55 * math.sin(2 * math.pi * 13 * t)))
                frame = self.stream.append(samples, frame)
                deadline += .01
                self.stop.wait(max(0, deadline - time.perf_counter()))
        except Exception as error:
            self.errors.append(str(error))

    def pause(self):
        self.plot.set_paused(not self.plot.state().paused)

    def change_y(self):
        if self.auto_y.get():
            self.plot.set_auto_y()
        else:
            self.plot.set_y_range(-1.5, 1.5)

    def frame(self):
        self.timer = None
        try:
            if self.errors:
                raise RuntimeError(self.errors[0])
            self.plot.render()
            state, data = self.plot.state(), self.stream.state()
            self.pause_button.configure(text="恢复实时" if state.paused else "暂停显示")
            self.auto_y.set(bool(state.auto_y))
            self.status.configure(text=f"模拟 2 通道 · 20 kSa/s · 已接收 {data.next_frame} 帧")
            if self.smoke and time.perf_counter() - self.started >= 4:
                self.smoke_ok = state.plotted_points > 0 and data.next_frame >= 50_000
                Path("python-host-smoke.json").write_text(json.dumps({
                    "ok": self.smoke_ok, "received_frames": data.next_frame,
                    "retained_frames": data.frame_count, "plotted_points": state.plotted_points}), encoding="utf-8")
                self.close()
                return
            self.timer = self.root.after(16, self.frame)
        except Exception as error:
            self.errors.append(str(error))
            if not self.smoke:
                messagebox.showerror("Wave Python host", str(error), parent=self.root)
            self.close()

    def close(self):
        if self.timer is not None:
            self.root.after_cancel(self.timer)
            self.timer = None
        self.stop.set()
        if self.worker:
            self.worker.join()
        for name in ("plot", "stream", "session"):
            resource = getattr(self, name)
            if resource:
                resource.close()
                setattr(self, name, None)
        self.root.destroy()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--smoke-test", action="store_true")
    args = parser.parse_args()
    ctypes.windll.user32.SetProcessDPIAware()
    root = tk.Tk()
    if args.smoke_test:
        root.withdraw()
    demo = Demo(root, args.smoke_test)
    root.mainloop()
    if args.smoke_test and not getattr(demo, "smoke_ok", False):
        raise SystemExit("Python host check failed: " + "; ".join(demo.errors))
