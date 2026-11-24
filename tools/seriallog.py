#!/usr/bin/env python3
# =====================================================================
# seriallog.py -- capture a node's serial session for offline analysis.
#
#   python3 tools/seriallog.py              # autodetect port
#   python3 tools/seriallog.py /dev/ttyACM0 # explicit port
#
# What you get (in data/serial/):
#   <dev>_<timestamp>.log  the raw serial stream, byte-faithful. This is
#                          the input for `bash tests/replay.sh`.
#   <dev>_<timestamp>.csv  the [RAW] telemetry lines as columns
#                          (wall time, ts_ms, features..., state, cond,
#                          conf, score, theta) - load with pandas and go.
#
# While it runs, everything you type goes to the node, so the usual
# console workflow (s, m, l, p, f <label> [normal|fault], reset) works
# unchanged. 'q' or Ctrl-C stops the capture.
#
# No dependencies: raw termios on the tty (WSL/Linux, the environment
# the nodes are flashed from).
# =====================================================================
import os
import glob
import sys
import time
import termios
import tty
import select
import threading

BAUD = 115200
OUTDIR = os.path.join(os.path.dirname(__file__), "..", "data", "serial")


def find_port():
    if len(sys.argv) > 1:
        return sys.argv[1]
    for pat in ("/dev/ttyACM*", "/dev/ttyUSB*", "/dev/tty.usb*"):
        hits = sorted(glob.glob(pat))
        if hits:
            if len(hits) > 1:
                print("several ports found:", ", ".join(hits))
                print("pick one: python3 tools/seriallog.py <port>")
                sys.exit(1)
            return hits[0]
    print("no serial port found (looked for /dev/ttyACM*, /dev/ttyUSB*)")
    print("plug the MCU in, or pass it explicitly: python3 tools/seriallog.py <port>")
    sys.exit(1)


def open_port(port):
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        tty.setraw(fd)
        a = termios.tcgetattr(fd)
        speed = getattr(termios, "B115200")
        a[0] = a[1] = 0                       # iflag, oflag: raw
        a[2] = a[2] | termios.CS8             # cflag: 8N1, no flow control
        a[2] = a[2] & ~(termios.CRTSCTS | termios.CSTOPB | termios.PARENB)
        a[3] = 0                              # lflag: raw
        a[4] = speed
        a[5] = speed
        termios.tcsetattr(fd, termios.TCSANOW, a)
    except termios.error:
        pass                                  # USB CDC ignores line settings
    return fd


class Capture:
    def __init__(self, port):
        self.port = port
        self.dev = port.replace("/dev/", "")
        self.hdr = None                       # parsed [HDR] line
        self.stamp = time.strftime("%Y%m%d_%H%M%S")
        self.log_path = os.path.join(OUTDIR, f"{self.dev}_{self.stamp}.log")
        self.csv_path = os.path.join(OUTDIR, f"{self.dev}_{self.stamp}.csv")
        self.rows = 0
        self.buf = b""
        self.lock = threading.Lock()
        self.fd = None
        self.done = threading.Event()

    def rename_to_dev(self):
        if self.hdr and self.hdr.get("dev") and self.hdr["dev"] != self.dev:
            dev = self.hdr["dev"]
            new_log = os.path.join(OUTDIR, f"{dev}_{self.stamp}.log")
            new_csv = os.path.join(OUTDIR, f"{dev}_{self.stamp}.csv")
            try:
                os.rename(self.log_path, new_log)
                if os.path.exists(self.csv_path):
                    os.rename(self.csv_path, new_csv)
                self.log_path, self.csv_path = new_log, new_csv
                self.dev = dev
            except OSError:
                pass

    def parse_hdr(self, line):
        kv = dict(t.split("=", 1) for t in line.split()[1:] if "=" in t)
        if "features" in kv:
            kv["feat_list"] = [f.split(":")[0] for f in kv["features"].split(",")]
        self.hdr = kv
        os.makedirs(OUTDIR, exist_ok=True)
        print(f"-- capture target: dev={kv.get('dev')} type={kv.get('type')} "
              f"features={kv.get('feat_list')}")
        self.rename_to_dev()

    def parse_raw(self, line):
        kv = {}
        for tok in line.split()[1:]:
            if "=" in tok:
                k, v = tok.split("=", 1)
                kv[k] = v
        if "ts_ms" not in kv or not self.hdr or "feat_list" not in self.hdr:
            return
        cols = [time.strftime("%H:%M:%S"), kv["ts_ms"]]
        cols += [kv.get(f, "") for f in self.hdr["feat_list"]]
        cols += [kv.get("state", ""), kv.get("cond", ""), kv.get("conf", ""),
                 kv.get("score", ""), kv.get("theta", "")]
        with self.lock:
            if not os.path.exists(self.csv_path):     # pandas-ready header
                with open(self.csv_path, "w") as f:
                    f.write("wall,ts_ms," + ",".join(self.hdr["feat_list"]) +
                            ",state,cond,conf,score,theta\n")
            with open(self.csv_path, "a") as f:
                f.write(",".join(cols) + "\n")
            self.rows += 1

    def handle_line(self, line):
        print(line)                           # live echo, same as a monitor
        with self.lock:
            with open(self.log_path, "a") as f:
                f.write(line + "\n")
        if line.startswith("[HDR]"):
            self.parse_hdr(line)
        elif line.startswith("[RAW]"):
            self.parse_raw(line)

    def reader(self):
        while not self.done.is_set():
            try:
                data = os.read(self.fd, 4096)
            except (BlockingIOError, OSError):
                select.select([self.fd], [], [], 0.1)
                continue
            if not data:
                continue
            self.buf += data
            while b"\n" in self.buf:
                raw, self.buf = self.buf.split(b"\n", 1)
                line = raw.decode(errors="replace").rstrip("\r")
                if line:
                    self.handle_line(line)

    def run(self):
        os.makedirs(OUTDIR, exist_ok=True)
        self.fd = open_port(self.port)
        print(f"-- capturing {self.port} @ {BAUD} -> {self.log_path}")
        print("-- type commands for the node ('s' = start learning, 'm' = status)")
        print("-- 'q' or Ctrl-C stops the capture")
        threading.Thread(target=self.reader, daemon=True).start()
        try:
            while True:
                s = sys.stdin.readline()
                if not s:                     # EOF (Ctrl-D)
                    break
                s = s.strip()
                if s == "q":
                    break
                if s:
                    os.write(self.fd, (s + "\n").encode())
        except (KeyboardInterrupt, OSError):
            pass
        finally:
            self.done.set()
            time.sleep(0.2)                   # let the reader flush
            os.close(self.fd)
            self.rename_to_dev()
            print(f"\n-- captured {self.rows} telemetry rows")
            print(f"-- raw log (replay input): {self.log_path}")
            if self.rows:
                print(f"-- csv (pandas-ready):     {self.csv_path}")


if __name__ == "__main__":
    Capture(find_port()).run()
