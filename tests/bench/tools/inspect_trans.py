#!/usr/bin/env python3
"""Dump the collect CSV's transition segments row by row (fset set but
current still at the stopped baseline = the knob-turned-no-RUN rows)."""
import csv
from pathlib import Path

path = Path("data/serial/collect_20261121_211534.csv")
rows = list(csv.DictReader(path.open()))

segs = []
for idx, r in enumerate(rows):
    lab = r["label"]
    if not segs or segs[-1][0] != lab:
        segs.append([lab, idx, []])
    segs[-1][2].append(idx)

for lab, start, idxs in segs:
    if lab:            # settled condition blocks printed separately
        continue
    print(f"--- transition from row {start} ({len(idxs)} rows) ---")
    for i in idxs:
        r = rows[i]
        f, ii = float(r["fset"]), float(r["i"])
        mark = "  <== set-not-running" if f > 0 and ii < 0.3 else ""
        print(f"  {i:4d} {r['wall_hms']} fset={f:g} i={ii:.4f} pf={float(r['pf']):.4f}{mark}")
