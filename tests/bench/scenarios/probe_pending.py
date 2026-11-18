#!/usr/bin/env python3
"""Targeted probe: the pending-batch plumbing only (no full scenario).

    python3 tests/bench/probe_pend.py [--port 8820]

1. learn on the stopped machine, wait for both monitors
2. START -> the speed_1 ask -> pending created (print id/step/n once)
3. release the hold, let the batch accumulate (~30 sim s, print n at
   most 3 checkpoints)
4. /discard by id
5. PASS if the pending list is empty afterwards
"""
import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from smoke import Client, pendings

ROOT = Path(__file__).resolve().parents[3]
SERVER = ROOT / "tests" / "bench" / "bench_server.py"


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--steps", type=int, default=900)
    ap.add_argument("--port", type=int, default=8820)
    args = ap.parse_args()

    server = subprocess.Popen(
        [sys.executable, str(SERVER), "--nodes", "2", "--steps",
         str(args.steps), "--port", str(args.port)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    c = Client(args.port)
    ok = False
    try:
        ready, t0 = False, time.time()
        while time.time() - t0 < 90:
            try:
                c.get("/state")
                ready = True
                break
            except Exception:
                if server.poll() is not None:
                    break
                time.sleep(1)
        if not ready:
            print("PROBE FAIL: server never came up")
            sys.exit(1)

        # 1) commission on the stopped machine
        c.post("/control", {"cmd": "learn"})
        while True:
            st = c.get("/state")
            if all(n["phase"] == "mon" for n in st["nodes"]):
                break
            if not st["playing"] and not st["holding"] and not st["done"]:
                c.post("/control", {"cmd": "run", "seconds": 60})
            time.sleep(0.2)
        print("1. both monitors ready at op_stopped")

        # 2) START -> the speed_1 ask -> pending created
        c.post("/machine", {"ev": "START"})
        item = None
        while item is None:
            st = c.get("/state")
            if not st["playing"] and not st["holding"] and not st["done"]:
                c.post("/control", {"cmd": "run", "seconds": 60})
            if st["ask"]:
                c.post("/control", {"cmd": "continue"})   # release the hold
            for node in range(len(st["pending"])):
                if st["pending"][node]:
                    item = (node, st["pending"][node][0])
            time.sleep(0.2)
        node, b = item
        print(f"2. pending created: node {node} id={b['id']} "
              f"step={b['step']} n={b['n']}")

        # 3) let it accumulate ~30 sim s (print only list changes)
        shown = 0
        t0 = time.time()
        last_sig = None
        while time.time() - t0 < 120 and shown < 3:
            st = c.get("/state")
            if not st["playing"] and not st["holding"] and not st["done"]:
                c.post("/control", {"cmd": "run", "seconds": 60})
            if st["ask"]:
                c.post("/control", {"cmd": "continue"})
            sig = json.dumps(st["pending"], sort_keys=True)
            if sig != last_sig:
                print(f"3. t+{int(time.time()-t0)}s pend -> {sig}")
                last_sig = sig
            cur = None
            for nb in st["pending"][node]:
                if nb["id"] == b["id"]:
                    cur = nb
            if cur and cur["n"] >= (shown + 1) * 8:
                shown += 1
            if cur is None:
                break                       # consumed early: plumbing bug
            time.sleep(0.2)
        print(f"4. discarding {b['id']} on node {node}")
        c.post("/discard", {"node": node, "batch": b["id"]})
        # queue sim time so the node processes the command and emits the
        # PEND update (an idle binary emits nothing)
        c.post("/control", {"cmd": "run", "seconds": 10})
        time.sleep(1.0)
        st = c.get("/state")
        for e in st["events"][-6:]:
            print("   ev:", e[1][:80])
        left = pendings(st)
        print(f"5. pendings left after discard: {left} "
              f"(node {node}: {st['pending'][node]})")
        # the probe discards only node {node}'s batch: the other node's
        # ask is untouched by design (batch ids are per node)
        ok = not st["pending"][node]

        print("PROBE PASS" if ok else "PROBE FAIL")
        sys.exit(0 if ok else 1)
    finally:
        server.terminate()


if __name__ == "__main__":
    main()
