#!/usr/bin/env python3
"""Publication figures from an exported web-bench run.

    python3 tests/host-test/webbench/plot_run.py runs/run-<ts>.jsonl [--pdf]

Renders one figure per node: score vs theta (log scale) with phase bands
(cal/fit), alarm spans and episode spans, then the per-feature inputs
(min-max normalised, raw tick labels in the legend). Saves PNG next to
the run file; --pdf adds a vector copy for the report/paper.
"""
import argparse
import json
import math
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

BAND = {"cal": "#a8dadc", "fit": "#ffe0ac"}
ALARM = "#ff5d5d"
EP = "#ff9f1c"


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


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("run", help="runs/run-<ts>.jsonl from the web bench export")
    ap.add_argument("--pdf", action="store_true", help="also save vector PDF")
    args = ap.parse_args()

    meta, srows, events = {}, [], []
    with open(args.run) as f:
        for line in f:
            d = json.loads(line)
            if d["type"] == "meta":
                meta = d
            elif d["type"] == "S":
                srows.append(d)
            elif d["type"] == "EV":
                events.append((d["step"], d["text"]))

    n_nodes = meta.get("node_types") and len(meta["node_types"]) or 1
    fnames = meta.get("feature_names", {})
    funits = meta.get("feature_units", {})

    for i in range(n_nodes):
        rows = [r for r in srows if r["node"] == i]
        if not rows:
            continue
        ntype = (meta.get("node_types") or ["node"])[i]
        names = fnames.get(ntype, [f"f{k}" for k in
                                   range(len(rows[0]["feat"]))])
        units = funits.get(ntype, [""] * len(names))
        xs = [r["step"] for r in rows]

        fig, (ax_s, ax_f) = plt.subplots(
            2, 1, figsize=(9, 6), dpi=150,
            gridspec_kw={"height_ratios": [1.4, 1.2], "hspace": 0.35})

        for a, b in runs(rows, "phase", "cal"):
            for ax in (ax_s, ax_f):
                ax.axvspan(a, b, color=BAND["cal"], alpha=0.30)
        for a, b in runs(rows, "phase", "fit"):
            for ax in (ax_s, ax_f):
                ax.axvspan(a, b, color=BAND["fit"], alpha=0.35)
        for a, b in runs(rows, "phase", "alarm"):
            for ax in (ax_s, ax_f):
                ax.axvspan(a, b, color=ALARM, alpha=0.25)
        for a, b in runs(rows, "ep", 1):
            ax_s.axvspan(a, b, color=EP, alpha=0.15)

        th = [r["theta"] if r["theta"] > 0 else math.nan for r in rows]
        ax_s.plot(xs, [max(r["score"], 1e-3) for r in rows], color="#4c9aff",
                  lw=1.1, label="score (EMA of surprise)")
        ax_s.plot(xs, th, color="#ff6b6b", ls="--", lw=1.1,
                  label=r"$\theta = \mu + 4\sigma$")
        ax_s.set_yscale("log")
        ax_s.set_ylabel("score / $\\theta$")
        ax_s.grid(alpha=0.3)
        ax_s.legend(fontsize=8, loc="lower right")
        asks = sum(1 for s, t in events if "open cond=unknown" in t)
        ax_s.set_title(f"node {i} ({ntype}): score vs threshold  "
                       f"[asks opened: {asks}]", fontsize=10, loc="left")

        for k in range(len(names)):
            vals = [r["feat"][k] for r in rows]
            lo, hi = min(vals), max(vals)
            rng = (hi - lo) or 1
            ax_f.plot(xs, [(v - lo) / rng for v in vals], lw=1.0,
                     label=f"{names[k]} [{units[k]}]")
        for a, b in runs(rows, "phase", "alarm"):
            ax_f.axvspan(a, b, color=ALARM, alpha=0.20)
        ax_f.set_ylabel("inputs (min-max)")
        ax_f.set_xlabel("monitor step")
        ax_f.grid(alpha=0.3)
        ax_f.legend(fontsize=7, ncol=min(len(names), 3), loc="upper right")

        stem = Path(args.run).with_suffix("")
        out_png = f"{stem}-node{i}.png"
        fig.savefig(out_png)
        if args.pdf:
            fig.savefig(f"{stem}-node{i}.pdf")
        plt.close(fig)
        print("wrote", out_png, "(+pdf)" if args.pdf else "")


if __name__ == "__main__":
    main()
