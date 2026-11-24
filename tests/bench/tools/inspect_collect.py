#!/usr/bin/env python3
"""Inspect the g100 collect CSV: where is fset set but the current never
left the stopped baseline (knob turned, RUN not pressed)?"""
import csv
import sys
from pathlib import Path

path = Path("data/serial/collect_20261121_211534.csv")
rows = list(csv.DictReader(path.open()))
print(f"total rows: {len(rows)}")

# stopped baseline of i: rows with fset==0
istop = [float(r["i"]) for r in rows if float(r["fset"]) == 0]
print(f"fset==0 rows: {len(istop)}  i range {min(istop):.4f}..{max(istop):.4f}")

# segment map: label changes
segs = []
for r in rows:
    lab = r["label"]
    if not segs or segs[-1][0] != lab:
        segs.append([lab, r["wall_hms"], 1, []])
    else:
        segs[-1][2] += 1
    segs[-1][3].append(r)
print("\nsegments (label, from, rows, fset set, i range):")
for lab, t0, n, rs in segs:
    fs = [float(r["fset"]) for r in rs]
    ii = [float(r["i"]) for r in rs]
    n_set = sum(1 for f in fs if f > 0)
    print(f"  {lab or '(transition)':22s} from {t0}  rows={n:3d} "
          f"fset>0 rows={n_set:3d}  fset {min(fs):g}..{max(fs):g}  "
          f"i {min(ii):.3f}..{max(ii):.3f}")
