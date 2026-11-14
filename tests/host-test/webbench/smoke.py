#!/usr/bin/env python3
"""Headless end-to-end smoke test of the web bench's user loop.

    python3 tests/host-test/webbench/smoke.py [--steps 900]

Plays the human against the live HTTP server:
  1. inject IMBALANCE from the machine panel
  2. when the vibration node asks, label it 'imbalance'
  3. dismiss any false alarm (normal feedback)
  4. revert the machine to normal, wait for the episode to clear
  5. re-inject IMBALANCE -> the node must recognise it LOCALLY
     (cond=imbalance, NO new ask) -> PASS

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
SERVER = ROOT / "tests" / "host-test" / "interactive_bench.py"


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
        sys.exit(0 if result else 1)
    finally:
        server.terminate()


def run_loop(c: Client, steps: int):
    injected = False
    labeled = False
    reverted = False
    rechecked = False
    t0 = time.time()
    last_dbg = t0
    node_type = {}
    while time.time() - t0 < 120:
        st = c.get("/state")
        if time.time() - last_dbg > 10:
            last_dbg = time.time()
            print(f"  [dbg t={int(time.time()-t0)}s machine={st['machine']} "
                  f"steps={[n['step'] for n in st['nodes']]} "
                  f"conds={[n['cond'] for n in st['nodes']]} "
                  f"holding={st['holding']} done={st['done']} coaching={not (injected and labeled and reverted and rechecked)}")
        st = c.get("/state")
        for n in st["nodes"]:
            node_type[n["id"]] = n["node_type"]
        vib = next((i for i, t in node_type.items() if t == "vib_adxl"), None)

        # 5) the payoff check: re-injected imbalance is matched locally
        if rechecked and injected and labeled:
            vib_ask = next((a for a in st["ask"] if a["node"] == vib), None)
            vib_cond = st["nodes"][vib]["cond"] if vib is not None else ""
            if vib is not None and not vib_ask and vib_cond == "imbalance":
                return True, (f"vib node matched the re-injected imbalance "
                              f"locally at step {st['nodes'][vib]['step']} "
                              f"with no ask")
            if st["done"]:
                return False, "run ended before the payoff check"

        # the machine panel: user injects the fault
        if not injected and st["machine"] == "normal" \
                and st["nodes"][0]["step"] > 90:
            c.post("/machine", {"ev": "IMBALANCE"})
            injected = True
            print("  injected IMBALANCE")

        # 2) the oracle: label the injected fault, dismiss false alarms
        if st["ask"] and st["holding"]:
            a = st["ask"][0]
            is_vib_fault = (st["machine"].upper() == "IMBALANCE"
                            and node_type.get(a["node"]) == "vib_adxl")
            if not labeled:
                if is_vib_fault and not injected:
                    pass  # ask came before the injection: not our event
                elif is_vib_fault:
                    c.post("/label", {"node": a["node"], "slug": "imbalance",
                                      "action": "label"})
                    labeled = True
                    print(f"  labeled imbalance on node {a['node']} "
                          f"(step {a['step']})")
                else:
                    c.post("/label", {"node": a["node"], "slug": "",
                                      "action": "normal"})
                    print(f"  dismissed a false alarm on node {a['node']} "
                          f"(step {a['step']})")
            elif labeled and not rechecked:
                c.post("/control", {"cmd": "continue"})

        # 3) revert the machine once the labelled episode is closed
        if labeled and not reverted \
                and st["machine"].upper() == "IMBALANCE" \
                and not st["ask"]:
            c.post("/machine", {"ev": "NONE"})
            reverted = True
            print("  machine back to normal")

        # 4) second injection -> the payoff check arms
        if labeled and reverted and not rechecked \
                and st["machine"].lower() == "normal":
            c.post("/machine", {"ev": "IMBALANCE"})
            rechecked = True
            print("  re-injected IMBALANCE (payoff check)")
        time.sleep(0.2)
    return False, "timed out"


if __name__ == "__main__":
    main()
