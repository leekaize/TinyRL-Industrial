#!/usr/bin/env python3
"""Condition-space geometry dump from an exported web-bench run.

    python3 tests/host-test/webbench/diag_space.py runs/run-<ts>.jsonl

Replicates the node's math on the run's own data to explain WHY states
match or veto:
  - CAL-phase S rows -> per-feature mu, sigma -> anchor A = 6*sigma and
    the CAL fallback unit (feature_space.h arm())
  - LIBC/LIBU rows -> stored centroids and each condition's own units
  - S rows grouped by MACHINE TRUTH (MARK segments) -> the live rel
    position of every true state (cond spans would be polluted by
    run-on episodes)
  - per (truth state, centroid) pair: the MAX-component margin
    max_f |rel(f) - c(f)| / unit(f)  vs  q (3 quiet / 4 match)
    margin <= q means the state MATCHES that centroid (quiet/no ask)
"""
import json
import math
import sys

REL_CLAMP = 12.0
QUIET_Q = 3.0
MATCH_Q = 4.0
SCORE_FLOOR = 1e-4
UNIT_MIN = 0.05
FAULTS = ("imbalance", "phase_loss")


def rel(mu, anchor, x):
    denom = abs(mu) + anchor
    num = max(x + anchor, denom * 1e-3)
    d = math.log2(num / denom)
    return max(-REL_CLAMP, min(REL_CLAMP, d))


def spans(rows, key):
    out = []
    if not rows:
        return out
    start, cur, prev = rows[0]["step"], rows[0][key], rows[0]["step"]
    for r in rows[1:]:
        if r[key] != cur:
            out.append((cur, start, prev))
            cur, start = r[key], r["step"]
        prev = r["step"]
    out.append((cur, start, prev))
    return out


def main() -> None:
    path = sys.argv[1]
    srows, libcs, marks, n_nodes = [], [], [], 1
    with open(path) as f:
        for line in f:
            d = json.loads(line)
            if d["type"] == "S":
                srows.append(d)
            elif d["type"] == "LIBC":
                libcs.append(d)
            elif d["type"] == "MARK":
                marks.append((d["step"], d["name"]))
            elif d["type"] == "meta":
                n_nodes = len(d.get("node_types") or [1])
    marks.sort()
    for i in range(n_nodes):
        rows = [r for r in srows if r["node"] == i]
        if not rows:
            continue
        nf = len(rows[0]["feat"])
        names = rows[0].get("feature_names") or [f"f{k}" for k in range(nf)]
        cal = [r for r in rows if r["phase"] == "cal"] or rows[:30]
        mu, anchor, unit = [], [], []
        for k in range(nf):
            vals = [r["feat"][k] for r in cal]
            m = sum(vals) / len(vals)
            sd = (sum((v - m) ** 2 for v in vals) / max(len(vals) - 1, 1)) ** 0.5
            a = max(6.0 * sd, SCORE_FLOOR)
            mu.append(m)
            anchor.append(a)
            unit.append(max(math.log2(1.0 + sd / (abs(m) + a)), 0.05))

        libs = {d["slug"]: d for d in libcs if d["node"] == i}
        print(f"\n=== node {i} ===")
        print("CAL moments -> anchor A, fallback unit (decades):")
        for k in range(nf):
            print(f"  {names[k]:>6}: mu={mu[k]:<12.6g} A={anchor[k]:.4g} "
                  f"unit={unit[k]:.3f}")
        if libs:
            print("stored conditions (centroid rel decades | own units):")
            for slug, d in libs.items():
                cc = " ".join(f"{v:6.2f}" for v in d["c"])
                uu = " ".join(f"{v:5.3g}" for v in d.get("u", [])) \
                     if d.get("u") else "  (CAL fallback)"
                print(f"  {slug:>18}: [{cc}]  [{uu}]")

        # live positions grouped by MACHINE TRUTH (MARK segments)
        states = {}
        for si, (s, name) in enumerate(marks):
            end = marks[si + 1][0] if si + 1 < len(marks) else 10 ** 9
            if end - s < 10:
                continue
            if name in FAULTS:
                # compound with the running setpoint at injection time
                speed = "stopped"
                for ps, pn in marks[:si]:
                    if pn.startswith("op_speed"):
                        speed = pn.replace("op_speed", "speed")
                    elif pn == "op_stopped":
                        speed = "stopped"
                truth = (speed + "_" + name if speed != "stopped"
                         else "stopped_" + name)
            else:
                truth = name
            seg = [r for r in rows if s <= r["step"] < end
                   and r["phase"] in ("mon", "alarm")]
            if len(seg) < 10:
                continue
            pos = []
            for k in range(nf):
                vals = sorted(rel(mu[k], anchor[k], r["feat"][k]) for r in seg)
                pos.append(vals[len(vals) // 2])
            states[truth] = pos
        if not states or not libs:
            print("not enough data for margins")
            continue
        print("match margins (max_f |rel-c|/unit; <=3 quiet, <=4 match):")
        hdr = "  " + " " * 20 + "".join(f"{s:>16}" for s in libs)
        print(hdr)
        for tname, pos in states.items():
            cells = []
            for slug, d in libs.items():
                margins = []
                for k in range(nf):
                    u = d.get("u") or []
                    uf = u[k] if k < len(u) and u[k] > 0 else unit[k]
                    uf = max(uf, UNIT_MIN)
                    margins.append(abs(pos[k] - d["c"][k]) / uf)
                m = max(margins)
                worst = names[margins.index(m)]
                mark = "MATCH" if m <= QUIET_Q else "veto"
                cells.append(f"{m:6.1f}{mark[:3]}({worst[:4]})")
            print(f"  {tname:>20}:" + "".join(f"{x:>16}" for x in cells))


if __name__ == "__main__":
    main()
