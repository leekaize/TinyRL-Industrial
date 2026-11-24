#!/usr/bin/env python3
"""Per-condition detection figure for the three-era A/B (2026-11-21).

Consumes the stage-H HTRACE logs of the three host builds (same monitor,
same fixture, three representations: z / rel / cond) and renders one panel
block per era:

  - score vs its ACTIVE gate: the condition's own surprise band once
    mature, the global theta otherwise (grace spans shaded, gate = -1);
  - the positional aggregate r_agg vs its band (cond era only): the
    static-offset evidence the one-step predictor is blind to;
  - the evidence accumulator with its trigger (ACC_ALARM = 5.0) and clear
    (ACC_CLEAR = 1.0) levels from node_config.h.

Alarm spans (red) and fixture phases (alternating shading, tagged) are
overlaid, so the stage-H verdict - z misses, rel misses, cond catches -
is readable directly off the figure. matplotlib is the only dependency.

    python3 tests/bench/tools/plot_percond_trace.py /tmp/h_z.log /tmp/h_rel.log \
            /tmp/h_cond.log [--out assets/percond-trace.png] [--pdf]
"""
import argparse
import math
import re
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HTRACE = re.compile(
    r"HTRACE step=(\d+) tag=(\S+) score=(\S+) theta=(\S+) km=(-?\d+) "
    r"gate=(\S+) al=(\d+) bm=(\d+) ragg=(\S+) grate=(\S+) acc=(\S+)")

ACC_ALARM = 5.0    # cfg::ACC_ALARM, node_config.h
ACC_CLEAR = 1.0    # cfg::ACC_CLEAR, node_config.h

ERAS = [
    ("z",    "z era: frozen-z obs, positional latch - MISSES the fault"),
    ("rel",  "rel era: rel levels + per-cond score band - MISSES (interpolation)"),
    ("cond", "cond era: centered residuals + positional band + accumulator - CATCHES"),
]
SCORE_C, GATE_C, THETA_C = "#1f77b4", "#d62728", "#7f7f7f"
RAGG_C, GRATE_C = "#2ca02c", "#9467bd"
ACC_C, ALARM_FACE = "#ff7f0e", "#d62728"


def load(path):
    rows = []
    for line in Path(path).read_text(errors="replace").splitlines():
        m = HTRACE.search(line)
        if not m:
            continue
        rows.append({
            "step": int(m.group(1)), "tag": m.group(2),
            "score": float(m.group(3)), "theta": float(m.group(4)),
            "km": int(m.group(5)), "gate": float(m.group(6)),
            "al": int(m.group(7)), "bm": int(m.group(8)),
            "ragg": float(m.group(9)), "grate": float(m.group(10)),
            "acc": float(m.group(11)),
        })
    return rows


def phase_spans(rows):
    spans = []
    for r in rows:
        if spans and spans[-1][0] == r["tag"]:
            spans[-1][2] = r["step"]
        else:
            spans.append([r["tag"], r["step"], r["step"]])
    return spans


def bool_spans(rows, key, on):
    spans = []
    for r in rows:
        v = r[key] == on if key == "al" else r[key] < 0
        if v and spans and spans[-1][1] >= r["step"] - 1:
            spans[-1][1] = r["step"]
        elif v:
            spans.append([r["step"], r["step"]])
    return spans


def series(rows, key, masked_below=None):
    xs, ys = [], []
    for r in rows:
        v = r[key]
        if masked_below is not None and v < masked_below:
            v = math.nan          # break the line (grace / immature band)
        xs.append(r["step"])
        ys.append(v)
    return xs, ys


def shade(axes, spans, color, alpha):
    for a, b in spans:
        for ax in axes:
            ax.axvspan(a, b, color=color, alpha=alpha, lw=0)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("zlog", help="stage-H HTRACE log, z-era build")
    ap.add_argument("rellog", help="stage-H HTRACE log, rel-era build")
    ap.add_argument("condlog", help="stage-H HTRACE log, cond-era build")
    ap.add_argument("--out", default="assets/percond-trace.png")
    ap.add_argument("--pdf", action="store_true")
    args = ap.parse_args()

    data = {key: load(path) for (key, _), path
            in zip(ERAS, (args.zlog, args.rellog, args.condlog))}

    # one score + one acc axis per era, plus a positional axis for cond
    heights = []
    for key, _ in ERAS:
        heights += [2.4, 1.5] + ([1.5] if key == "cond" else [])
    fig, axes = plt.subplots(len(heights), 1, figsize=(11, 13),
                             gridspec_kw=dict(height_ratios=heights,
                                              hspace=0.30))
    axes = list(axes)
    for ax in axes:
        ax.grid(True, alpha=0.3)

    ai = 0
    for key, title in ERAS:
        rows = data[key]
        ax_s, ax_a = axes[ai], axes[ai + 1]
        ai += 2
        ax_r = None
        if key == "cond":
            ax_r = axes[ai]
            ai += 1

        block = [ax_s, ax_a] + ([ax_r] if ax_r else [])
        phases = phase_spans(rows)
        for i, (tag, a, b) in enumerate(phases):
            for ax in block:
                ax.axvspan(a, b, color="0.55" if i % 2 else "0.85",
                           alpha=0.18, lw=0)
            ax_s.text((a + b) / 2, 0.97, tag, transform=ax_s.get_xaxis_transform(),
                      ha="center", va="top", fontsize=7, color="0.25",
                      rotation=90)

        grace = bool_spans(rows, "gate", None)
        alarm = bool_spans(rows, "al", 1)
        # in the z era gate = -1 means the positional LATCH, not grace -
        # shading it would paint the whole panel amber
        shade(block, alarm, ALARM_FACE, 0.16)
        if key != "z":
            shade([ax_s, ax_a], grace, "#f0c020", 0.25)

        # score vs the ACTIVE gate (per-condition band or global theta);
        # log scale: the label transient lives 1-2 decades above the floor
        xs, sc = series(rows, "score")
        _, gt = series(rows, "gate", masked_below=0)
        _, th = series(rows, "theta")
        ax_s.plot(xs, sc, color=SCORE_C, lw=1.0, label="score")
        ax_s.plot(xs, gt, color=GATE_C, lw=1.2, label="active gate")
        ax_s.plot(xs, th, color=THETA_C, lw=0.8, ls="--", label="global theta")
        ax_s.set_yscale("log")
        ax_s.set_ylim(ymin=1e-4)
        ax_s.set_title(title, fontsize=10, loc="left")
        ax_s.legend(fontsize=7, ncol=3, loc="upper right", framealpha=0.7)
        ax_s.set_ylabel("score (log)")

        if ax_r:
            xs, rg = series(rows, "ragg")
            _, gr = series(rows, "grate", masked_below=0)
            ax_r.plot(xs, rg, color=RAGG_C, lw=1.0, label="r_agg")
            ax_r.plot(xs, gr, color=GRATE_C, lw=1.2, label="positional band")
            ax_r.legend(fontsize=7, ncol=2, loc="upper right", framealpha=0.7)
            ax_r.set_ylabel("positional")

        xs, ac = series(rows, "acc")
        ax_a.plot(xs, ac, color=ACC_C, lw=1.0, label="accumulator")
        ax_a.axhline(ACC_ALARM, color=GATE_C, lw=0.8, ls="--")
        ax_a.axhline(ACC_CLEAR, color="0.5", lw=0.8, ls=":")
        ax_a.text(0.995, ACC_ALARM, " ACC_ALARM", va="center", ha="left",
                  fontsize=7, color=GATE_C, transform=ax_a.get_yaxis_transform())
        ax_a.text(0.995, ACC_CLEAR, " ACC_CLEAR", va="center", ha="left",
                  fontsize=7, color="0.4", transform=ax_a.get_yaxis_transform())
        ax_a.set_ylim(-0.5, 21)
        ax_a.set_ylabel("evidence")

    for ax in (axes[-1],):
        ax.set_xlabel("fixture step")

    fig.suptitle("Per-condition detection, stage-H fixture (one monitor, "
                 "three representations)", fontsize=12, y=0.995)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out, dpi=150, bbox_inches="tight")
    print(f"wrote {out}")
    if args.pdf:
        pdf = out.with_suffix(".pdf")
        fig.savefig(pdf, bbox_inches="tight")
        print(f"wrote {pdf}")


if __name__ == "__main__":
    main()
