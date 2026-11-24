#!/usr/bin/env python3
"""Probe: node-0 (vfd) decision trace across the phase_loss windows of a
bench export. Prints S rows (telemetry + raw features) and the [WHY]/[RL]
events interleaved, so the monitor's chain of thought can be followed one
step at a time (2026-11-22d, next-thread VFD gap work).

    python3 tests/bench/tools/probe_vfd.py runs/run-<ts>.jsonl [step0 step1]
"""
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
lo = int(sys.argv[2]) if len(sys.argv) > 2 else 0
hi = int(sys.argv[3]) if len(sys.argv) > 3 else 10**9

events = {}
marks = []
libc = {0: [], 1: []}
srows = []
with open(path) as f:
    for line in f:
        d = json.loads(line)
        if d["type"] == "EV":
            events.setdefault(d["step"], []).append(d["text"])
        elif d["type"] == "MARK":
            marks.append((d["step"], d["name"]))
        elif d["type"] == "LIBC" and d["node"] == 0:
            libc[0].append(d)
        elif d["type"] == "S" and d["node"] == 0:
            srows.append(d)

print("== node 0 library (LIBC c + u) ==")
for e in libc[0]:
    print(f"  {e['slug']:22s} kind={e.get('kind','?'):6s} "
          f"c={['%.3f' % v for v in e['c']]} u={['%.4f' % v for v in e.get('u', [])]}")
print()
print("== marks overlapping [%d,%d) ==" % (lo, hi))
prev = None
for s, n in marks:
    if n != prev and lo - 5 <= s < hi:
        print(f"  step {s}: {n}")
    prev = n
print()
print("== S rows + events, steps %d..%d ==" % (lo, hi))
for r in srows:
    if not (lo <= r["step"] < hi):
        continue
    if r["step"] % 5 and not events.get(r["step"]):
        continue
    print(f"S {r['step']:5d} {r['phase']:9s} cond={r['cond'][:22]:22s} "
          f"score={r['score']:8.3f} gate={r['gate']:7.3f} ragg={r['ragg']:10.2f} "
          f"gate_r={r['gate_r']:8.2f} acc={r['acc']:6.2f} ep={r['ep']} "
          f"feat={['%.4f' % v for v in r['feat']]}")
    for t in events.get(r["step"], []):
        print(f"   EV  {t}")
