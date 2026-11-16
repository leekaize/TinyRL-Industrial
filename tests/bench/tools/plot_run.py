#!/usr/bin/env python3
"""Publication figures from an exported web-bench run.

    python3 tests/host-test/webbench/plot_run.py runs/run-<ts>.jsonl [--pdf]

Per node, top to bottom:
  1. strips: the machine truth (MARK rows) vs the node's own recognised
     condition (S-row cond) - "what happened" vs "what it thought"
  2. score vs theta (log scale) with cal/fit bands, alarm + episode
     spans, fault-injection markers
  3. one row per feature: the condition-space position rel(f, x) =
     log2(x / CAL-mean), in decades (clamped +-6), with every learned
     condition's centroid drawn as a dashed line (LIBC rows) - the
     multi-centroid geometry across time
Saves PNG next to the run file; --pdf adds a vector copy for the paper.
"""
import argparse
import json
import math
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Patch

BAND = {"cal": "#a8dadc", "fit": "#ffe0ac"}
ALARM = "#ff5d5d"
EP = "#ff9f1c"
UNKNOWN = "#d62728"
TRUTH_FAULT = "#e15759"
TRUTH_NORMAL = "#8a9bb0"
PALETTE = ["#4c9aff", "#ff9f1c", "#2ec4b6", "#e36588", "#9b5de5",
           "#43aa8b", "#f9c74f", "#8a5fbf", "#90be6d", "#5f6caf"]

FEAT_COLORS = ["#4c9aff", "#f9c74f", "#2ec4b6", "#e36588",
               "#9b5de5", "#43aa8b", "#f8961e", "#90be6d"]


def cond_color(slug):
    if slug == "unknown":
        return UNKNOWN
    return PALETTE[sum(ord(ch) for ch in slug) % len(PALETTE)]


def runs(rows, key, want):
    out, start, prev = [], None, None
    for r in rows:
        cur = r[key] == want
        if cur and start is None:
            start = r["step"]
        if not cur and start is not None:
            out.append((start, prev))
            start = None
        prev = r["step"]
    if start is not None:
        out.append((start, prev))
    return out


def name_spans(rows, key):
    """Contiguous (value, start, end) spans of rows[key]."""
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


def load(path):
    meta, srows, events, marks = {}, [], [], []
    pend, libs = [], []
    with open(path) as f:
        for line in f:
            d = json.loads(line)
            t = d.get("type")
            if t == "meta":
                meta = d
            elif t == "S":
                srows.append(d)
            elif t == "EV":
                events.append((d["step"], d["text"]))
            elif t == "MARK":
                marks.append((d["step"], d["name"]))
            elif t == "PEND":
                pend.append(d)
            elif t == "LIBC":
                libs.append(d)
    return meta, srows, events, sorted(marks), pend, libs


def draw_strips(ax, cond_spans, mark_spans, x0, x1):
    for name, a, b in cond_spans:
        ax.broken_barh([(a, max(b - a, 1))], (0.68, 0.62),
                       facecolors=cond_color(name))
        if b - a > (x1 - x0) * 0.045:
            ax.text((a + b) / 2, 0.99, name, rotation=0, fontsize=6,
                    ha="center", va="top",
                    color="black" if name != "unknown" else "white")
    for name, a, b in mark_spans:
        col = TRUTH_FAULT if name in ("imbalance", "phase_loss") \
            else TRUTH_NORMAL
        ax.broken_barh([(a, max(b - a, 1))], (0.04, 0.62), facecolors=col)
        if b - a > (x1 - x0) * 0.045:
            ax.text((a + b) / 2, 0.34, name, rotation=0, fontsize=6,
                    ha="center", va="center", color="white")
    ax.set_ylim(0, 1.35)
    ax.set_yticks([0.35, 1.0])
    ax.set_yticklabels(["truth", "node says"], fontsize=7)
    ax.set_xticklabels([])


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("run", help="runs/run-<ts>.jsonl from the web bench export")
    ap.add_argument("--pdf", action="store_true", help="also save vector PDF")
    args = ap.parse_args()

    meta, srows, events, marks, pend, libcs = load(args.run)
    n_nodes = meta.get("node_types") and len(meta["node_types"]) or 1
    fnames = meta.get("feature_names", {})
    funits = meta.get("feature_units", {})
    mode = meta.get("mode", "two-way")

    # machine-truth segments from the MARK timeline
    mark_spans = []
    for i, (s, name) in enumerate(marks):
        end = marks[i + 1][0] if i + 1 < len(marks) else 10 ** 9
        mark_spans.append((name, s, end))

    for i in range(n_nodes):
        rows = [r for r in srows if r["node"] == i]
        if not rows:
            continue
        ntype = (meta.get("node_types") or ["node"])[i]
        names = fnames.get(ntype, [f"f{k}" for k in
                                   range(len(rows[0]["feat"]))])
        units = funits.get(ntype, [""] * len(names))
        nf = len(names)
        xs = [r["step"] for r in rows]
        x0, x1 = xs[0], xs[-1]

        # CAL mean per feature = the condition-space anchor mu
        cal = [r for r in rows if r["phase"] == "cal"] or rows[:30]
        mu = []
        for k in range(nf):
            mu.append(sum(r["feat"][k] for r in cal) / max(len(cal), 1))
        mu = [m if m > 1e-9 else 1e-9 for m in mu]   # guard log2 (a CAL
                                                     # mean may be ~0)

        node_libs = [d for d in libcs if d["node"] == i]
        cond_spans = name_spans(rows, "cond")
        node_marks = [(n, a, min(b, x1)) for (n, a, b) in mark_spans
                      if b > x0]

        fig = plt.figure(figsize=(9, 4.2 + 0.62 * nf), dpi=150)
        gs = fig.add_gridspec(nrows=2 + nf, ncols=1,
                              height_ratios=[0.55, 1.6] + [0.62] * nf,
                              hspace=0.14)

        # 1) truth vs recognised strips
        ax_t = fig.add_subplot(gs[0])
        draw_strips(ax_t, cond_spans, node_marks, x0, x1)
        ax_t.set_xlim(x0, x1)
        ax_t.set_title(f"node {i} ({ntype}) [{mode}]", fontsize=10,
                       loc="left")
        handles = [Patch(facecolor=cond_color(c["slug"]),
                         ls="-" if c["kind"] == "fault" else ":",
                         label=c["slug"]) for c in node_libs]
        if handles:
            ax_t.legend(handles=handles, fontsize=6, ncol=min(len(handles), 5),
                        loc="upper right", framealpha=0.85)

        # 2) score vs theta
        ax_s = fig.add_subplot(gs[1], sharex=ax_t)
        for a, b in runs(rows, "phase", "cal"):
            ax_s.axvspan(a, b, color=BAND["cal"], alpha=0.30)
        for a, b in runs(rows, "phase", "fit"):
            ax_s.axvspan(a, b, color=BAND["fit"], alpha=0.35)
        for a, b in runs(rows, "phase", "alarm"):
            ax_s.axvspan(a, b, color=ALARM, alpha=0.25)
        for a, b in runs(rows, "ep", 1):
            ax_s.axvspan(a, b, color=EP, alpha=0.12)
        for step, name in marks:
            if name in ("imbalance", "phase_loss"):
                ax_s.axvline(step, color="#d62728", ls=":", lw=1.0)
        th = [r["theta"] if r["theta"] > 0 else math.nan for r in rows]
        ax_s.plot(xs, [max(r["score"], 1e-3) for r in rows], color="#4c9aff",
                  lw=1.1, label="score (EMA of surprise)")
        ax_s.plot(xs, th, color="#ff6b6b", ls="--", lw=1.1,
                  label=r"$\theta = \mu + 4\sigma$")
        ax_s.set_yscale("log")
        ax_s.set_ylabel("score / $\\theta$", fontsize=8)
        ax_s.grid(alpha=0.3)
        ax_s.legend(fontsize=7, loc="lower right")
        ax_s.set_xticklabels([])
        asks = sum(1 for s, t in events if "open cond=unknown" in t)
        ax_s.text(0.995, 0.95, f"asks: {asks}", transform=ax_s.transAxes,
                  fontsize=7, ha="right", va="top", color="#444444")

        # 3) condition-space rows (rel = log2(x/mu) decades) + centroids
        for k in range(nf):
            ax_f = fig.add_subplot(gs[2 + k], sharex=ax_t)
            rel = [max(-6.0, min(6.0, math.log2(max(r["feat"][k], 1e-12)
                                                    / mu[k]))) for r in rows]
            ax_f.plot(xs, rel, lw=1.0,
                      color=FEAT_COLORS[k % len(FEAT_COLORS)])
            for c in node_libs:
                ax_f.axhline(c["c"][k], ls="--" if c["kind"] == "fault"
                             else ":", lw=1.0, color=cond_color(c["slug"]),
                             alpha=0.9)
            for a, b in runs(rows, "phase", "alarm"):
                ax_f.axvspan(a, b, color=ALARM, alpha=0.15)
            ax_f.set_ylabel(f"{names[k]}\n[{units[k]}] rel", fontsize=7)
            ax_f.set_ylim(-6.2, 6.2)
            ax_f.grid(alpha=0.25)
            if k < nf - 1:
                ax_f.set_xticklabels([])
            else:
                ax_f.set_xlabel("monitor step", fontsize=8)

        stem = Path(args.run).with_suffix("")
        out_png = f"{stem}-node{i}.png"
        fig.savefig(out_png)
        if args.pdf:
            fig.savefig(f"{stem}-node{i}.pdf")
        plt.close(fig)
        print("wrote", out_png, "(+pdf)" if args.pdf else "")


if __name__ == "__main__":
    main()
