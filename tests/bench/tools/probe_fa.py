#!/usr/bin/env python3
"""Probe 3: list every alarm span of a node with the machine truth at the
span start and the cond the node claimed - the false-alarm map."""
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
node = int(sys.argv[2]) if len(sys.argv) > 2 else 0

rows, marks = [], []
with open(path) as f:
    for line in f:
        d = json.loads(line)
        if d["type"] == "S" and d["node"] == node:
            rows.append(d)
        elif d["type"] == "MARK":
            marks.append((d["step"], d["name"]))

def truth_at(step):
    name = "op_stopped"
    for s, n in sorted(marks):
        if s <= step:
            name = n
        else:
            break
    return name

spans, start, prev = [], None, False
for r in rows:
    cur = r["phase"] == "alarm"
    if cur and not prev:
        start = r["step"]
    if not cur and prev:
        spans.append((start, r["step"]))
    prev = cur
if prev and rows:
    spans.append((start, rows[-1]["step"]))

print(f"node {node}: {len(spans)} alarm spans")
for a, b in spans:
    r0 = next(r for r in rows if r["step"] == a)
    print(f"  {a:4d}..{b:<4d} truth={truth_at(a):12s} "
          f"cond={r0['cond']:20s} score={r0['score']:7.2f} "
          f"ragg={r0['ragg']:8.2f} gate_r={r0['gate_r']:7.2f} "
          f"acc={r0['acc']:6.2f}")
