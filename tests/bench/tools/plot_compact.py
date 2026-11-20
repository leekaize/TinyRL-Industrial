#!/usr/bin/env python3
"""Compact single-node run figure for the paper (replaces the tall
per-node plot_run figure): truth/recognition strip, ALL feature
positions in ONE axes (anchored rel decades), score vs theta with ask
and label markers.

    python3 tests/bench/tools/plot_compact.py runs/run-<ts>.jsonl \
            [--node 1] [--out assets/run-vib-compact.png] [--pdf]
"""
import argparse
import math
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

from plot_run import (ALARM, BAND, FEAT_COLORS, TRUTH_FAULT,
                      TRUTH_NORMAL, UNKNOWN, load, name_spans, runs)

ABBREV = {"op_stopped": "stopped", "op_speed_1": "speed_1",
          "op_speed_2": "speed_2"}


def clean_spans(spans, min_len=3, merge_gap=10):
    """Drop transition flicker (spans shorter than min_len) and merge
    same-value spans separated by less than merge_gap steps."""
    spans = [(v, a, b) for (v, a, b) in spans if b - a >= min_len]
    out = []
    for v, a, b in spans:
        if out and out[-1][0] == v and a - out[-1][2] <= merge_gap:
            out[-1] = (v, out[-1][1], b)
        else:
            out.append((v, a, b))
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("run", help="runs/run-<ts>.jsonl from the web bench export")
    ap.add_argument("--node", type=int, default=1, help="node index (default 1 = vib)")
    ap.add_argument("--out", default=None, help="output PNG path")
    ap.add_argument("--pdf", action="store_true")
    args = ap.parse_args()

    meta, srows, events, marks, pend, libcs = load(args.run)
    i = args.node
    ntype = (meta.get("node_types") or ["node"])[i]
    fnames = meta.get("feature_names", {})
    names = fnames.get(ntype, [])
    nf = len(names)

    rows = [r for r in srows if r["node"] == i]
    xs = [r["step"] for r in rows]
    x0, x1 = xs[0], xs[-1]

    # CAL mean per feature (the condition-space anchor mu) + the TRUE
    # anchors from the LIBX dump, same as plot_run.py
    cal = [r for r in rows if r["phase"] == "cal"] or rows[:30]
    mu = [sum(r["feat"][k] for r in cal) / max(len(cal), 1) for k in range(nf)]
    mu = [m if m > 1e-9 else 1e-9 for m in mu]
    anchors = None
    for d in libcs:
        if d.get("node") == i and d.get("anchor"):
            anchors = d["anchor"]
            break

    def rel(k, r):
        a = max((anchors or [0] * nf)[k], 1e-9)
        lo = -a * 0.999
        return max(-9.0, min(9.0, math.log2((max(r["feat"][k], lo) + a)
                                            / (mu[k] + a))))

    cond_spans = clean_spans(name_spans(rows, "cond"))
    mark_spans = []
    for j, (s, name) in enumerate(marks):
        end = marks[j + 1][0] if j + 1 < len(marks) else 10 ** 9
        mark_spans.append((name, s, min(end, x1)))
    ask_steps = [s for (s, t) in events
                 if "open cond=unknown" in t and s >= x0]
    label_steps = [s for (s, t) in events if "'label'" in t and s >= x0]

    fig = plt.figure(figsize=(8.6, 4.9), dpi=150)
    gs = fig.add_gridspec(nrows=3, ncols=1, height_ratios=[0.55, 1.7, 1.7],
                          hspace=0.16)

    # 1) truth vs recognised strips; the node row reads by KIND so it
    # needs no legend: blue = named normal, orange = named fault,
    # red = unknown (the ask), gray = commissioning
    KIND_COL = {"normal": "#7fb3d5", "fault": "#ff9f1c",
                "unknown": UNKNOWN, "other": "#c9ccd1"}

    def kind_of(slug):
        if slug in ("unknown", "unidentified"):
            return "unknown"
        if "imbalance" in slug or "phase_loss" in slug:
            return "fault"
        if slug in ("learning", "normal", "idle"):
            return "other"
        return "normal"

    ax_t = fig.add_subplot(gs[0])
    for name, a, b in cond_spans:
        ax_t.axvspan(a, max(b, a + 1), ymin=0.52, ymax=1.0,
                     color=KIND_COL[kind_of(name)], alpha=0.85)
        if b - a > (x1 - x0) * 0.05:
            ax_t.text((a + b) / 2, 0.76, ABBREV.get(name, name), fontsize=5.5,
                      ha="center", va="center", color="white")
    for name, a, b in mark_spans:
        col = TRUTH_FAULT if name in ("imbalance", "phase_loss") \
            else TRUTH_NORMAL
        ax_t.axvspan(a, max(b, a + 1), ymin=0.0, ymax=0.48, color=col,
                     alpha=0.55)
        if b - a > (x1 - x0) * 0.05:
            ax_t.text((a + b) / 2, 0.24, ABBREV.get(name, name), fontsize=5.5,
                      ha="center", va="center", color="white")
    ax_t.set_ylim(0, 1.15)
    ax_t.set_yticks([0.24, 0.76])
    ax_t.set_yticklabels(["truth", "node says"], fontsize=7)
    ax_t.set_xticklabels([])
    ax_t.set_xlim(x0, x1)
    ax_t.set_title(f"vibration node ({ntype}), two-way tour",
                   fontsize=10, loc="left")

    # 2) ALL features in one axes: anchored rel decades, 0 = CAL baseline
    ax_f = fig.add_subplot(gs[1], sharex=ax_t)
    for k in range(nf):
        ax_f.plot(xs, [rel(k, r) for r in rows], lw=0.9,
                  color=FEAT_COLORS[k % len(FEAT_COLORS)],
                  label=f"{names[k]}")
    for a, b in runs(rows, "phase", "alarm"):
        ax_f.axvspan(a, b, color=ALARM, alpha=0.12)
    for step, name in marks:
        if name in ("imbalance", "phase_loss"):
            ax_f.axvline(step, color="#d62728", ls=":", lw=0.9)
    ax_f.axhline(0, color="#bbbbbb", lw=0.7)
    ax_f.set_ylabel("position (decades)", fontsize=8)
    ax_f.set_ylim(-2.4, 2.4)
    ax_f.grid(alpha=0.25)
    ax_f.legend(fontsize=6, ncol=nf, loc="upper left", framealpha=0.85)
    ax_f.set_xticklabels([])

    # 3) score vs fit theta, asks and labels marked
    ax_s = fig.add_subplot(gs[2], sharex=ax_t)
    for a, b in runs(rows, "phase", "cal"):
        ax_s.axvspan(a, b, color=BAND["cal"], alpha=0.25)
    for a, b in runs(rows, "phase", "fit"):
        ax_s.axvspan(a, b, color=BAND["fit"], alpha=0.30)
    for a, b in runs(rows, "phase", "alarm"):
        ax_s.axvspan(a, b, color=ALARM, alpha=0.22)
    ax_s.plot(xs, [max(r["score"], 1e-3) for r in rows], color="#4c9aff",
              lw=1.0, label="score (EMA of surprise)")
    th = [r["theta"] if r["theta"] > 0 else math.nan for r in rows]
    ax_s.plot(xs, th, color="#ff6b6b", ls="--", lw=1.1,
              label=r"fit $\theta$ (gates unknown states)")
    for s in ask_steps:
        ax_s.axvline(s, color="#d62728", ls=":", lw=1.0)
    for s in label_steps:
        ax_s.plot([s], [ax_s.get_ylim()[1]], marker="v", ms=4,
                  color="#2ec4b6", mec="none", clip_on=False)
    ax_s.set_yscale("log")
    ax_s.set_ylabel("score / $\\theta$", fontsize=8)
    ax_s.set_xlabel("monitor step  (node says: blue = named normal, orange = "
                    "named fault, red = unknown ask; red dotted = fault "
                    "inject; red span = alarm; green marker = label)",
                    fontsize=7)
    ax_s.grid(alpha=0.3)
    ax_s.legend(fontsize=7, loc="upper left")
    ax_s.text(0.995, 0.95, f"asks: {len(ask_steps)}  labels: {len(label_steps)}",
              transform=ax_s.transAxes, fontsize=7, ha="right", va="top",
              color="#444444")

    out = args.out or str(Path(args.run).with_suffix("")) + f"-node{i}-compact.png"
    fig.tight_layout()
    fig.savefig(out)
    if args.pdf:
        fig.savefig(str(Path(out).with_suffix(".pdf")))
    plt.close(fig)
    print("wrote", out, "(+pdf)" if args.pdf else "")


if __name__ == "__main__":
    main()
