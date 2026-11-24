#!/usr/bin/env python3
"""Publication figures from an exported web-bench run.

    python3 tests/bench/tools/plot_run.py runs/run-<ts>.jsonl [--pdf]

Per node, top to bottom:
  1. strips: the machine truth (MARK rows) vs the node's own recognised
     condition (S-row cond) - "what happened" vs "what it thought"
  2. score vs thresholds (log scale): the GLOBAL fit theta (it gates
     unknown states) and the EFFECTIVE threshold - a named condition is
     judged against its own per-condition band (LIBX rows); known spans
     shaded, alarm + episode spans, fault-injection markers
  3. one row per feature: the condition-space position rel(f, x) =
     log2((x+A)/(mu+A)) decades in the TRUE anchored coordinates (A from
     the LIBX anchors, exactly what the node matches against), with every
     learned condition's centroid as a dashed line (LIBC rows)
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
    # deterministic PER-NODE assignment happens in main() (sorted slugs ->
    # palette); the hash here is only the fallback for standalone use
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
                marks.append((d["step"], d["name"], d.get("ctx", "")))
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
    for i, (s, name, _ctx) in enumerate(marks):
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

        # LIBX rows (merged into the lib entries by app.py) carry each
        # condition's own score threshold and the feature anchors of the
        # TRUE condition space rel(f, x) = log2((x+A)/(mu+A)). Older
        # exports without them fall back to the bare ratio.
        anchors = None
        for d in libcs:
            if d.get("node") == i and d.get("anchor"):
                anchors = d["anchor"]
                break
        cond_theta = {}
        for d in libcs:
            if d.get("node") == i and d.get("theta") is not None:
                cond_theta[d["slug"]] = d["theta"]
        lo_ref = 0.0   # rug ylim floor reference (centroids may sit below)

        node_libs = [d for d in libcs if d["node"] == i]
        # dedupe by slug keeping the LAST dump (older exports predate the
        # app.py LIBC upsert and carry one entry per dump)
        _by_slug = {}
        for d in node_libs:
            _by_slug[d["slug"]] = d
        node_libs = list(_by_slug.values())
        cond_spans = name_spans(rows, "cond")
        node_marks = [(n, a, min(b, x1)) for (n, a, b) in mark_spans
                      if b > x0]
        known_spans = [(a, b) for (name, a, b) in cond_spans
                       if name not in ("unknown", "unidentified", "normal")]

        # deterministic color per condition: sorted slugs -> palette (the
        # hash collided two conditions into one color and made the strips
        # unreadable, 2026-11-19)
        slugs = sorted({d["slug"] for d in node_libs} |
                       {n for (n, _a, _b) in cond_spans} |
                       set(cond_theta))
        colof = {"unknown": UNKNOWN}
        for j, s in enumerate([s for s in slugs if s != "unknown"]):
            colof[s] = PALETTE[j % len(PALETTE)]

        fig = plt.figure(figsize=(9, 4.2 + 0.62 * nf), dpi=150)
        gs = fig.add_gridspec(nrows=2 + nf, ncols=1,
                              height_ratios=[0.55, 1.6] + [0.62] * nf,
                              hspace=0.14)

        # 1) truth vs recognised strips
        ax_t = fig.add_subplot(gs[0])
        for name, a, b in cond_spans:
            ax_t.broken_barh([(a, max(b - a, 1))], (0.68, 0.62),
                             facecolors=colof.get(name, UNKNOWN))
            if b - a > (x1 - x0) * 0.045:
                ax_t.text((a + b) / 2, 0.99, name, rotation=0, fontsize=6,
                          ha="center", va="top",
                          color="black" if name != "unknown" else "white")
        for name, a, b in node_marks:
            col = TRUTH_FAULT if name in ("imbalance", "phase_loss") \
                else TRUTH_NORMAL
            ax_t.broken_barh([(a, max(b - a, 1))], (0.04, 0.62), facecolors=col)
            if b - a > (x1 - x0) * 0.045:
                ax_t.text((a + b) / 2, 0.34, name, rotation=0, fontsize=6,
                          ha="center", va="center", color="white")
        ax_t.set_ylim(0, 1.35)
        ax_t.set_yticks([0.35, 1.0])
        ax_t.set_yticklabels(["truth", "node says"], fontsize=7)
        ax_t.set_xticklabels([])
        ax_t.set_xlim(x0, x1)
        ax_t.set_title(f"node {i} ({ntype}) [{mode}]", fontsize=10,
                       loc="left")
        handles = [Patch(facecolor=colof.get(c["slug"], UNKNOWN),
                         ls="-" if c["kind"] == "fault" else ":",
                         label=c["slug"]) for c in node_libs]
        if handles:
            ax_t.legend(handles=handles, fontsize=6,
                        ncol=min(len(handles), 5),
                        loc="upper right", framealpha=0.85)

        # 2) score vs thresholds - the panel's definition: does the score
        # cross the threshold THAT APPLIES, and is the state known (quiet
        # is positional there) or unknown (the fit theta gates it)?
        ax_s = fig.add_subplot(gs[1], sharex=ax_t)
        for a, b in runs(rows, "phase", "cal"):
            ax_s.axvspan(a, b, color=BAND["cal"], alpha=0.25)
        for a, b in runs(rows, "phase", "fit"):
            ax_s.axvspan(a, b, color=BAND["fit"], alpha=0.30)
        for a, b in runs(rows, "phase", "alarm"):
            ax_s.axvspan(a, b, color=ALARM, alpha=0.22)
        for a, b in known_spans:
            ax_s.axvspan(a, max(b, a + 1), color="#2ec4b6", alpha=0.10)
        for step, name, _ctx in marks:
            if name in ("imbalance", "phase_loss"):
                ax_s.axvline(step, color="#d62728", ls=":", lw=1.0)
        th = [r["theta"] if r["theta"] > 0 else math.nan for r in rows]
        eff = []
        for r in rows:
            t = cond_theta.get(r["cond"])
            eff.append(t if (t and t > 0) else
                       (r["theta"] if r["theta"] > 0 else math.nan))
        ax_s.plot(xs, [max(r["score"], 1e-3) for r in rows], color="#4c9aff",
                  lw=1.1, label="score (EMA of surprise)")
        ax_s.plot(xs, th, color="#ff6b6b", ls="--", lw=1.1,
                  label="fit $\\theta$ (global, gates unknown states)")
        if any(not math.isnan(v) for v in eff):
            ax_s.plot(xs, eff, color="#b23a48", ls="-.", lw=1.0,
                      label="effective threshold (per-condition band)")
        ax_s.set_yscale("log")
        ax_s.set_ylabel("score / $\\theta$", fontsize=8)
        ax_s.grid(alpha=0.3)
        ax_s.legend(fontsize=7, loc="lower right")
        ax_s.set_xticklabels([])
        asks = sum(1 for s, t in events if "open cond=unknown" in t)
        ax_s.text(0.995, 0.95, f"asks: {asks}", transform=ax_s.transAxes,
                  fontsize=7, ha="right", va="top", color="#444444")

        # 3) condition-space rows in the TRUE anchored coordinates
        # rel(f, x) = log2((x+A)/(mu+A)) decades; per-condition centroids
        # drawn as a RUG on the right edge (full-width lines for every
        # condition turned each row into spaghetti)
        for k in range(nf):
            ax_f = fig.add_subplot(gs[2 + k], sharex=ax_t)
            if anchors:
                a = max(anchors[k], 1e-9)
                rel = [max(-9.0, min(9.0,
                           math.log2((max(r["feat"][k], -a * 0.999) + a)
                                     / (mu[k] + a)))) for r in rows]
            else:
                rel = [max(-6.0, min(6.0, math.log2(max(r["feat"][k], 1e-12)
                                                    / mu[k]))) for r in rows]
            ax_f.plot(xs, rel, lw=1.0,
                      color=FEAT_COLORS[k % len(FEAT_COLORS)])
            # rug rail: per-condition centroids as squares on the right
            # edge - full-width lines for every condition turned each row
            # into spaghetti
            rail_x = x1 + (x1 - x0) * 0.008
            cent = [c["c"][k] for c in node_libs]
            ax_f.axvline(rail_x, color="#d0d0d0", lw=2)
            lo = min(min(rel), min(cent + [lo_ref])) if cent else min(rel)
            hi = max(max(rel), max(cent + [lo_ref])) if cent else max(rel)
            pad = 0.12 * (hi - lo + 1.0)
            ax_f.set_ylim(lo - pad, hi + pad)
            for c in node_libs:
                ax_f.plot([rail_x], [c["c"][k]],
                          marker="s", ms=5, mec="none",
                          color=colof.get(c["slug"], UNKNOWN),
                          label=c["slug"] if k == 0 else None)
            for a, b in runs(rows, "phase", "alarm"):
                ax_f.axvspan(a, b, color=ALARM, alpha=0.12)
            ax_f.set_ylabel(f"{names[k]}", fontsize=7)
            ax_f.grid(alpha=0.25)
            ax_f.set_xlim(x0, x1 + (x1 - x0) * 0.02)
            if k < nf - 1:
                ax_f.set_xticklabels([])
            else:
                ax_f.set_xlabel("monitor step  "
                                "(anchored rel decades; squares on the "
                                "right edge = condition centroids)",
                                fontsize=8)
            if k == 0 and node_libs:
                ax_f.legend(fontsize=6, ncol=min(len(node_libs), 4),
                            loc="upper left", framealpha=0.85)

        stem = Path(args.run).with_suffix("")
        out_png = f"{stem}-node{i}.png"
        fig.savefig(out_png)
        if args.pdf:
            fig.savefig(f"{stem}-node{i}.pdf")
        plt.close(fig)
        print("wrote", out_png, "(+pdf)" if args.pdf else "")


if __name__ == "__main__":
    main()
