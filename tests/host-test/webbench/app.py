#!/usr/bin/env python3
"""Web bench server: N condition-monitor nodes in a browser.

Structure (the standard split - server, API, static frontend):

  webbench/app.py            this file: Bench driver + HTTP server
  webbench/static/           index.html + app.js + style.css (editable,
                             refreshed without restarting the server)
  webbench/plot_run.py       publication figures from an exported run
  interactive_bench.py       thin launcher (python3 tests/host-test/
                             interactive_bench.py)

Sources: today the data source is the scripted host bench (N REAL
SensorNodeApps on Arduino stubs, built from interactive_host.cpp). The
planned real-hardware source is USB serial: the node firmware will emit
the SAME line protocol (S/EV lines, one per node per step) over serial,
and SerialSource below replaces the subprocess - nothing else changes.
That firmware streaming mode is future work; --source serial fails with
a clear message today.

Run:
    python3 tests/host-test/interactive_bench.py [--nodes 2]
            [--profile g100|vib] [--steps 900] [--port 8765] [--ci]
            [--source bench|serial]

API (used by the page, also curl-able):
    GET  /state                full snapshot (series + asks + verdicts)
    GET  /debug                counters for stall diagnosis
    POST /label                {"node", "slug", "action": label|normal|fault,
                                "kind": normal|fault (label only)}
    POST /control              {"cmd": run|continue, "seconds": n}
    POST /machine              {"ev": START|STOP|SPEED1..3|IMBALANCE|
                                     LOOSE_MOUNT|PHASE_LOSS|NORMAL}
    POST /export               writes runs/run-<ts>.jsonl, returns the path
"""
import argparse
import json
import os
import queue
import re
import subprocess
import sys
import threading
import time
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
WEBBENCH = Path(__file__).resolve().parent
HOSTTEST = WEBBENCH.parent
STUBS = ROOT / "tests" / "stub_esp32"
TINYRL = ROOT / "TinyRL"
CORE = ROOT / "nodes" / "core"
STATIC = WEBBENCH / "static"
RUNS = WEBBENCH / "runs"
BIN = Path("/tmp/tinyrl_interactive")

FEATURES = {
    "vfd_g100": [("i", "A"), ("fset", "Hz"), ("vdc", "V"), ("tq", "%")],
    "vib_adxl": [("rms", "g"), ("b1", "g2"), ("b2", "g2"), ("b3", "g2"),
                 ("b4", "g2"), ("fmax", "Hz")],
}

SRC_FILES = [
    "stub_impl.cpp", "node_runtime.cpp", "uns_messaging.cpp", "console.cpp",
    "tinyrl_monitor.cpp", "condition_library.cpp", "episode.cpp",
    "persistence.cpp", "interactive_host.cpp",
]


def build() -> None:
    srcs = [STUBS / SRC_FILES[0]] + [CORE / f for f in SRC_FILES[1:8]] \
        + [HOSTTEST / SRC_FILES[8]]
    newest = max(os.path.getmtime(p) for p in srcs)
    if BIN.exists() and os.path.getmtime(BIN) >= newest:
        return
    inc = [f"-I{STUBS}", f"-I{TINYRL}/src", f"-I{TINYRL}/examples/stream_x/src",
           f"-I{CORE}"]
    cmd = ["g++", "-std=c++17", "-O2", "-DAG_EMBEDDED", *inc,
           *[str(p) for p in srcs], "-o", str(BIN)]
    print("building", BIN, "...")
    subprocess.run(cmd, check=True)


class Bench:
    """Drives the binary: pacing, parsing, ask/verdict/hold state."""

    def __init__(self, profile, steps: int, nodes: int, autoagent: bool):
        self.steps_total = steps
        self.n_nodes = nodes
        # no autoplay: sim time only moves when the user queues it (+5 s / +60 s)
        self.pace_left = 0            # sim seconds queued, not yet advanced
        self.pace_rate = 10.0         # sim seconds per wall second: visible
                                      # motion, faster than real time
        self._pace_carry = 0.0
        self._last_tick = time.time()
        self.sent = 0
        self.consumed = 0
        self.ask_serial = 0
        self.dismissed = -1
        self.verdict_count = 0
        self.done = False
        self.lock = threading.Lock()
        self.events = []
        self.rows = [[] for _ in range(nodes)]
        self.ask_serials = [-1] * nodes
        self.verdicts = [None] * nodes
        self.machine = "op_stopped"           # boot truth before META/M
        self.context = "op_stopped"           # setpoint the machine runs at
        self.marks = []                       # every machine event (step, name)
        self.node_types = None                # filled from META
        env = dict(os.environ, TINYRL_STEPS=str(steps), TINYRL_NODES=str(nodes))
        if profile:
            env["TINYRL_PROFILE"] = profile
        if autoagent:
            env["TINYRL_AUTOAGENT"] = "1"
        self.proc = subprocess.Popen(
            [str(BIN)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, bufsize=1, env=env)
        self.stdin = self.proc.stdin
        self.q = queue.Queue()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self) -> None:
        for line in self.proc.stdout:
            line = line.rstrip()
            if line.startswith("META "):
                self.q.put(("META", None, line))
            elif line == "READY":
                self.q.put(("EV", 0, line))
            elif line.startswith("M "):
                self.q.put(("MACHINE", None, line[2:].strip()))
            elif line.startswith("S "):
                t = line.split()
                row = {"node": int(t[1]), "step": int(t[2]), "phase": t[3],
                       "score": float(t[4]), "theta": float(t[5]),
                       "ep": int(t[6]), "cond": t[7], "lib": int(t[8]),
                       "feat": [float(v) for v in t[9:]]}
                self.q.put(("S", row["node"], row))
            elif line.startswith("VERDICT "):
                t = line.split(maxsplit=3)
                self.q.put(("VERDICT", int(t[1]),
                            {"verdict": t[2],
                             "text": t[3] if len(t) > 3 else ""}))
            elif line.startswith("DONE"):
                self.q.put(("EV", 10 ** 9, line))
            elif line:
                self.q.put(("EV", None, line))
        self.q.put(("EXIT", None, None))

    def pump(self) -> None:
        with self.lock:
            while True:
                try:
                    kind, node, payload = self.q.get_nowait()
                except queue.Empty:
                    break
                if kind == "S":
                    self.rows[node].append(payload)
                    if payload["ep"] == 1 and payload["cond"] == "unknown" \
                            and self.ask_serials[node] != self.ask_serial:
                        self.ask_serial += 1
                        self.ask_serials[node] = self.ask_serial
                elif kind == "VERDICT":
                    self.verdict_count += 1
                    v = dict(payload)
                    v["step"] = max((r["step"] for r in self.rows[node]),
                                    default=0)
                    self.verdicts[node] = v
                elif kind == "META":
                    t = payload.split()
                    self.steps_total = int(next(
                        x for x in t if x.startswith("steps=")).split("=")[1])
                    self.node_types = [x.split("=", 1)[1] for x in t
                                       if re.match(r"node\d+=", x)]
                elif kind == "MACHINE":
                    parts = payload.split()
                    self.machine = parts[0]
                    self.context = parts[1] if len(parts) > 1 else parts[0]
                    step = max((r["step"] for rows in self.rows
                                for r in rows), default=0)
                    self.marks.append([step, self.machine])
                elif kind == "EV":
                    step = max((r["step"] for rows in self.rows for r in rows),
                               default=0)
                    self.events.append((step, payload))
                else:
                    self.done = True
            self.consumed = sum(len(r) for r in self.rows) // max(1, self.n_nodes)

    def advance(self) -> None:
        if self.sent - self.consumed < 8:
            self.send("s")
            self.sent += 1

    def send(self, cmd: str) -> None:
        self.stdin.write(cmd + "\n")
        self.stdin.flush()

    def tick(self) -> None:
        with self.lock:
            if self.done:
                return
            now = time.time()
            dt = min(0.5, now - self._last_tick)
            self._last_tick = now
            ask_nodes = [i for i, rows in enumerate(self.rows)
                         if rows and rows[-1]["ep"] == 1
                         and rows[-1]["cond"] == "unknown"]
            holding = bool(ask_nodes) and self.ask_serial != self.dismissed
            if holding:          # an ask freezes sim time; drop missed time
                self._pace_carry = 0.0
                return
            if self.pace_left <= 0:
                return
            due = self.pace_rate * dt + self._pace_carry
            n = int(due)
            self._pace_carry = due - n
            if n > self.pace_left:
                n = self.pace_left
                self._pace_carry = 0.0
            for _ in range(n):
                self.advance()
            self.pace_left -= n

    def label(self, node: int, slug: str, action: str,
              kind: str = "fault") -> None:
        if action == "label":
            j = json.dumps({"action": "label", "label": slug, "kind": kind,
                            "by": "user"}, separators=(",", ":"))
        elif action == "fault":
            j = json.dumps({"action": "fault", "by": "user"},
                           separators=(",", ":"))
        else:
            j = json.dumps({"action": "normal", "by": "user"},
                           separators=(",", ":"))
        self.send(f"L {node} {slug} {j}")

    def export(self) -> str:
        RUNS.mkdir(exist_ok=True)
        ts = time.strftime("%Y%m%d-%H%M%S")
        out = RUNS / f"run-{ts}.jsonl"
        with self.lock:
            with open(out, "w") as f:
                meta = {"type": "meta", "steps_total": self.steps_total,
                        "node_types": list(self.node_types or []),
                        "feature_names": {t: [n for n, _ in FEATURES[t]]
                                          for t in self.node_types or []},
                        "feature_units": {t: [u for _, u in FEATURES[t]]
                                          for t in self.node_types or []}}
                f.write(json.dumps(meta) + "\n")
                for i, rows in enumerate(self.rows):
                    for r in rows:
                        f.write(json.dumps({"type": "S", "node": i, **r}) + "\n")
                for step, text in self.events:
                    f.write(json.dumps({"type": "EV", "step": step,
                                        "text": text}) + "\n")
        return str(out)

    def snapshot(self) -> dict:
        with self.lock:
            ask = []
            for i, rows in enumerate(self.rows):
                if rows and rows[-1]["ep"] == 1 \
                        and rows[-1]["cond"] == "unknown":
                    ask.append({"node": i, "cond": rows[-1]["cond"],
                                "step": rows[-1]["step"],
                                "verdict": self.verdicts[i]})
            nodes = []
            for i, rows in enumerate(self.rows):
                ntype = (self.node_types[i]
                         if self.node_types and i < len(self.node_types)
                         else "vfd_g100")
                feats = FEATURES.get(ntype, [])
                last = rows[-1] if rows else {"step": 0, "phase": "idle",
                                              "score": 0, "theta": 0, "ep": 0,
                                              "cond": "idle", "lib": 0,
                                              "feat": [0] * len(feats)}
                nodes.append({
                    "id": i, "node_type": ntype,
                    "feature_names": [n for n, _ in feats],
                    "feature_units": [u for _, u in feats],
                    "nf": len(feats),
                    "step": last["step"], "phase": last["phase"],
                    "score": last["score"], "theta": last["theta"],
                    "ep": last["ep"], "cond": last["cond"], "lib": last["lib"],
                    "feat": last["feat"],
                    "series": {
                        "steps": [r["step"] for r in rows],
                        "score": [r["score"] for r in rows],
                        "theta": [r["theta"] for r in rows],
                        "feat": [[r["feat"][k] for r in rows]
                                 for k in range(len(feats))],
                    },
                })
            return {
                "steps_total": self.steps_total,
                "machine": self.machine,
                "context": self.context,
                "marks": self.marks,
                "playing": self.pace_left > 0,
                "done": self.done,
                "ask": ask,
                "holding": bool(ask) and self.ask_serial != self.dismissed,
                "nodes": nodes,
                "events": [list(e) for e in self.events[-15:]],
            }


class Handler(SimpleHTTPRequestHandler):
    bench: Bench = None

    def __init__(self, *a, **kw):
        super().__init__(*a, directory=str(STATIC), **kw)

    def log_message(self, *a):        # quiet
        pass

    def end_headers(self):            # never cache the UI while iterating
        self.send_header("Cache-Control", "no-cache")
        super().end_headers()

    def _send(self, code: int, body: str, ctype: str) -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.end_headers()
        self.wfile.write(body.encode())

    def do_GET(self):
        if self.path == "/state":
            self._send(200, json.dumps(self.bench.snapshot()), "application/json")
        elif self.path == "/debug":
            b = self.bench
            with b.lock:
                dbg = {"sent": b.sent, "consumed": b.consumed,
                       "pace_left": b.pace_left, "pace_rate": b.pace_rate,
                       "done": b.done, "poll": b.proc.poll(),
                       "ask_serial": b.ask_serial, "dismissed": b.dismissed,
                       "row_lens": [len(r) for r in b.rows],
                       "last_rows": [{"step": r[-1]["step"],
                                      "phase": r[-1]["phase"],
                                      "cond": r[-1]["cond"], "ep": r[-1]["ep"]}
                                     for r in b.rows if r]}
            self._send(200, json.dumps(dbg), "application/json")
        else:
            super().do_GET()          # / and /static/* from disk

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        body = json.loads(self.rfile.read(n) or "{}")
        if self.path == "/label":
            self.bench.label(int(body["node"]), str(body.get("slug", "")),
                             str(body.get("action", "label")),
                             str(body.get("kind", "fault")))
            self._send(200, "ok", "text/plain")
        elif self.path == "/control":
            cmd = body.get("cmd")
            if cmd == "run":
                self.bench.pace_left += max(1, min(600,
                                                   int(body.get("seconds", 5))))
            elif cmd == "continue":
                self.bench.dismissed = self.bench.ask_serial
            self._send(200, "ok", "text/plain")
        elif self.path == "/machine":
            ev = str(body.get("ev", ""))
            if ev in ("START", "STOP", "SPEED1", "SPEED2", "SPEED3",
                      "IMBALANCE", "LOOSE_MOUNT", "PHASE_LOSS", "NORMAL"):
                self.bench.send(f"F {ev}")
                self._send(200, "ok", "text/plain")
            else:
                self._send(400, "bad state", "text/plain")
        elif self.path == "/export":
            self._send(200, json.dumps({"file": self.bench.export()}),
                       "application/json")
        else:
            self._send(404, "nope", "text/plain")


def pacer(bench: Bench, stop: threading.Event) -> None:
    while not stop.is_set():
        bench.pump()
        bench.tick()
        stop.wait(0.05)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--nodes", type=int, default=2)
    ap.add_argument("--profile", choices=["g100", "vib"], default=None,
                    help="single-node profile override (with --nodes 1)")
    ap.add_argument("--steps", type=int, default=900)
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--source", choices=["bench", "serial"], default="bench",
                    help="serial = real node over USB (firmware streaming "
                         "mode is future work)")
    ap.add_argument("--ci", action="store_true",
                    help="headless smoke test: scripted agent answers asks")
    args = ap.parse_args()

    if args.source == "serial":
        print("serial source is not implemented yet: the node firmware "
              "needs a USB streaming mode that emits the same S-line "
              "protocol (see webbench/app.py docstring)", file=sys.stderr)
        return

    build()
    if args.ci:
        bench = Bench(args.profile if args.nodes == 1 else None,
                      args.steps, args.nodes, autoagent=True)
        while True:
            bench.pump()
            if bench.done or all(len(rows) >= args.steps
                                 for rows in bench.rows):
                break
            bench.advance()
        bench.send("q")
        time.sleep(0.2)
        bench.pump()
        ev = [t for _, t in bench.events]
        print(f"ci: asks={sum(1 for t in ev if 'open cond=unknown' in t)} "
              f"labels={sum(1 for t in ev if '[FB]' in t and chr(39) + 'label' + chr(39) in t)} "
              f"shares={sum(1 for t in ev if '[SHARE]' in t)} "
              f"autos={sum(1 for t in ev if '[AUTO]' in t)} "
              f"verdicts={bench.verdict_count}")
        for t in [t for _, t in bench.events if t.startswith("DONE")]:
            print("  ", t)
        return

    bench = Bench(args.profile if args.nodes == 1 else None,
                  args.steps, args.nodes, autoagent=False)
    stop = threading.Event()
    threading.Thread(target=pacer, args=(bench, stop), daemon=True).start()
    Handler.bench = bench
    url = f"http://127.0.0.1:{args.port}"
    print(f"interactive bench: {url}  (nodes={args.nodes} "
          f"steps={args.steps}) - Ctrl-C to quit")
    try:
        ThreadingHTTPServer(("127.0.0.1", args.port), Handler).serve_forever()
    except KeyboardInterrupt:
        pass
    stop.set()
    bench.send("q")


if __name__ == "__main__":
    main()
