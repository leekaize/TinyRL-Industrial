#!/usr/bin/env python3
"""Remove the knob-turned-no-RUN rows from the g100 collect CSV.

The collect procedure sets the VFD's frequency knob BEFORE pressing RUN,
so each start records rows with fset > 0 while the current still sits at
the stopped baseline - the register says "running", the machine is not.
Those rows poison the condition space (a fake running-setpoint point at
stopped current) and the bench tour's setpoint-change replays.

Rule (user, 2026-11-22): drop every row where fset > 0 and the current
has not started rising. Measured on this capture:
  not-risen rows: i in 0.1469..0.1563 (stopped baseline ~0.149 +- 0.002)
  first rising row: i = 0.1839 (inrush beginning, row 565)
  threshold 0.17 A sits between them.
Labeled condition rows are asserted unaffected: the 5 condition blocks
are fully running-level, so the rule only ever fires inside unlabeled
transition ramps.

    python3 tests/bench/tools/clean_collect.py [in.csv [out.csv]]

Default: cleans data/serial/collect_20261121_211534.csv in place after
writing collect_20261121_211534.raw.csv next to it (the loader reads the
original name; data/ is gitignored so the backup keeps the raw capture).
"""
import csv
import shutil
import sys
from pathlib import Path

I_STILL = 0.17   # A; between 0.1563 (not risen) and 0.1839 (rising)

src = Path(sys.argv[1] if len(sys.argv) > 1
           else "data/serial/collect_20261121_211534.csv")
dst = Path(sys.argv[2]) if len(sys.argv) > 2 else src

rows = list(csv.DictReader(src.open()))
fields = list(rows[0].keys())

def is_bad(r):
    return float(r["fset"]) > 0 and float(r["i"]) < I_STILL

labeled_bad = [r for r in rows if r["label"] and is_bad(r)]
if labeled_bad:
    sys.exit(f"ABORT: {len(labeled_bad)} LABELED rows match the rule - "
             "the threshold is wrong for this capture, fix it first")

# report per segment
segs = []
for r in rows:
    lab = r["label"] or "(transition)"
    if not segs or segs[-1][0] != lab:
        segs.append([lab, []])
    segs[-1][1].append(r)

removed = 0
for lab, rs in segs:
    bad = [r for r in rs if is_bad(r)]
    if bad:
        print(f"{lab:22s} rows {len(rs):3d}: removing {len(bad)} "
              f"(t {bad[0]['wall_hms']}..{bad[-1]['wall_hms']}, "
              f"fset {bad[0]['fset']}..{bad[-1]['fset']}, "
              f"i {min(float(r['i']) for r in bad):.4f}.."
              f"{max(float(r['i']) for r in bad):.4f})")
        removed += len(bad)

print(f"total: {removed} of {len(rows)} rows removed, "
      f"{len(rows) - removed} remain")

if removed == 0:
    print("nothing to do")
    sys.exit(0)

if dst == src:
    bak = src.with_suffix(".raw.csv")
    if not bak.exists():
        shutil.copy2(src, bak)
        print(f"backup: {bak}")

with dst.open("w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=fields)
    w.writeheader()
    for r in rows:
        if not is_bad(r):
            w.writerow(r)
print(f"written: {dst}")
