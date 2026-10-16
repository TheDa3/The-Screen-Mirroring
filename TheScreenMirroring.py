import io
import queue
import select
import socket
import struct
import threading
import time
import tkinter as tk
from concurrent.futures import ThreadPoolExecutor
from tkinter import ttk

import numpy as np
from PIL import Image

try:
    import dxcam
except Exception:
    dxcam = None
try:
    import mss
except Exception:
    mss = None
try:
    import pyaudiowpatch as pyaudio
except Exception:
    pyaudio = None

STREAM_PORT = 9000
DISCOVERY_PORT = 9001
AUDIO_PORT = 9002
REQUEST = b"PS4MIRROR_DISCOVER"
REPLY = b"PS4MIRROR_HERE"

TILES = 4
AUDIO_RATE = 48000
AUDIO_GRAIN = 1024
AUDIO_BYTES = AUDIO_GRAIN * 4

def local_ips():
    ips = set()
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ips.add(info[4][0])
    except OSError:
        pass
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ips.add(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    return [ip for ip in ips if not ip.startswith("127.") and not ip.startswith("169.254.")]


def find_ps4(stop_evt, timeout=4.0):
    """Broadcast on every network adapter; returns (ip, video_port, audio_port) or None."""
    socks = []
    for ip in local_ips():
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            s.bind((ip, 0))
            socks.append((s, ip))
        except OSError:
            pass
    if not socks:
        time.sleep(1)
        return None

    deadline = time.time() + timeout
    next_send = 0.0
    try:
        while time.time() < deadline and not stop_evt.is_set():
            if time.time() >= next_send:
                for s, ip in socks:
                    for dest in ("255.255.255.255", ip.rsplit(".", 1)[0] + ".255"):
                        try:
                            s.sendto(REQUEST, (dest, DISCOVERY_PORT))
                        except OSError:
                            pass
                next_send = time.time() + 1.0
            readable, _, _ = select.select([s for s, _ in socks], [], [], 0.25)
            for s in readable:
                try:
                    data, addr = s.recvfrom(256)
                except OSError:
                    continue
                if data.startswith(REPLY):
                    p = data.decode(errors="ignore").split("|")
                    vp = int(p[1]) if len(p) > 1 and p[1].isdigit() else STREAM_PORT
                    ap = int(p[3]) if len(p) > 3 and p[3].isdigit() else AUDIO_PORT
                    return addr[0], vp, ap
    finally:
        for s, _ in socks:
            s.close()
    return None

_capture = None


class Capture:
    """dxcam (GPU, fast) if it works, otherwise mss. grab() returns a PIL image, or None if nothing changed."""

    def __init__(self):
        self.cam = None
        self.sct = None
        self.name = ""
        self.err = "" if dxcam is not None else "dxcam is not installed (pip install dxcam opencv-python)"
        if dxcam is not None:
            try:
                self.cam = dxcam.create(output_idx=0, output_color="RGB")
                self.name = "dxcam"
            except Exception as e:
                self.cam = None
                self.err = "dxcam failed: %s" % e
        if self.cam is None:
            self._use_mss()

    def _use_mss(self):
        if mss is None:
            raise RuntimeError("No screen capture available. Install:  pip install mss")
        mss_class = getattr(mss, "MSS", None) or mss.mss
        self.sct = mss_class()
        self.mon = self.sct.monitors[1]
        self.cam = None
        self.name = "mss"

    def grab(self):
        if self.cam is not None:
            try:
                frame = self.cam.grab()
            except Exception as e:
                self.err = "dxcam failed (%s) - using mss. Try: pip install opencv-python" % e
                self._use_mss()
                return self.grab()
            if frame is None:
                return None
            return Image.fromarray(frame)
        shot = self.sct.grab(self.mon)
        return Image.frombytes("RGB", shot.size, shot.bgra, "raw", "BGRX")


def get_capture():
    global _capture
    if _capture is None:
        _capture = Capture()
    return _capture

def encode_frame(img, width, height, quality, pool):
    if img.size != (width, height):
        img = img.resize((width, height), Image.BILINEAR)

    th = ((height + TILES - 1) // TILES + 15) // 16 * 16
    jobs, y = [], 0
    while y < height:
        h = min(th, height - y)
        jobs.append((y, h))
        y += h

    def enc(job):
        y0, h0 = job
        buf = io.BytesIO()
        img.crop((0, y0, width, y0 + h0)).save(buf, "JPEG", quality=quality, subsampling=2)
        return y0, h0, buf.getvalue()

    parts = list(pool.map(enc, jobs))
    out = bytearray(struct.pack(">BHH", len(parts), width, height))
    for y0, h0, data in parts:
        out += struct.pack(">HHI", y0, h0, len(data))
        out += data
    return bytes(out)

class AudioStreamer(threading.Thread):
    def __init__(self, ip, port, stop_evt):
        super().__init__(daemon=True)
        self.ip, self.port, self.parent_stop = ip, port, stop_evt
        self.local_stop = threading.Event()
        self.q = queue.Queue(maxsize=200)
        self.error = ""

    def run(self):
        if pyaudio is None:
            self.error = "PyAudioWPatch not installed"
            return
        pa = None
        stream = None
        sock = None
        try:
            sock = socket.create_connection((self.ip, self.port), timeout=5)
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            sock.settimeout(10)

            pa = pyaudio.PyAudio()
            wasapi = pa.get_host_api_info_by_type(pyaudio.paWASAPI)
            dev = pa.get_device_info_by_index(wasapi["defaultOutputDevice"])
            if not dev.get("isLoopbackDevice"):
                for lb in pa.get_loopback_device_info_generator():
                    if dev["name"] in lb["name"]:
                        dev = lb
                        break
            rate = int(dev["defaultSampleRate"])
            ch = int(dev["maxInputChannels"])

            def cb(in_data, frame_count, time_info, status):
                try:
                    self.q.put_nowait(in_data)
                except queue.Full:
                    pass
                return (None, pyaudio.paContinue)

            stream = pa.open(format=pyaudio.paInt16, channels=ch, rate=rate, input=True,
                             input_device_index=dev["index"], frames_per_buffer=1024,
                             stream_callback=cb)
            stream.start_stream()

            acc = bytearray()
            interval = AUDIO_GRAIN / float(AUDIO_RATE)
            next_t = time.perf_counter()
            silence = bytes(AUDIO_BYTES)
            while not self.parent_stop.is_set() and not self.local_stop.is_set():
                try:
                    while True:
                        raw = self.q.get_nowait()
                        acc += self.convert(raw, rate, ch)
                except queue.Empty:
                    pass
                if len(acc) > AUDIO_BYTES * 6:
                    del acc[:len(acc) - AUDIO_BYTES * 3]

                if len(acc) >= AUDIO_BYTES:
                    chunk = bytes(acc[:AUDIO_BYTES])
                    del acc[:AUDIO_BYTES]
                else:
                    chunk = silence
                sock.sendall(chunk)

                next_t += interval
                delay = next_t - time.perf_counter()
                if delay > 0:
                    time.sleep(delay)
                elif delay < -0.5:
                    next_t = time.perf_counter()
        except Exception as e:
            self.error = str(e)
        finally:
            try:
                if stream:
                    stream.stop_stream()
                    stream.close()
                if pa:
                    pa.terminate()
            except Exception:
                pass
            if sock:
                sock.close()

    @staticmethod
    def convert(raw, rate, ch):
        a = np.frombuffer(raw, dtype=np.int16)
        if ch > 1:
            a = a.reshape(-1, ch)[:, :2]
        else:
            a = np.repeat(a.reshape(-1, 1), 2, axis=1)
        if rate != AUDIO_RATE:
            n = a.shape[0]
            m = int(n * AUDIO_RATE / rate)
            x = np.linspace(0, n - 1, m)
            a = np.stack([np.interp(x, np.arange(n), a[:, i]) for i in range(2)], axis=1).astype(np.int16)
        return a.astype("<i2").tobytes()

class Streamer(threading.Thread):
    def __init__(self, settings):
        super().__init__(daemon=True)
        self.settings = settings
        self.stop_evt = threading.Event()
        self.status = "Starting..."
        self.state = "searching" 

    def set_status(self, text, state="searching"):
        self.status = text
        self.state = state

    def run(self):
        while not self.stop_evt.is_set():
            manual = self.settings.get("manual_ip", "").strip()
            if manual:
                target = (manual, STREAM_PORT, AUDIO_PORT)
            else:
                self.set_status("Searching for your PS4...  (open the app and choose PC)")
                target = find_ps4(self.stop_evt)
                if not target:
                    continue

            ip, vport, aport = target
            self.set_status("Connecting to %s ..." % ip)
            try:
                sock = socket.create_connection((ip, vport), timeout=5)
            except OSError:
                self.set_status("Could not connect, retrying...", "error")
                self.stop_evt.wait(1.5)
                continue

            sock.settimeout(10)
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 256 * 1024)   # small queue = low delay

            audio = None
            if self.settings.get("audio") and pyaudio is not None:
                audio = AudioStreamer(ip, aport, self.stop_evt)
                audio.start()

            try:
                self.stream(sock, ip, audio)
            except OSError:
                pass
            except Exception as e:                    # show unexpected errors instead of dying silently
                self.set_status("Error: %s" % e, "error")
                self.stop_evt.wait(3)
            finally:
                sock.close()

            if audio:
                audio.local_stop.set()
            if not self.stop_evt.is_set():
                self.set_status("PS4 disconnected. Searching again...", "error")
                self.stop_evt.wait(1.0)
        self.set_status("Stopped")

    def stream(self, sock, ip, audio):
        cap = get_capture()
        self.set_status("Capture: %s%s" % (cap.name, ("  (%s)" % cap.err) if cap.err else ""), "connected")
        pool = ThreadPoolExecutor(max_workers=TILES)
        frames, t0 = 0, time.time()
        last_send = time.time()
        bytes_sent = 0
        eff_q = self.settings["quality"]      # quality actually used (auto-lowered when the link is slow)
        try:
            while not self.stop_evt.is_set():
                start = time.time()
                st = self.settings
                img = cap.grab()
                if img is None:                                  # screen did not change
                    if time.time() - last_send > 1.0:
                        sock.sendall(struct.pack(">BHH", 0, 0, 0))   # keep-alive
                        last_send = time.time()
                    time.sleep(0.002)
                    continue

                width = min(st["width"], img.width)
                height = int(round(width * img.height / img.width / 2)) * 2
                eff_q = min(eff_q, st["quality"])
                data = encode_frame(img, width, height, eff_q, pool)
                t_s = time.time()
                sock.sendall(data)
                send_time = time.time() - t_s
                last_send = time.time()
                bytes_sent += len(data)

                if st.get("auto", True):                       # link too slow -> lower quality, link free -> raise it back
                    budget = 1.0 / st["fps"]
                    if send_time > budget and eff_q > 50:
                        eff_q -= 4
                    elif send_time < budget * 0.4 and eff_q < st["quality"]:
                        eff_q += 1

                frames += 1
                if time.time() - t0 >= 1.0:
                    extra = ""
                    if audio is not None:
                        extra = "   |   sound: " + ("on" if not audio.error else "error (%s)" % audio.error[:40])
                    el = max(time.time() - t0, 0.001)
                    mbps = bytes_sent * 8 / el / 1e6
                    kb = bytes_sent / max(frames, 1) / 1024
                    self.set_status("Streaming to %s - %d FPS | %.1f Mbps | %d KB/frame | q%d%s"
                                    % (ip, frames, mbps, kb, eff_q, extra), "connected")
                    frames, bytes_sent, t0 = 0, 0, time.time()

                delay = 1.0 / st["fps"] - (time.time() - start)
                if delay > 0:
                    time.sleep(delay)
        finally:
            pool.shutdown(wait=False)

    def stop(self):
        self.stop_evt.set()

class App:
    COLORS = {"searching": "#d89a1e", "connected": "#2ca064", "error": "#d9534f", "idle": "#777777"}

    def __init__(self, root):
        self.root = root
        root.title("The Screen Mirror")
        root.geometry("480x470")
        root.resizable(False, False)

        self.settings = {"quality": 60, "fps": 30, "width": 1280, "auto": True, "manual_ip": "", "audio": pyaudio is not None}
        self.streamer = None

        main = ttk.Frame(root, padding=18)
        main.pack(fill="both", expand=True)

        ttk.Label(main, text="PS4 Screen Mirror", font=("Segoe UI", 18, "bold")).pack(anchor="w")
        ttk.Label(main, text="Open the app on your PS4, choose PC, then press Start.").pack(anchor="w", pady=(2, 12))

        status_row = ttk.Frame(main)
        status_row.pack(fill="x", pady=(0, 12))
        self.dot = tk.Canvas(status_row, width=16, height=16, highlightthickness=0)
        self.dot.pack(side="left")
        self.dot_item = self.dot.create_oval(2, 2, 14, 14, fill=self.COLORS["idle"], outline="")
        self.status_var = tk.StringVar(value="Not running")
        ttk.Label(status_row, textvariable=self.status_var, wraplength=400).pack(side="left", padx=8)

        self.btn = ttk.Button(main, text="Start", command=self.toggle)
        self.btn.pack(fill="x", ipady=6)

        box = ttk.LabelFrame(main, text="Settings", padding=12)
        box.pack(fill="x", pady=14)

        ttk.Label(box, text="Quality").grid(row=0, column=0, sticky="w")
        self.q_label = ttk.Label(box, text="60")
        self.q_label.grid(row=0, column=2, padx=6)
        q = ttk.Scale(box, from_=30, to=95, orient="horizontal", length=220, command=self.on_quality)
        q.set(60)
        q.grid(row=0, column=1, padx=8, pady=3)

        ttk.Label(box, text="Max FPS").grid(row=1, column=0, sticky="w")
        self.f_label = ttk.Label(box, text="30")
        self.f_label.grid(row=1, column=2, padx=6)
        f = ttk.Scale(box, from_=10, to=60, orient="horizontal", length=220, command=self.on_fps)
        f.set(30)
        f.grid(row=1, column=1, padx=8, pady=3)

        ttk.Label(box, text="Resolution").grid(row=2, column=0, sticky="w")
        self.res = ttk.Combobox(box, state="readonly", width=26,
                                values=["960 px wide (fastest)", "1280 px wide (fast)",
                                        "1600 px wide", "1920 px wide (sharpest)"])
        self.res.current(1)
        self.res.bind("<<ComboboxSelected>>", self.on_res)
        self.res.grid(row=2, column=1, padx=8, pady=3, sticky="w")

        self.audio_var = tk.BooleanVar(value=self.settings["audio"])
        self.audio_chk = ttk.Checkbutton(box, text="Stream audio (PC sound to PS4)", variable=self.audio_var,
                                         command=self.on_audio)
        self.audio_chk.grid(row=3, column=0, columnspan=3, sticky="w", pady=(8, 2))
        if pyaudio is None:
            self.audio_chk.state(["disabled"])
            self.audio_var.set(False)
            ttk.Label(box, text="Install PyAudioWPatch to enable audio:  pip install PyAudioWPatch",
                      foreground="#b55").grid(row=4, column=0, columnspan=3, sticky="w")

        ttk.Label(box, text="PS4 IP").grid(row=5, column=0, sticky="w", pady=(8, 0))
        self.ip_entry = ttk.Entry(box, width=28)
        self.ip_entry.grid(row=5, column=1, padx=8, pady=(8, 0), sticky="w")
        ttk.Label(box, text="optional", foreground="#888").grid(row=5, column=2, pady=(8, 0))

        cap = "dxcam (fast)" if dxcam is not None else "mss (slower - install dxcam for more FPS)"
        ttk.Label(main, text="Capture: " + cap + "\nIf the PS4 isn't found: allow this app in Windows Firewall (Private networks)\nor type the PS4 IP above.",
                  foreground="#666").pack(anchor="w")

        root.protocol("WM_DELETE_WINDOW", self.close)
        self.tick()

    def on_quality(self, v):
        self.settings["quality"] = int(float(v))
        self.q_label.config(text=str(self.settings["quality"]))

    def on_fps(self, v):
        self.settings["fps"] = int(float(v))
        self.f_label.config(text=str(self.settings["fps"]))

    def on_res(self, _e=None):
        self.settings["width"] = [960, 1280, 1600, 1920][self.res.current()]

    def on_audio(self):
        self.settings["audio"] = bool(self.audio_var.get())      # applies at the next connection

    def toggle(self):
        if self.streamer and self.streamer.is_alive():
            self.streamer.stop()
            self.streamer = None
            self.btn.config(text="Start")
            self.status_var.set("Not running")
            self.dot.itemconfig(self.dot_item, fill=self.COLORS["idle"])
        else:
            self.settings["manual_ip"] = self.ip_entry.get()
            self.settings["audio"] = bool(self.audio_var.get())
            self.streamer = Streamer(self.settings)
            self.streamer.start()
            self.btn.config(text="Stop")

    def tick(self):
        if self.streamer and self.streamer.is_alive():
            self.status_var.set(self.streamer.status)
            self.dot.itemconfig(self.dot_item, fill=self.COLORS.get(self.streamer.state, "#777"))
        self.root.after(250, self.tick)

    def close(self):
        if self.streamer:
            self.streamer.stop()
        self.root.destroy()


if __name__ == "__main__":
    root = tk.Tk()
    App(root)
    root.mainloop()
