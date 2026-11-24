#!/usr/bin/env python3
"""Paper scenario: the SEVEN-condition tour, oracle labels from machine truth.

    python3 tests/bench/paper_run.py [--mode two-way|node-only]
            [--nodes 2] [--steps 2400] [--port 8791]

Commissioning happens on the STOPPED machine ('G'; user decision
2026-11-15): the baseline condition is op_stopped. Faults are installed
while stopped, then the motor starts (real-world procedure). Learning
tour: speed_1, speed_2, speed_1_imbalance, speed_1_phase_loss,
speed_2_phase_loss (op_stopped comes free with the baseline). Re-tour:
all conditions recognised with ZERO asks - that count is the payoff.

Every unknown ask is labeled from its stored batch id (late labels are
the point of the pending store); asks during a known state map onto it.
In node-only mode no labels exist, so the tour walks the states and
measures detection latency only.

Ends with /export and prints the summarizer block for the run.
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
MIN_BATCH_N = 15      # = cfg::ASK_MIN_BATCH: label as soon as the ask matures
SLUG = {"op_speed_1": "speed_1", "op_speed_2": "speed_2",
        "op_stopped": "op_stopped",
        "imbalance": None, "phase_loss": None}


def truth_slug(machine, context):
    if machine in SLUG and machine.startswith("op_"):
        return SLUG[machine], "normal"
    # fault installed: compound with the running setpoint (speed_1_imb...)
    speed = context[3:] if context.startswith("op_") else context
    return f"{speed}_{machine}", "fault"


LEARN_TOUR = [
    ("cmd", "learn"),
    ("wait_mon", None),
    # Stage-6's commissioning budget (run_all(500) = cal 30 + fit 30 +
    # ~440 settled stopped steps): the per-condition units tighten on the
    # streamed rows, so a foreign state leaves MATCH_Q at its ONSET (the
    # vib's b2 d=13.36 at speed_1) and asks - instead of diluting into the
    # fresh op_stopped radius and holding forever (the 2026-11-22f finding:
    # without this the vib never learned speed_1 in the paper tour).
    ("settle", 440),
    ("machine", "START"),
    ("unknown", "speed_1"),
    ("machine", "SPEED2"),
    ("unknown", "speed_2"),
    ("machine", "STOP"),
    ("known", "op_stopped"),
    ("machine", "IMBALANCE"),
    ("machine", "SPEED1"),
    ("machine", "START"),
    ("unknown", "speed_1_imbalance"),
    ("machine", "STOP"),
    ("machine", "NORMAL"),
    ("machine", "IMBALANCE"),
    ("machine", "SPEED2"),
    ("machine", "START"),
    ("unknown", "speed_2_imbalance"),
    ("machine", "STOP"),
    ("machine", "NORMAL"),
    ("machine", "PHASE_LOSS"),
    ("machine", "SPEED1"),
    ("machine", "START"),
    ("unknown", "speed_1_phase_loss"),
    ("machine", "SPEED2"),
    ("unknown", "speed_2_phase_loss"),
    ("machine", "STOP"),
    ("machine", "NORMAL"),
]
RETOUR = [
    ("machine", "SPEED1"),
    ("machine", "START"),
    ("known", "speed_1"),
    ("machine", "SPEED2"),
    ("known", "speed_2"),
    ("machine", "STOP"),
    ("known", "op_stopped"),
    ("machine", "IMBALANCE"),
    ("machine", "SPEED1"),
    ("machine", "START"),
    ("known", "speed_1_imbalance"),
    ("machine", "STOP"),
    ("machine", "NORMAL"),
    ("machine", "IMBALANCE"),
    ("machine", "SPEED2"),
    ("machine", "START"),
    ("known", "speed_2_imbalance"),
    ("machine", "STOP"),
    ("machine", "NORMAL"),
    ("machine", "PHASE_LOSS"),
    ("machine", "SPEED2"),
    ("machine", "START"),
    ("known", "speed_2_phase_loss"),
    ("machine", "STOP"),
    ("machine", "NORMAL"),
]
MACHINE_TRUTH = {"START": "op_speed_1", "STOP": "op_stopped",
                 "SPEED2": "op_speed_2"}   # fault states: truth stays
                                          # op_stopped until running
DWELL = 40            # sim seconds per state in node-only mode
SETTLE = 8            # sim seconds after each machine command


def max_step(st) -> int:
    return max((n["step"] for n in st["nodes"]), default=0)


def maturing(st) -> bool:
    """Any node still accumulating a batch (condition 'unidentified'): the
    ask has not surfaced yet, so a stage must NOT resolve while this is
    true - an ask that lands after the stage moved on gets stale-discarded
    by the oracle and its condition is never named (bench 2026-11-19)."""
    return any(n["cond"] == "unidentified" for n in st["nodes"])


def flow(c, st) -> None:
    if not st["playing"] and not st["holding"] and not st["done"]:
        c.post("/control", {"cmd": "run", "seconds": 60})


def oracle(c, st, slug=None, kind="fault", stage_step=0) -> int:
    """Label the CURRENT state's stored batches once they hold enough
    settled samples (MIN_BATCH_N); before that, keep sim time flowing so
    they accumulate. Batches from PAST states (frozen before this stage's
    step) are DISCARDED, never labelled: stamping a stale batch with this
    stage's slug blends two different states into one condition and
    poisons the centroid (bench, 2026-11-15). Returns labels sent.

    Default (slug=None): asks during a KNOWN normal state map onto that
    state (desensitise), per the 2026-11-15 workflow.
    """
    sent = 0
    now_step = max((n["step"] for n in st["nodes"]), default=0)
    if slug is None:
        slug, kind = truth_slug(st["machine"], st["context"])
    for node in range(len(st["pending"])):
        for b in st["pending"][node]:
            if b["n"] >= MIN_BATCH_N and b["step"] >= stage_step - 5:
                c.post("/label", {"node": node, "slug": slug,
                                  "action": "label", "kind": kind,
                                  "batch": b["id"]})
                sent += 1
            elif b["step"] < stage_step - 5:
                c.post("/discard", {"node": node, "batch": b["id"]})
    if st["ask"]:
        c.post("/control", {"cmd": "continue"})   # keep time flowing
    return sent


def run_tour(c: Client, args):
    mode = args.mode
    tour = LEARN_TOUR + RETOUR
    retour_at = len(LEARN_TOUR)
    stage = 0
    stage_step = 0
    issued = False
    asked_seen = False
    retour_asks = 0
    learned = 0
    node_type = {}
    dwell = args.dwell
    t0 = time.time()
    wall = 600 + dwell * 12      # the learn stages each hold >= dwell steps

    while stage < len(tour) and time.time() - t0 < wall:
        st = c.get("/state")
        flow(c, st)
        for n in st["nodes"]:
            node_type[n["id"]] = n["node_type"]

        kind, arg = tour[stage]
        due = False

        # An ask can open on a state the tour does not name at THIS stage
        # (e.g. a setpoint change mid-stage). A hold here would freeze sim
        # time forever - machine stages have no oracle. Release it; the
        # state is named at its own 'unknown' stage. NOTE: since the
        # 2026-11-19 generator change, faults installed while stopped
        # measure exactly as stopped, so no ask can fire before START.
        if kind in ("cmd", "wait_mon", "machine", "settle") and st["ask"]:
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
                print(f"  [{stage}] both nodes monitor (baseline op_stopped)")
        elif kind == "machine":
            if not issued:
                c.post("/machine", {"ev": arg})
                issued = True
                print(f"  [{stage}] machine {arg}")
            due = max_step(st) - stage_step >= SETTLE
        elif kind == "settle":
            due = max_step(st) - stage_step >= arg
            if due:
                print(f"  [{stage}] commissioning settle done "
                      f"({arg} settled stopped steps)")
        elif kind == "unknown":
            if mode == "two-way":
                if not asked_seen:
                    if st["ask"] or pendings(st) or maturing(st):
                        asked_seen = True
                    elif max_step(st) - stage_step >= 45:
                        asked_seen = True       # nothing asked: known
                        conds = [st["nodes"][i]["cond"] for i in node_type]
                        print(f"  [{stage}] no ask - {arg} already known "
                              f"(conds={conds})")
                else:
                    if st["ask"] or pendings(st):
                        sent = oracle(c, st, arg,
                                      "normal" if arg in NORMALS else "fault",
                                      stage_step)
                        learned += sent
                        if sent:
                            print(f"  [{stage}] labeled {sent} batch(es) "
                                  f"as {arg}")
                    # the stage holds >= DWELL steps after the switch so
                    # EVERY node gets its latency budget (sticky release
                    # 80 + alarm + ask batch 15): the first node to ask
                    # must not close the stage under the slower one
                    # (2026-11-22c VFD finding, --dwell sweep)
                    due = not st["ask"] and pendings(st) == 0 \
                        and not maturing(st) \
                        and max_step(st) - stage_step >= dwell
            else:
                due = max_step(st) - stage_step >= DWELL
                if due:
                    print(f"  [{stage}] dwell done ({arg}, node-only)")
        elif kind == "known":
            if mode == "two-way":
                if st["ask"] or pendings(st):
                    # map onto the condition's OWN kind: a fault condition
                    # must never be mapped as normal (that flips the kind
                    # and ratchets theta deaf)
                    n_sent = oracle(c, st, arg,
                                    "normal" if arg in NORMALS else "fault",
                                    stage_step)
                    if stage >= retour_at:
                        retour_asks += n_sent
                    if n_sent:
                        print(f"  [{stage}] mapped {n_sent} batch(es) onto "
                              f"{arg} (desensitise)")
                conds = [st["nodes"][i]["cond"] for i in node_type]
                # owner rule (2026-11-19): ANY node naming the state is
                # recognition - fleet faults are owned by one modality (the
                # vfd physically cannot see imbalance: i +0.05, tq +1.2 are
                # inside its own noise), so requiring ALL nodes pinned
                # stages on states half the rig is blind to
                due = arg in conds and not st["ask"] and not maturing(st)
            else:
                due = max_step(st) - stage_step >= DWELL
            if due:
                print(f"  [{stage}] {'recognised' if mode == 'two-way' else 'dwell done'} {arg}")

        if due:
            stage += 1
            stage_step = max_step(st)
            issued = False
            asked_seen = False
            stage_until = time.time() + 60
        elif time.time() - t0 > 0 and max_step(st) - stage_step > 60 \
                and kind not in ("unknown", "settle"):
            # safety valve: never wedge the tour on one stage
            print(f"  [{stage}] stage forced on ({kind} {arg})")
            stage += 1
            stage_step = max_step(st)
            issued = False
            asked_seen = False
        time.sleep(0.2)

    complete = stage >= len(tour)
    if not st["done"]:
        c.post("/control", {"cmd": "run", "seconds": 30})
        time.sleep(3)
    data = json.loads(c.post("/export", {}).decode())
    run_file = data["file"]
    detail = (f"tour {'complete' if complete else 'incomplete'}, "
              f"batches labeled {learned}, re-tour asks {retour_asks}")
    return complete, detail + f", exported {run_file}", run_file


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", choices=["two-way", "node-only"],
                    default="two-way")
    ap.add_argument("--nodes", type=int, default=2)
    ap.add_argument("--steps", type=int, default=2400)
    ap.add_argument("--port", type=int, default=8791)
    ap.add_argument("--profile", choices=["g100", "vib"], default=None,
                    help="single-node profile override (with --nodes 1); "
                         "used to measure per-node flash state exactly")
    ap.add_argument("--dwell", type=int, default=12,
                    help="min sim steps each learn stage stays open after "
                         "the switch (the per-node latency budget: sticky "
                         "release 80 + alarm + ask batch 15)")
    args = ap.parse_args()

    server = subprocess.Popen(
        [sys.executable, str(SERVER), "--nodes", str(args.nodes), "--steps",
         str(args.steps), "--port", str(args.port), "--mode", args.mode]
        + (["--profile", args.profile] if args.profile else []),
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
            print("PAPER RUN FAIL: server never came up; output:")
            print(server.stdout.read()[-2000:] if server.stdout else "")
            sys.exit(1)
        ok, detail, run_file = run_tour(c, args)
        text, _ = summarize(run_file)
        print(text)
        print(("PAPER RUN PASS: " if ok else "PAPER RUN FAIL: ") + detail)
        sys.exit(0 if ok else 1)
    finally:
        server.terminate()


if __name__ == "__main__":
    main()
