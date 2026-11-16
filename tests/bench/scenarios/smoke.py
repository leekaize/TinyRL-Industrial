#!/usr/bin/env python3
"""Headless end-to-end smoke test of the web bench's user loop.

    python3 tests/bench/smoke.py [--steps 900]

Plays the human against the live HTTP server (no autoplay anywhere):
  1. commission on the STOPPED machine ('learn'; 2026-11-15 flow) ->
     baseline op_stopped
  2. START -> speed_1 unknown -> label 'speed_1' from the stored batch
  3. inject IMBALANCE -> unknown -> label 'speed_1_imbalance' from the
     batch, release the hold (the operator walks away: late labels)
  4. clear the fault, wait for the state to settle
  5. re-inject IMBALANCE -> the node must recognise it LOCALLY
     (cond=speed_1_imbalance, NO new ask) -> PASS

Exits 0 on pass, 1 on fail. Uses only stdlib + the HTTP API.
"""
import argparse
import json
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SERVER = ROOT / "tests" / "bench" / "bench_server.py"

TRUTH = {"op_speed_1": ("speed_1", "normal"),
         "imbalance": ("speed_1_imbalance", "fault"),
         "phase_loss": ("speed_1_phase_loss", "fault")}
MIN_BATCH_N = 25      # label only after the batch accumulated settled data


class Client:
    def __init__(self, port):
        self.base = f"http://127.0.0.1:{port}"

    def get(self, path):
        with urllib.request.urlopen(self.base + path, timeout=5) as r:
            return json.loads(r.read())

    def post(self, path, body):
        req = urllib.request.Request(
            self.base + path, data=json.dumps(body).encode(),
            headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.read()


def pendings(st) -> int:
    return sum(len(p) for p in st["pending"])


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--steps", type=int, default=900)
    ap.add_argument("--port", type=int, default=8790)
    args = ap.parse_args()

    server = subprocess.Popen(
        [sys.executable, str(SERVER), "--nodes", "2", "--steps",
         str(args.steps), "--port", str(args.port)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    c = Client(args.port)
    try:
        # the server builds the bench binary first: retry until it listens
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
            print("SMOKE FAIL: server never came up; output:")
            print(server.stdout.read()[-2000:] if server.stdout else "")
            sys.exit(1)
        result, detail = run_loop(c, args.steps)
        print(("SMOKE PASS: " if result else "SMOKE FAIL: ") + detail)
        try:
            data = json.loads(c.post("/export", {}).decode())
            print("evidence:", data["file"])
        except Exception:
            pass
        sys.exit(0 if result else 1)
    finally:
        server.terminate()


def run_loop(c: Client, steps: int):
    started = False
    injected = False
    labeled_speed = False
    labeled_fault = False
    labeled_at = 0
    reverted = False
    reverted_at = 0
    rechecked = False
    t0 = time.time()
    last_dbg = t0
    node_type = {}
    while time.time() - t0 < 240:
        st = c.get("/state")
        if time.time() - last_dbg > 10:
            last_dbg = time.time()
            print(f"  [dbg t={int(time.time()-t0)}s machine={st['machine']} "
                  f"steps={[n['step'] for n in st['nodes']]} "
                  f"phases={[n['phase'] for n in st['nodes']]} "
                  f"conds={[n['cond'] for n in st['nodes']]} "
                  f"pend={pendings(st)} holding={st['holding']} "
                  f"done={st['done']}")

        # the bench has no autoplay: keep sim time flowing in +60 s chunks
        if not st["playing"] and not st["holding"] and not st["done"]:
            c.post("/control", {"cmd": "run", "seconds": 60})

        for n in st["nodes"]:
            node_type[n["id"]] = n["node_type"]
        vib = next((i for i, t in node_type.items() if t == "vib_adxl"), None)

        # 5) the payoff check: re-injected imbalance is matched locally
        if rechecked and labeled_fault:
            vib_ask = next((a for a in st["ask"] if a["node"] == vib), None)
            vib_cond = st["nodes"][vib]["cond"] if vib is not None else ""
            if vib is not None and not vib_ask \
                    and vib_cond == "speed_1_imbalance":
                return True, (f"vib node matched the re-injected imbalance "
                              f"locally at step {st['nodes'][vib]['step']} "
                              f"with no ask")
            if st["done"]:
                return False, "run ended before the payoff check"

        # 0) commission on the STOPPED machine (2026-11-15 flow)
        if not started and all(n["phase"] == "idle" for n in st["nodes"]):
            c.post("/control", {"cmd": "learn"})
            started = True
            print("  learn requested (machine stopped)")

        # 1) once both nodes monitor, start the motor
        if started and not injected \
                and st["machine"] == "op_stopped" \
                and all(n["phase"] == "mon" for n in st["nodes"]):
            c.post("/machine", {"ev": "START"})
            print("  START -> speed_1 (unknown: ask expected)")

        # oracle: release the hold immediately so sim time keeps flowing
        # and the ask's batch accumulates SETTLED samples; label it by id
        # once it holds enough data (the real operator reviews before
        # naming). Stale batches are discarded: their state has passed.
        now_step = max(n["step"] for n in st["nodes"])
        if st["ask"] or pendings(st):
            slug, kind = TRUTH.get(st["machine"], ("", "normal"))
            if slug:
                for node in range(len(st["pending"])):
                    for b in st["pending"][node]:
                        if b["n"] >= MIN_BATCH_N:
                            c.post("/label", {"node": node, "slug": slug,
                                              "action": "label",
                                              "kind": kind,
                                              "batch": b["id"]})
                            if kind == "fault":
                                labeled_fault = True
                            elif slug == "speed_1":
                                labeled_speed = True
                            labeled_at = now_step
                            print(f"  labeled {slug} on node {node} "
                                  f"(batch {b['id']}, n={b['n']})")
                        elif now_step - b["step"] > 60:
                            c.post("/discard", {"node": node,
                                                "batch": b["id"]})
                            print(f"  discarded stale {b['id']} "
                                  f"on node {node}")
            for a in st["ask"]:
                c.post("/control", {"cmd": "continue"})

        # 2) once speed_1 is learned AND quiet for a while, inject the
        # fault: nodes alarm at different times, so the dwell must span
        # the slowest node's alarm latency before the next machine command
        if started and not injected and labeled_speed and not labeled_fault \
                and st["machine"] == "op_speed_1" \
                and all(n["phase"] == "mon" for n in st["nodes"]) \
                and not st["ask"] and pendings(st) == 0 \
                and now_step - labeled_at >= 30:
            c.post("/machine", {"ev": "IMBALANCE"})
            injected = True
            print("  injected IMBALANCE (unknown: ask expected)")

        # 3) clear the fault once the batch is labeled AND the net has
        # had a dwell on it: the ask popup legitimately persists while a
        # real fault stays installed (sustained faults keep alarming)
        if injected and labeled_fault and not rechecked \
                and st["machine"] == "imbalance" and pendings(st) == 0 \
                and now_step - labeled_at >= 40:
            c.post("/machine", {"ev": "NORMAL"})
            reverted = True
            reverted_at = now_step
            print("  fault cleared, back to speed_1")

        # 4) second injection -> the payoff check arms (same quiet-dwell
        # rule: let the settling finish before changing state again)
        if reverted and not rechecked and st["machine"] == "op_speed_1" \
                and not st["ask"] and pendings(st) == 0 \
                and now_step - reverted_at >= 20:
            c.post("/machine", {"ev": "IMBALANCE"})
            rechecked = True
            print("  re-injected IMBALANCE (payoff check)")
        time.sleep(0.2)
    return False, "timed out"


if __name__ == "__main__":
    main()
