#!/usr/bin/env python3
"""Minimal incremental tour: add ONE condition at a time (user discipline,
2026-11-22d). The tour list is explicit and SHORT; --stages N stops after
N steps so the next condition can be appended and re-tested on its own,
and the per-stage dwell is the sample count that stage holds for.

    python3 tests/bench/scenarios/mini_tour.py [--stages N] [--dwell D]

Stage kinds:
  ("machine", ev, dwell)  fire the machine event, hold >= dwell steps
                          (asks during machine stages are released - the
                          state is named at its own label stage)
  ("label", slug, dwell)  label matured pendings as slug; done when labeled
                          and settled >= dwell; 45 steps with no ask =
                          already known, move on
  ("known", slug, dwell)  map asks onto the slug (desensitise); done when
                          ANY node names it and nothing is maturing

The user's sequence, one condition at a time:
  stopped -> speed_1 -> stopped -> speed_2 -> speed_1 -> stopped
  -> speed_1 -> speed_1_phase_loss -> (next run: speed_2_phase_loss)
"""
import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from smoke import Client, pendings
from summarize_run import summarize

ROOT = Path(__file__).resolve().parents[3]
SERVER = ROOT / "tests" / "bench" / "bench_server.py"

NORMALS = {"op_stopped", "speed_1", "speed_2"}
MIN_BATCH_N = 15      # = cfg::ASK_MIN_BATCH

TOUR = [
    ("cmd",     "learn",   0),
    ("wait_mon", None,     0),
    ("machine", "START",  30),   # speed_1 first visit
    ("label",   "speed_1", 30),
    ("machine", "STOP",   20),
    ("known",   "op_stopped", 20),
    ("machine", "SPEED2",  8),
    ("machine", "START",  30),   # speed_2 first visit
    ("label",   "speed_2", 30),
    ("machine", "SPEED1", 20),   # RUNNING setpoint change back to speed_1
    ("known",   "speed_1", 20),
    ("machine", "STOP",   20),
    ("known",   "op_stopped", 20),
    ("machine", "NORMAL",  8),
    ("machine", "PHASE_LOSS", 8),
    ("machine", "SPEED1",  8),
    ("machine", "START",  60),   # start INTO the fault
    ("label",   "speed_1_phase_loss", 60),
    ("machine", "STOP",   20),
    ("machine", "NORMAL",  8),
    ("machine", "PHASE_LOSS", 8),
    ("machine", "SPEED2",  8),
    ("machine", "START",  60),   # start INTO the fault at speed_2
    ("label",   "speed_2_phase_loss", 60),
    # fault re-tour: learned faults must be RECOGNIZED (zero asks)
    ("machine", "STOP",   20),
    ("machine", "NORMAL",  8),
    ("machine", "PHASE_LOSS", 8),
    ("machine", "SPEED1",  8),
    ("machine", "START",  40),
    ("known",   "speed_1_phase_loss", 40),
    ("machine", "SPEED2", 40),   # setpoint change UNDER the fault
    ("known",   "speed_2_phase_loss", 40),
    ("machine", "STOP",   20),
    ("machine", "NORMAL",  8),
]


def max_step(st) -> int:
    return max((n["step"] for n in st["nodes"]), default=0)


def maturing(st) -> bool:
    return any(n["cond"] == "unidentified" for n in st["nodes"])


def flow(c, st) -> None:
    if not st["playing"] and not st["holding"] and not st["done"]:
        c.post("/control", {"cmd": "run", "seconds": 60})


def oracle(c, st, slug, stage_step) -> int:
    """Label matured pendings with slug; discard stale batches (the
    paper_tour stale-batch discipline, unchanged)."""
    sent = 0
    kind = "normal" if slug in NORMALS else "fault"
    for node in range(len(st["pending"])):
        for b in st["pending"][node]:
            if b["n"] >= MIN_BATCH_N and b["step"] >= stage_step - 5:
                c.post("/label", {"node": node, "slug": slug,
                                  "action": "label", "kind": kind,
                                  "batch": b["id"]})
                sent += 1
            elif b["step"] < stage_step - 5:
                c.post("/discard", {"node": node, "batch": b["id"]})
    return sent


def run_tour(c: Client, args):
    tour = TOUR[:args.stages] if args.stages else TOUR
    dwell_default = args.dwell
    stage = 0
    stage_step = 0
    issued = False
    learned = 0
    t0 = time.time()
    wall = 600 + sum((s[2] or dwell_default) for s in tour) * 12

    while stage < len(tour) and time.time() - t0 < wall:
        st = c.get("/state")
        flow(c, st)
        kind, arg, dwell = tour[stage][0], tour[stage][1], \
            tour[stage][2] or dwell_default
        due = False

        if st["ask"] and kind in ("cmd", "wait_mon", "machine"):
            c.post("/control", {"cmd": "continue"})

        if kind == "cmd":
            if not issued:
                c.post("/control", {"cmd": "learn"})
                issued = True
                print(f"  [{stage}] learn requested (machine stopped)")
            due = issued
        elif kind == "wait_mon":
            due = all(n["phase"] == "mon" for n in st["nodes"])
            if due:
                print(f"  [{stage}] both nodes monitor (op_stopped)")
        elif kind == "machine":
            if not issued:
                c.post("/machine", {"ev": arg})
                issued = True
                print(f"  [{stage}] machine {arg}")
            due = max_step(st) - stage_step >= dwell
        elif kind == "label":
            if st["ask"] or pendings(st):
                sent = oracle(c, st, arg, stage_step)
                learned += sent
                if sent:
                    print(f"  [{stage}] labeled {sent} batch(es) as {arg}")
            if max_step(st) - stage_step >= 45 and not pendings(st) \
                    and not maturing(st) and not st["ask"]:
                print(f"  [{stage}] no ask - {arg} already known")
                due = True
            else:
                due = sent > 0 and not st["ask"] and not pendings(st) \
                    and not maturing(st) \
                    and max_step(st) - stage_step >= dwell
        elif kind == "known":
            if st["ask"] or pendings(st):
                n_sent = oracle(c, st, arg, stage_step)
                learned += n_sent
                if n_sent:
                    print(f"  [{stage}] mapped {n_sent} batch(es) onto "
                          f"{arg}")
            conds = [st["nodes"][i]["cond"] for i in
                     range(len(st["nodes"]))]
            due = arg in conds and not st["ask"] and not maturing(st) \
                and max_step(st) - stage_step >= dwell
            if due:
                print(f"  [{stage}] recognised {arg} "
                      f"(conds={conds})")

        if due:
            stage += 1
            stage_step = max_step(st)
            issued = False
        elif time.time() - t0 > 0 and max_step(st) - stage_step > 120 \
                and kind != "label":
            print(f"  [{stage}] stage forced on ({kind} {arg})")
            stage += 1
            stage_step = max_step(st)
            issued = False
        time.sleep(0.2)

    complete = stage >= len(tour)
    if not st["done"]:
        c.post("/control", {"cmd": "run", "seconds": 30})
        time.sleep(3)
    data = json.loads(c.post("/export", {}).decode())
    run_file = data["file"]
    detail = (f"tour {'complete' if complete else 'incomplete'}, "
              f"batches labeled {learned}")
    return complete, detail, run_file


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--stages", type=int, default=0,
                    help="stop after N tour steps (0 = the whole tour)")
    ap.add_argument("--dwell", type=int, default=20,
                    help="default per-stage dwell for stages without one")
    ap.add_argument("--port", type=int, default=8791)
    args = ap.parse_args()

    server = subprocess.Popen(
        [sys.executable, str(SERVER), "--nodes", "2", "--steps", "1800",
         "--port", str(args.port), "--mode", "two-way"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    c = Client(args.port)
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
            print("MINI TOUR FAIL: server never came up; output:")
            print(server.stdout.read()[-2000:] if server.stdout else "")
            sys.exit(1)
        ok, detail, run_file = run_tour(c, args)
        text, _ = summarize(run_file)
        print(text)
        print(("MINI TOUR PASS: " if ok else "MINI TOUR FAIL: ") + detail)
        sys.exit(0 if ok else 1)
    finally:
        server.terminate()


if __name__ == "__main__":
    main()
