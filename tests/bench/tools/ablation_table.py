#!/usr/bin/env python3
"""Compact leave-one-out ablation rows from the exported paper-tour runs.

    python3 tests/bench/tools/ablation_table.py

One line per configuration: asks (redundant), false alarms vfd/vib,
owner identification hits/total + latencies (steps, tour order: imb, imb,
pf, imb, imb, pf), final vocabulary vfd/vib, re-tour asks (from the run's
PAPER RUN line, noted in ROWS).
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from summarize_run import summarize

RUNS = Path(__file__).resolve().parents[1] / "runs"
# (label, run file, re-tour asks from the PAPER RUN line)
ROWS = [
    ("full (all ON)", "run-20261122-203729.jsonl", 0),
    ("- positional",  "run-20261122-220739.jsonl", 0),
    ("- percond",     "run-20261122-221416-8801.jsonl", 0),
    ("- accumulator", "run-20261122-221415-8802.jsonl", 0),
    ("- settle",      "run-20261122-221415-8803.jsonl", 0),
    ("- drift-recov", "run-20261122-221415-8804.jsonl", 0),
    ("- unk-persist", "run-20261122-221415-8805.jsonl", 0),
    ("rel-obs",       "run-20261122-221417-8806.jsonl", 1),
]
print(f"{'row':14s} {'asks(red)':9s} {'FA v/v':7s} {'owner':6s} "
      f"{'lat (steps)':16s} {'vocab v/v':9s} {'retour':6s}")
for name, f, ret in ROWS:
    _, m = summarize(str(RUNS / f))
    fa = m["false_alarms"]
    fa_s = "/".join(str(fa.get(k, "-")) for k in ("vfd_g100", "vib_adxl"))
    lat = ",".join(str(v) for v in m["owner_latencies"]) or "-"
    own = f"{m['owner_hits']}/{m['owner_total']}"
    v = m["vocabulary"]
    v_s = "/".join(str(v.get(k, "-")) for k in ("vfd_g100", "vib_adxl"))
    print(f"{name:14s} {str(m['asks']) + '(' + str(m['redundant_asks']) + ')':9s} "
          f"{fa_s:7s} {own:6s} {lat:16s} {v_s:9s} {ret}")
