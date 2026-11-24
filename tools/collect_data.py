#!/usr/bin/env python3
# =====================================================================
# collect_data.py -- pure serial data collection, no inference.
#
#   python3 tools/collect_data.py                 # autodetect port, campaign below
#   python3 tools/collect_data.py /dev/ttyACM1    # explicit port
#   python3 tools/collect_data.py --n 200         # samples per condition (default 150)
#   python3 tools/collect_data.py --idle          # factory-reset first: the node
#                                                 # stays IDLE, the stream is pure
#                                                 # telemetry (no model behavior)
#
# Edit CAMPAIGN per collection run. The script:
#   - prompts you to set each condition ("set the motor to X, press
#     Enter when running"),
#   - counts N [RAW] telemetry lines (one every 2 s = the model's
#     feature vector),
#   - marks the log and moves to the next condition until the campaign
#     is done. Nothing is sent to the node except the optional reset;
#     nothing is judged.
#
# Outputs in data/serial/:
#   collect_<ts>.log   byte-faithful stream + condition markers
#   collect_<ts>.csv   one row per sample: wall_hms, condition label,
#                      then the feature values in model order. That is
#                      the whole model-testing dataset - no decision
#                      columns (with --idle the node infers nothing).
#                      The full serial stream stays in the .log.
# =====================================================================
import os
import re
import sys
import threading
import time

from rig_tour import OUTDIR, find_port, open_port, now_hms, prompt

# the feature blob of a [RAW] line: everything between ts_ms= and state=
# (works for any node type - the names come from the node's profile)
RX_FEATS = re.compile(r"\[RAW\] ts_ms=\d+ (.+?) state=")

# the campaign: (condition label, samples) - edit per run. 150 samples
# = 5 min at the node's 2 s telemetry cadence.
CAMPAIGN = [
    ("op_stopped", 150),
    ("speed_1", 150),
    ("speed_2", 150),
    ("speed_1_phase_loss", 150),
    ("speed_2_phase_loss", 150),
]


class Collector:
    """Reader thread: byte-faithful log + the model-testing CSV +
    sample count. All inference-related columns are dropped here: the
    CSV is features + the campaign's ground-truth label, nothing else."""

    def __init__(self, fd, log_path, csv_path):
        self.fd = fd
        self.buf = b""
        self.lock = threading.Lock()
        self.done = threading.Event()
        self.logf = open(log_path, "a")
        self.csv_path = csv_path
        self.csvf = None
        self.csv_cols = None
        self.collecting = None              # current campaign label or None
        self.count = 0
        self.idle_ack = None                # set on '[FS] back to IDLE'

    def log(self, line):
        with self.lock:
            self.logf.write(f"[{now_hms()}] {line}\n")
            self.logf.flush()

    def send(self, cmd):
        self.log(f">>> send: {cmd}")
        os.write(self.fd, (cmd + "\n").encode())

    def csv_row(self, line):
        m = RX_FEATS.search(line)
        if not m:
            return
        pairs = [p.split("=", 1) for p in m.group(1).split() if "=" in p]
        names = [k for k, _ in pairs]
        vals = [v for _, v in pairs]
        if self.csvf is None:
            self.csvf = open(self.csv_path, "w")
            self.csv_cols = names
            self.csvf.write("wall_hms,label," + ",".join(names) + "\n")
        elif names != self.csv_cols:
            return                          # feature set changed: keep stable
        self.csvf.write(f"{now_hms()},{self.collecting or ''},"
                        + ",".join(vals) + "\n")
        self.csvf.flush()

    def handle(self, line):
        self.log(line)
        if "[FS] back to IDLE" in line:
            self.idle_ack = True
        if line.startswith("[RAW]"):
            self.csv_row(line)
            if self.collecting:
                self.count += 1

    def reader(self):
        while not self.done.is_set():
            try:
                data = os.read(self.fd, 4096)
            except (BlockingIOError, OSError):
                import select
                select.select([self.fd], [], [], 0.1)
                continue
            if not data:
                continue
            self.buf += data
            while b"\n" in self.buf:
                raw, self.buf = self.buf.split(b"\n", 1)
                line = raw.decode(errors="replace").rstrip("\r").strip()
                if line:
                    self.handle(line)


def main():
    args = sys.argv[1:]
    idle = "--idle" in args
    n_override = None
    if "--n" in args:
        n_override = int(args[args.index("--n") + 1])

    port = find_port()
    os.makedirs(OUTDIR, exist_ok=True)
    stamp = time.strftime("%Y%m%d_%H%M%S")
    log_path = os.path.join(OUTDIR, f"collect_{stamp}.log")
    csv_path = os.path.join(OUTDIR, f"collect_{stamp}.csv")
    print(f"== collect: {port} -> {log_path}")
    print("== campaign: " +
          ", ".join(f"{k}(n={n_override or v})" for k, v in CAMPAIGN))

    col = Collector(open_port(port), log_path, csv_path)
    threading.Thread(target=col.reader, daemon=True).start()

    if idle:
        print("--- sending reset: node IDLE, pure telemetry")
        col.send("reset")
        t0 = time.monotonic()
        while not col.idle_ack and time.monotonic() - t0 < 10:
            time.sleep(0.2)
        print("--- node IDLE" if col.idle_ack else
              "!!! no reset ack - continuing (check the log)")

    done_counts = []
    for label, cnt in CAMPAIGN:
        if n_override:
            cnt = n_override
        prompt(f"Set the motor to {label}. Confirm when running")
        with col.lock:
            col.collecting = label
            col.count = 0
        col.log(f"##### COLLECT {label}: start ({cnt} samples) #####")
        print(f"--- collecting {cnt} samples of {label} "
              f"(~{cnt * 2 // 60} min)")
        t0 = time.monotonic()
        last_report = -1
        while col.count < cnt:
            time.sleep(0.5)
            if col.count >= last_report + 20:
                last_report = col.count
                print(f"   ... {label}: {col.count}/{cnt} "
                      f"(T+{int(time.monotonic() - t0)}s)")
        with col.lock:
            col.collecting = None
        col.log(f"##### COLLECT {label}: end #####")
        done_counts.append((label, col.count))
        print(f"--- {label}: {col.count} samples collected")

    col.log("##### COLLECT END - summary #####")
    print("\n== COLLECT DONE ==")
    for label, c in done_counts:
        print(f"  {label:22s} {c} samples")
    print(f"  log: {log_path}")
    print(f"  csv: {csv_path}")
    col.done.set()
    time.sleep(0.3)


if __name__ == "__main__":
    main()
