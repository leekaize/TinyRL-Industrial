#!/usr/bin/env python3
"""Probe 2: where did the labeled fault conditions go, and does the owner
node ever show phase=alarm / cond=*_phase_loss in the S rows?"""
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
lo = int(sys.argv[2]) if len(sys.argv) > 2 else 0
hi = int(sys.argv[3]) if len(sys.argv) > 3 else 10**9

srows = {0: [], 1: []}
events = {}
with open(path) as f:
    for line in f:
        d = json.loads(line)
        if d["type"] == "S":
            srows[d["node"]].append(d)
        elif d["type"] == "EV":
            events.setdefault(d["step"], []).append(d["text"])

for node in (0, 1):
    rows = srows[node]
    n_alarm = sum(1 for r in rows if r["phase"] == "alarm")
    conds = {}
    for r in rows:
        conds.setdefault(r["cond"], [r["step"], r["step"]])
        conds[r["cond"]][1] = r["step"]
    print(f"node {node}: rows={len(rows)} phase=alarm rows={n_alarm}")
    for c, (a, b) in sorted(conds.items(), key=lambda kv: kv[1][0]):
        print(f"   cond {c!r:28s} steps {a}..{b}")
print()

r0 = srows[0]
print(f"== node 0 dense S rows {lo}..{hi} ==")
for r in r0:
    if lo <= r["step"] < hi:
        print(f"S {r['step']:5d} {r['phase']:9s} cond={r['cond'][:24]:24s} "
              f"score={r['score']:8.3f} acc={r['acc']:6.2f} ep={r['ep']} "
              f"ragg={r['ragg']:9.2f} gate_r={r['gate_r']:7.2f}")
        for t in events.get(r["step"], []):
            if any(k in t for k in ("[WHY]", "[RL]", "[EP]", "[PEND]", "[FB]",
                                    "[SHARE]")):
                print(f"   EV  {t}")
