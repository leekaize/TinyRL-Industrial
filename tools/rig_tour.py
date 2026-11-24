#!/usr/bin/env python3
# =====================================================================
# rig_tour.py -- scripted hardware tour: drives the operator, labels
# automatically, records everything with the reasoning attached.
#
#   python3 tools/rig_tour.py              # autodetect port, default plan
#   python3 tools/rig_tour.py /dev/ttyACM1 # explicit port
#   python3 tools/rig_tour.py --trace      # turn ON the node's decision
#                                          # dump ('d'); recorded around
#                                          # state changes only
#
# You only touch the MACHINE. The script:
#   - factory-resets the node at start (flash wipe: model, net,
#     library, pending batches) so every tour begins from scratch,
#   - prompts you at every state change ("set the motor to X, press
#     Enter when running"),
#   - watches the node's own [WHY]/[RL]/[EP]/[PEND] lines to decide when
#     a stage is DONE (never a fixed timer): a new state is complete when
#     the alarm cleared AND both bands matured; a revisit is complete
#     when the node matched the expected condition and stayed quiet,
#   - labels every ask by itself with the plan's slug/kind using the
#     batch id ('f P0001 <label> <kind>'),
#   - flags anything UNEXPECTED (an alarm on a known state, no alarm on
#     a new one, timeouts).
#
# Default plan (the 2026-11-21 abbreviated tour):
#   stopped -> speed_1 -> stopped -> speed_1 -> speed_1_phase_loss
#   -> speed_1 -> speed_1_phase_loss
#
# Outputs in data/serial/:
#   tour_<ts>.log  every [WHY]/[RL]/[RAW]/[EP] line + the [ST] decision
#                  dump kept only in a window around state changes +
#                  stage markers + final summary
#   tour_<ts>.csv  [RAW] telemetry columns (pandas-ready), rows labeled
#                  with the tour stage they were collected in
#
# No dependencies beyond Python 3: raw termios like seriallog.py (WSL /
# Linux; on Windows run inside WSL with the port attached via usbipd).
# =====================================================================
import glob
import os
import re
import select
import sys
import termios
import time
import threading
import tty

BAUD = 115200
OUTDIR = os.path.join(os.path.dirname(__file__), "..", "data", "serial")

# the abbreviated tour: (state, label, kind, new?)
#   new=True  -> expect ALARM + ask + auto-label + band maturity
#   new=False -> expect silent recognition of an existing condition
PLAN = [
    ("speed_1",            "speed_1",            "normal", True),
    ("stopped",            "op_stopped",         "normal", False),
    ("speed_1",            "speed_1",            "normal", False),
    ("speed_1_phase_loss", "speed_1_phase_loss", "fault",  True),
    ("speed_1",            "speed_1",            "normal", False),
    ("speed_1_phase_loss", "speed_1_phase_loss", "fault",  False),
]

SPECIAL = ("[WHY]", "[RL]", "[EP]", "[PEND]", "[FB]", "[LIB]",
           "[COND]", "[G100]", "[NODE]", "[FS]")

RX_MATCH = re.compile(r"\[WHY\] matched '([^']+)'")
RX_MATURE = re.compile(r"\[WHY\] '([^']+)' (score|positional) band MATURED")
RX_PEND = re.compile(r"\[PEND\] batch (\S+) stored")
RX_FBACK = re.compile(r"\[FB\] batch (\S+) labeled '([^']+)'")
RX_IDLE = re.compile(r"\[FS\] back to IDLE")
RX_TRACE = re.compile(r"\[NODE\] decision trace (ON|OFF)")
RX_RAWCOND = re.compile(r" cond=(\S+)")
RX_STATE = re.compile(r" state=(\S+)")
# generic [RAW] parse: ts_ms, the per-feature k=v blob (any node type),
# then the decision columns. Feature names come from the node's profile.
RX_RAW = re.compile(r"\[RAW\] ts_ms=(\d+) (.+?) state=(\S+) cond=(\S+) "
                    r"conf=(\S+) score=(\S+) theta=(\S+)")


class RawCsv:
    """[RAW] telemetry -> pandas-ready CSV. The header is written from the
    first parsed row, so the feature columns match whatever node is on the
    port (G100 fset/v/i/pf, vib bands, ...). Rows are labeled by the caller
    (tour stage / campaign condition); an empty label marks rows collected
    outside a labeled window (transitions, prompts)."""

    def __init__(self, path):
        self.path = path
        self.f = None
        self.cols = None

    def write(self, wall_hms, label, line):
        m = RX_RAW.search(line)
        if not m:
            return
        ts_ms, feats, state, cond, conf, score, theta = m.groups()
        pairs = [p.split("=", 1) for p in feats.split() if "=" in p]
        names = [k for k, _ in pairs]
        vals = [v for _, v in pairs]
        if self.f is None:
            self.f = open(self.path, "w")
            self.cols = names
            self.f.write("wall_hms,label,ts_ms," + ",".join(names) +
                         ",state,cond,conf,score,theta\n")
        elif names != self.cols:
            return                     # feature set changed: keep columns stable
        self.f.write(f"{wall_hms},{label},{ts_ms}," + ",".join(vals) +
                     f",{state},{cond},{conf},{score},{theta}\n")
        self.f.flush()

    def close(self):
        if self.f:
            self.f.close()
            self.f = None


def find_port():
    if len(sys.argv) > 1 and not sys.argv[1].startswith("--"):
        return sys.argv[1]
    for pat in ("/dev/ttyACM*", "/dev/ttyUSB*"):
        hits = sorted(glob.glob(pat))
        if hits:
            if len(hits) > 1:
                print("several ports found:", ", ".join(hits))
                print("pick one: python3 tools/rig_tour.py <port>")
                sys.exit(1)
            return hits[0]
    print("no serial port found (looked for /dev/ttyACM*, /dev/ttyUSB*)")
    sys.exit(1)


def open_port(port):
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        tty.setraw(fd)
        a = termios.tcgetattr(fd)
        speed = getattr(termios, "B115200")
        a[0] = a[1] = 0
        a[2] = a[2] | termios.CS8
        a[2] = a[2] & ~(termios.CRTSCTS | termios.CSTOPB | termios.PARENB)
        a[3] = 0
        a[4] = speed
        a[5] = speed
        termios.tcsetattr(fd, termios.TCSANOW, a)
    except termios.error:
        pass                                  # USB CDC ignores line settings
    return fd


def now_hms():
    return time.strftime("%H:%M:%S")


class Rig:
    """Reader thread + event store + log file. The stage engine polls
    the event store; the thread never blocks it."""

    def __init__(self, fd, trace):
        self.fd = fd
        self.buf = b""
        self.lock = threading.Lock()
        self.done = threading.Event()
        stamp = time.strftime("%Y%m%d_%H%M%S")
        self.log_path = os.path.join(OUTDIR, f"tour_{stamp}.log")
        self.csv_path = os.path.join(OUTDIR, f"tour_{stamp}.csv")
        self.trace = trace
        self.ev = {"alarm": [], "clear": [], "pend": [], "labeled": [],
                   "matured_score": [], "matured_pos": [], "matched": [],
                   "idle": [], "trace": []}
        self.raw_cond = None
        self.raw_cond_hits = 0
        self.raw_state = ""
        self.n_raw = 0
        self.ring = []            # [ST] lines pending an event flush
        self.trace_until = 0.0    # log [ST] live until t (post-event window)
        self.logf = None
        self.raw_csv = RawCsv(self.csv_path)
        self.stage_label = ""     # CSV label: the tour stage being collected

    def log(self, line):
        with self.lock:
            if self.logf:
                self.logf.write(f"[{now_hms()}] {line}\n")
                self.logf.flush()

    def send(self, cmd):
        self.log(f">>> send: {cmd}")
        os.write(self.fd, (cmd + "\n").encode())

    def classify(self, line):
        m = RX_MATCH.search(line)
        if m:
            self.ev["matched"].append((time.monotonic(), m.group(1)))
            return True
        m = RX_MATURE.search(line)
        if m:
            key = "matured_score" if m.group(2) == "score" else "matured_pos"
            self.ev[key].append((time.monotonic(), m.group(1)))
            return True
        m = RX_PEND.search(line)
        if m:
            self.ev["pend"].append((time.monotonic(), m.group(1)))
            return True
        m = RX_FBACK.search(line)
        if m:
            self.ev["labeled"].append((time.monotonic(), m.group(1)))
            return True
        m = RX_IDLE.search(line)
        if m:
            self.ev["idle"].append((time.monotonic(), line))
            return True
        m = RX_TRACE.search(line)
        if m:
            self.ev["trace"].append((time.monotonic(), m.group(1)))
            return True
        if "[RL] ALARM" in line:
            self.ev["alarm"].append((time.monotonic(), line))
            return True
        if "[RL] clear" in line:
            self.ev["clear"].append((time.monotonic(), line))
            return True
        if line.startswith("[RAW]"):
            m = RX_STATE.search(line)
            if m:
                self.raw_state = m.group(1)
            m = RX_RAWCOND.search(line)
            if m:
                if m.group(1) == self.raw_cond:
                    self.raw_cond_hits += 1
                else:
                    self.raw_cond = m.group(1)
                    self.raw_cond_hits = 1
            self.n_raw += 1
            return self.n_raw % 10 == 0        # heartbeat, not every line
        return False

    def flush_ring(self):
        """Dump the pre-event [ST] context into the log, arrival times
        preserved."""
        if not self.ring:
            return
        self.log("---- [ST] trace context (before the event below) ----")
        with self.lock:
            for t, ln in self.ring:
                stamp = time.strftime("%H:%M:%S", time.localtime(t))
                self.logf.write(f"[{stamp}] {ln}\n")
            self.logf.flush()
        self.ring = []

    def handle(self, line):
        # [ST] dump lines are never console-printed. They are logged live
        # only in the 2 s window after an event; otherwise they sit in the
        # ring as pre-event context and get flushed by the next event.
        if line.startswith("[ST "):
            if time.monotonic() <= self.trace_until:
                self.log(line)
            else:
                self.ring.append((time.time(), line))
            return
        special = self.classify(line)
        show = special or line.startswith(SPECIAL)
        if show:
            if not line.startswith("[RAW]"):
                self.flush_ring()
                self.trace_until = time.monotonic() + 2.0
            print(f"{now_hms()} {line}")
        self.log(line)
        if line.startswith("[RAW]"):
            self.raw_csv.write(now_hms(), self.stage_label, line)

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
                line = raw.decode(errors="replace").rstrip("\r").strip()
                if line:
                    self.handle(line)

    def since(self, key, t0):
        return [e for e in self.ev[key] if e[0] >= t0]

    def wait_for(self, pred, what, timeout, progress=15):
        """pred() -> truthy payload or None. Polls the event store until
        pred fires or the deadline passes; prints a progress beat."""
        t0 = time.monotonic()
        last_beat = 0
        while True:
            got = pred()
            if got is not None:
                return got, time.monotonic() - t0
            el = time.monotonic() - t0
            if el > timeout:
                return None, el
            if el - last_beat >= progress and el >= 1:
                last_beat = el
                print(f"   ... still waiting: {what} (T+{int(el)}s)")
            time.sleep(0.5)


def prompt(msg):
    print(f"\n>>> {msg}")
    print(">>> press Enter when done ... ", end="", flush=True)
    sys.stdin.readline()
    print()


def wait_latched_alarm_clear(rig, why, timeout=90):
    """A latched alarm swallows the next [RL] ALARM rising edge, so any
    stage that expects an edge must start clean. Returns (cleared, wait)."""
    if rig.raw_state != "alarm":
        return True, 0.0
    print(f"!!! alarm already ACTIVE at stage entry ({why}) - waiting for"
          f" it to clear; do NOT change the machine yet")
    t0 = time.monotonic()
    got, dt = rig.wait_for(
        lambda: ([(time.monotonic(), "")] if rig.raw_state != "alarm"
                 else None),
        "alarm clear", timeout)
    return bool(got), dt


def stage_new(rig, idx, n, state, label, kind, summ):
    """A state the node has never seen: alarm -> ask -> auto-label ->
    alarm cleared -> both bands matured."""
    rig.stage_label = ""                     # CSV: prompt rows are unlabeled
    t0 = time.monotonic()
    rig.log(f"##### STAGE {idx}/{n}: {state} (NEW, expect label "
            f"'{label}' {kind}) #####")
    print(f"--- STAGE {idx}/{n}: change the motor to {state} (NEW state)")
    print(f"--- expect: ALARM -> ask -> auto-label '{label}' ({kind})")
    ok, dt = wait_latched_alarm_clear(
        rig, "a NEW state must earn its own ALARM edge")
    if not ok:
        print(f"!!! alarm never cleared in {int(dt)}s - skipping this"
              f" stage: the next ALARM edge cannot fire while the old one"
              f" is latched, and labeling the open batch now would"
              f" mislabel the transition noise")
        summ.append((idx, state, "NEW", f"skipped: alarm latched {int(dt)}s",
                     "FAIL"))
        return
    prompt("Change the machine state NOW, then confirm")
    rig.stage_label = state                 # CSV rows: this stage's data

    got, dt = rig.wait_for(lambda: rig.since("alarm", t0) or None,
                           f"ALARM on the new state {state}", 120)
    if not got:
        print(f"!!! NO ALARM within {int(dt)}s on a NEW state {state} - "
              f"was the state actually changed? Recording and moving on.")
        summ.append((idx, state, "NEW", f"NO ALARM in {int(dt)}s", "FAIL"))
        return
    t_alarm = got[0][0]
    print(f"--- alarm {int(dt)}s after stage start (acc evidence crossed 5)")

    bid, dt = rig.wait_for(
        lambda: rig.since("pend", t_alarm) or None,
        "ask (batch stored after 15 settled samples)", 90)
    if not bid:
        print(f"!!! NO ASK within {int(dt)}s - alarm may have cleared"
              f" early; falling back to a live-episode label")
        rig.send(f"f {label} {kind}")
        summ.append((idx, state, "NEW", "labeled live episode (no ask)",
                     "WARN"))
    else:
        b_id = bid[0][1]
        rig.send(f"f {b_id} {label} {kind}")
        ack, dt = rig.wait_for(
            lambda: rig.since("labeled", t_alarm) or None,
            f"label ack for batch {b_id}", 15)
        print(f"--- labeled batch {b_id} -> '{label}' ({kind})"
              + (f" (ack {int(dt)}s)" if ack else " (NO ACK - check [FB])"))

    got, dt = rig.wait_for(lambda: rig.since("clear", t_alarm) or None,
                           "alarm clear (grace flush below ACC_CLEAR)", 150)
    print("--- alarm cleared " +
          (f"{int(got[0][0] - t_alarm)}s after the alarm" if got
           else f"NOT within {int(dt)}s (check the flush)") + ".")

    got_s, _ = rig.wait_for(
        lambda: [e for e in rig.since("matured_score", t_alarm)
                 if e[1] == label] or None,
        f"'{label}' score band maturity (16 samples)", 300)
    got_p, _ = rig.wait_for(
        lambda: [e for e in rig.since("matured_pos", t_alarm)
                 if e[1] == label] or None,
        f"'{label}' positional band maturity (16 samples)", 300)
    t_mat = (got_s or got_p or [(time.monotonic(),)])[0][0]
    if got_s and got_p:
        print(f"--- stage complete: both bands matured "
              f"({int(t_mat - t_alarm)}s after the alarm)")
        summ.append((idx, state, "NEW",
                     f"alarm+{int(t_alarm - t0)}s, clear+"
                     f"{int((got[0][0] if got else t_alarm) - t_alarm)}s, "
                     f"matured+{int(t_mat - t_alarm)}s", "OK"))
    else:
        miss = "score" if not got_s else "positional"
        print(f"!!! {miss} band did NOT mature within 300s - the state may"
              f" need a longer dwell next time")
        summ.append((idx, state, "NEW", f"{miss} band not matured in 300s",
                     "WARN"))


def stage_known(rig, idx, n, state, expect, summ):
    """A known condition: silent recognition, no ask, no alarm."""
    rig.stage_label = ""
    t0 = time.monotonic()
    rig.log(f"##### STAGE {idx}/{n}: {state} (KNOWN, expect quiet match "
            f"'{expect}') #####")
    print(f"--- STAGE {idx}/{n}: change the motor to {state} (KNOWN state)")
    print(f"--- expect: quiet match '{expect}', NO ask, NO alarm")
    ok, dt = wait_latched_alarm_clear(
        rig, "a KNOWN state must start from a clean, quiet node")
    if not ok:
        print(f"!!! alarm never cleared in {int(dt)}s - skipping this"
              f" stage (the dwell would pass spuriously on a latch)")
        summ.append((idx, state, "KNOWN",
                     f"skipped: alarm latched {int(dt)}s", "FAIL"))
        return
    prompt("Change the machine state NOW, then confirm")
    rig.stage_label = state                 # CSV rows: this stage's data

    def matched_two():
        if (rig.raw_cond == expect and rig.raw_cond_hits >= 2):
            return [(time.monotonic(), expect)]
        return None

    got, dt = rig.wait_for(matched_two,
                           f"RAW cond={expect} twice (quiet recognition)",
                           90)
    if got:
        print(f"--- recognized as '{expect}' {int(dt)}s after stage start"
              " (no ask, no alarm)")
    else:
        print(f"!!! NOT recognized within {int(dt)}s - cond stayed"
              f" '{rig.raw_cond}'. Recording and moving on.")
        summ.append((idx, state, "KNOWN", f"no match in {int(dt)}s", "FAIL"))
        return

    a0 = len(rig.ev["alarm"])
    time.sleep(45)                            # dwell: watch for surprises
    surprise = rig.since("alarm", t0)
    if len(rig.ev["alarm"]) > a0:
        print(f"!!! UNEXPECTED ALARM on the KNOWN state {state}: "
              f"{surprise[-1][1] if surprise else ''}")
        summ.append((idx, state, "KNOWN",
                     "UNEXPECTED ALARM on a known state", "FAIL"))
    else:
        print("--- dwell done: 45s quiet, no alarm, no ask")
        summ.append((idx, state, "KNOWN",
                     f"recognized +{int(dt)}s, 45s dwell quiet", "OK"))


def wipe(rig, summ):
    """Factory reset at script start: clears model, net, library and
    pending batches from flash (persist::factoryReset + PENDING_FILE).
    Waits on the node's own '[FS] back to IDLE' ack, never a timer."""
    rig.log("##### STAGE 0a: factory reset (flash wipe) #####")
    print("--- wiping the node (reset): model + net + library + pending")
    t0 = time.monotonic()
    rig.send("reset")
    got, dt = rig.wait_for(lambda: rig.since("idle", t0) or None,
                           "'[FS] back to IDLE' ack", 10)
    if got:
        print(f"--- flash wiped, node IDLE ({int(dt)}s)")
        summ.append((0, "wipe", "RESET", f"flash cleared in {int(dt)}s",
                     "OK"))
    else:
        print(f"!!! no reset ack within {int(dt)}s - wrong port? Node not"
              f" answering. Continuing (commissioning will tell more).")
        summ.append((0, "wipe", "RESET", "NO ACK - check port/baud",
                     "FAIL"))


def ensure_trace_on(rig):
    """'d' is a toggle and the node boots with the dump off (factoryReset
    does not clear it, so a leftover ON needs one flip). Send until the
    ack says ON, at most twice."""
    for _ in range(2):
        t0 = time.monotonic()
        rig.send("d")
        got, _ = rig.wait_for(
            lambda: ([l for l in rig.since("trace", t0) if l[1] == "ON"]
                     or None),
            "decision trace ON ack", 5)
        if got:
            print("--- decision trace ON: [ST] dump logged around state changes")
            return
    print("!!! could not confirm the decision trace ack - continuing"
          " (the [ST] dump may be missing from the log)")


def commission(rig, summ):
    """Baseline at stopped: start learning, wait for theta + op_stopped
    (the flash wipe happened in wipe() at script start)."""
    rig.stage_label = ""
    rig.log("##### STAGE 0b: commissioning at STOPPED #####")
    print("--- STAGE 0b: commissioning")
    prompt("Set the motor to STOPPED and let it sit. Confirm when ready")
    print("--- sending start ... keep the machine STOPPED")
    rig.stage_label = "commission"          # CSV rows: the baseline window
    rig.send("s")
    t0 = time.monotonic()
    got, dt = rig.wait_for(
        lambda: ([l for l in rig.ev["matched"] if l[1] == "op_stopped"]
                 or None) if rig.n_raw else None,
        "theta fit + baseline 'op_stopped' (~70 s: 30 CAL + 30 FIT)", 240)
    if got:
        print(f"--- monitoring started, baseline registered ({int(dt)}s)")
        summ.append((0, "stopped", "COMMISSION", f"ready in {int(dt)}s",
                     "OK"))
    else:
        print(f"!!! commissioning not confirmed within {int(dt)}s -"
              f" continuing anyway (check the log)")
        summ.append((0, "stopped", "COMMISSION", "no baseline seen", "FAIL"))


def main():
    trace = "--trace" in sys.argv
    port = find_port()
    os.makedirs(OUTDIR, exist_ok=True)
    rig = Rig(open_port(port), trace)
    rig.logf = open(rig.log_path, "a")
    print(f"== rig tour: {port} @ {BAUD} -> {rig.log_path}")
    print(f"== plan: stopped | " +
          " -> ".join(s for s, _, _, _ in PLAN))
    threading.Thread(target=rig.reader, daemon=True).start()

    summ = []
    try:
        wipe(rig, summ)
        if trace:
            ensure_trace_on(rig)
        commission(rig, summ)
        for i, (state, label, kind, new) in enumerate(PLAN, 1):
            if new:
                stage_new(rig, i, len(PLAN), state, label, kind, summ)
            else:
                stage_known(rig, i, len(PLAN), state, label, summ)
    except (KeyboardInterrupt, SystemExit):
        print("\n-- interrupted")
    finally:
        rig.flush_ring()
        rig.raw_csv.close()
        rig.log("##### TOUR END - summary #####")
        rig.logf.write("\n--- SUMMARY ---\n")
        print("\n== TOUR SUMMARY ==")
        for idx, state, kind, note, verdict in summ:
            line = f"  stage {idx}: {state:20s} {kind:10s} {note}"
            print(line)
            rig.logf.write(f"{verdict:5s} {line}\n")
        rig.logf.write(f"--- verdicts: "
                       f"{sum(1 for s in summ if s[4] == 'OK')}/"
                       f"{len(summ)} OK ---\n")
        ok = sum(1 for s in summ if s[4] == "OK")
        print(f"== verdicts: {ok}/{len(summ)} OK  "
              f"-> full log: {rig.log_path}")
        rig.done.set()
        time.sleep(0.3)


if __name__ == "__main__":
    main()
