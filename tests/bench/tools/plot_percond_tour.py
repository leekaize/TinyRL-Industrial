#!/usr/bin/env python3
"""Per-condition evidence figures for the two-way paper tour (cond era).

The 450-step tour squeezed into one figure hides how the parameters
develop, so the tour is split into STAGES, one figure each (--stages
auto is the default; --stages none restores the old single figure):

  stage 1  commissioning      CAL -> theta fit -> op_stopped baseline;
                              every new machine state is unknown: alarm,
                              ask, operator label, node relearns
  stage 2  first real faults  imbalance (vib detects, vfd stays below),
                              phase_loss (vfd detects; the speed change
                              under the continuing fault teaches a
                              second condition)
  stage 3  return tour        known states revisited: no asks, short
                              faults answered from vocabulary in ~1 step

Per node, top to bottom (shared x = monitor step):

  - strip: machine truth (bottom half) vs what the node says (top half).
    Node says: green = named normal, blue = named stopped, red = named
    fault, amber = alarm open (asking label), gray = commissioning.
    Truth: blue = stopped, green = normal, red = fault, each fault period
    labelled with its full compound slug (speed_1_imbalance, ...)
  - score vs the ACTIVE gate (log): the condition's own band once
    mature, the global fit theta while no band applies; amber = grace
    (gate = -1, bands reforming), red span = alarm open, red dotted
    vline = ask, green marker + slug = the label the operator taught
  - positional evidence as the RATIO r_agg / band (log; edge = 1) - the
    static-offset evidence the one-step predictor is blind to; dotted
    lines + slugs mark which condition's band is the reference at every
    step, so a ratio jump is read as a reference change, not the machine
  - evidence accumulator vs ACC_ALARM / ACC_CLEAR (node_config.h); its
    distance to the trigger is the alarm-approach confidence

One shared legend per figure, in a reserved strip under the stage
title - no per-panel legends, nothing covers the data.

    python3 tests/bench/tools/plot_percond_tour.py runs/run-<ts>.jsonl \
            [--node all] [--out assets/percond-tour.png] \
            [--stages auto|none|0,150,302,end] [--pdf]
"""
import argparse
import math
import re
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.patches import Patch
from matplotlib.ticker import FuncFormatter, MultipleLocator

from plot_run import ALARM, BAND, load, name_spans, runs
from plot_compact import ABBREV, clean_spans

ACC_ALARM = 5.0    # cfg::ACC_ALARM, node_config.h
ACC_CLEAR = 1.0    # cfg::ACC_CLEAR, node_config.h

KIND_COL = {"normal": "#2ca02c",    # green: labelled normal
            "fault": "#d62728",     # red: labelled fault
            "stopped": "#1f77b4",   # blue: labelled stopped
            "unknown": "#ff9f1c",   # amber: alarm open, asking label
            "other": "#c9ccd1"}     # grey: learning / commissioning
TRUTH_STOPPED, TRUTH_NORMAL, TRUTH_FAULT = "#1f77b4", "#2ca02c", "#c0392b"
TRUTH_COL = {"stopped": TRUTH_STOPPED, "normal": TRUTH_NORMAL,
             "fault": TRUTH_FAULT, "other": "#c9ccd1"}
SCORE_C, GATE_C, THETA_C = "#4c9aff", "#d62728", "#888888"
RAGG_C, GRATE_C, ACC_C = "#2ca02c", "#9467bd", "#ff7f0e"
ASK_C, LABEL_C, GRACE_C = "#d62728", "#2ec4b6", "#f0c020"

# compact forms for the vertical label annotations (panel height is
# limited; the full slug appears in the strip row when it fits)
ANNOT_ABBREV = {"imbalance": "imbal", "phase_loss": "p-loss",
                "speed_1_imbalance": "s1 imbal",
                "speed_2_imbalance": "s2 imbal",
                "speed_1_phase_loss": "s1 p-loss",
                "speed_2_phase_loss": "s2 p-loss"}

STAGE_NOTES = {
    "commissioning + vocabulary":
        "Commissioning: CAL (ranges) then theta fit while the machine "
        "sits still, then ~440 settled stopped steps mature the baseline "
        "(stage-6 budget) - the settle sits behind the axis break. "
        "op_stopped auto-registers as the baseline. Each NEW machine "
        "state is unknown: surprise fills the accumulator to ACC_ALARM, "
        "the alarm opens, the node asks, the operator's label teaches a "
        "condition, the node relearns (amber = grace: gate -1 while its "
        "bands reform; the accumulator flushes 2x/step).",
    "first real faults":
        "A real fault injected mid-tour. The owner node (features that "
        "move hard) adds evidence every settled step and asks within "
        "~10-12 steps; the partner may stay under (score at its gate, "
        "r_agg inside its band) and never fires. A speed change under "
        "a continuing fault teaches a second condition.",
    "return tour (known states)":
        "Every state is known now: no asks. Short faults are answered "
        "from vocabulary in ~1 step (the node strip flips instantly); "
        "a node whose features barely move under the fault still falls "
        "back to its nearest condition.",
}


def kind_of(slug):
    if slug in ("unknown", "unidentified"):
        return "unknown"            # alarm open, asking label (amber)
    if slug == "op_stopped":
        return "stopped"            # blue
    if "imbalance" in slug or "phase_loss" in slug:
        return "fault"              # red
    if slug in ("learning", "idle"):
        return "other"              # grey
    return "normal"                 # green (speed_* ids + the quiet
                                    # unmatched 'normal' claim)


def truth_span(mname, ctx):
    """(color key, label) for one machine-mark span: faults compound with
    the running setpoint so each period reads its full slug
    (speed_1_imbalance, speed_2_phase_loss, ...) instead of one shared
    'imbalance' rectangle (user spec 2026-11-22)."""
    if mname in ("imbalance", "phase_loss"):
        if ctx and ctx.startswith("op_speed"):
            return "fault", f"{ctx[3:]}_{mname}"
        return "fault", mname
    if mname == "op_stopped":
        return "stopped", "stopped"
    if mname.startswith("op_speed"):
        return "normal", mname[3:]
    return "other", mname


def spans_where(rows, pred):
    out, start, prev = [], None, None
    for r in rows:
        if pred(r):
            if start is None:
                start = r["step"]
            prev = r["step"]
        elif start is not None:
            out.append((start, prev))
            start = None
    if start is not None:
        out.append((start, prev))
    return out


def series(rows, key, default=math.nan, mask_below=None):
    xs, ys = [], []
    for r in rows:
        v = r.get(key, default)
        if mask_below is not None and v < mask_below:
            v = math.nan
        xs.append(r["step"])
        ys.append(v)
    return xs, ys


def strip(ax, spans, colors_of, height=1.0):
    for name, a, b in spans:
        ax.axvspan(a, max(b, a + 1), 0, height,
                   color=colors_of(name), alpha=0.85, lw=0)
        if b - a > 40:
            col = colors_of(name)
            dark = sum(int(col[j:j + 2], 16) for j in (1, 3, 5)) < 480
            ax.text((a + b) / 2, height / 2, ABBREV.get(name, name),
                    fontsize=5.5, ha="center", va="center",
                    color="white" if dark else "#333333")


def detect_stages(marks, x0, x1):
    """Two-way tour acts: first fault injection, then the first revisit
    of a state learned before it. None if the pattern is absent."""
    fault_names = ("imbalance", "phase_loss")
    b1 = next((s for s, n, _c in marks if n in fault_names and x0 <= s < x1),
              None)
    if b1 is None:
        return None
    pre = [n for s, n, _c in marks if s < b1 and n != "op_stopped"]
    if not pre:
        return None
    first_state = pre[0]
    b2 = next((s for s, n, _c in marks if s > b1 and n == first_state), None)
    if b2 is None:
        b2 = x1
    stages = []
    if b1 > x0 + 10:
        stages.append((x0, b1, "commissioning + vocabulary"))
    if b2 > b1 + 10:
        stages.append((b1, b2, "first real faults"))
    if x1 > b2 + 10:
        stages.append((b2, x1, "return tour (known states)"))
    return stages or None


def dedupe(events_in, window=6):
    """Fleet events echo per node within a few steps; keep the first.
    Items are steps or (step, ...) tuples."""
    def step_of(e):
        return e[0] if isinstance(e, tuple) else e
    out = []
    for e in sorted(events_in, key=step_of):
        if not out or step_of(e) - step_of(out[-1]) > window:
            out.append(e)
    return out


def legend_handles():
    h = [
        Line2D([], [], color=SCORE_C, lw=1.4,
               label="score (surprise EMA)"),
        Line2D([], [], color=GATE_C, lw=1.4, label="active gate"),
        Line2D([], [], color=THETA_C, lw=1.1, ls="--",
               label="global fit theta (fallback gate)"),
        Line2D([], [], color=RAGG_C, lw=1.4,
               label="r_agg / band (positional evidence, log)"),
        Line2D([], [], color=GRATE_C, lw=1.4,
               label="band edge (r_agg = gate_r)"),
        Line2D([], [], color="0.30", lw=0.9, ls=":",
               label="band reference switch (posref)"),
        Line2D([], [], color=ACC_C, lw=1.4, label="accumulator acc"),
        Line2D([], [], color=GATE_C, lw=1.0, ls="--",
               label="ACC_ALARM = 5 (trigger)"),
        Line2D([], [], color="0.45", lw=1.0, ls=":",
               label="ACC_CLEAR = 1 (release)"),
        Patch(facecolor=BAND["cal"], alpha=0.25, label="CAL (ranges)"),
        Patch(facecolor=BAND["fit"], alpha=0.30, label="theta fit"),
        Patch(facecolor=ALARM, alpha=0.22, label="alarm open"),
        Patch(facecolor=GRACE_C, alpha=0.25,
              label="grace (gate = -1, bands reform)"),
        Patch(facecolor=TRUTH_STOPPED, alpha=0.85, label="truth: stopped"),
        Patch(facecolor=TRUTH_NORMAL, alpha=0.85, label="truth: normal"),
        Patch(facecolor=TRUTH_FAULT, alpha=0.85, label="truth: fault"),
        Patch(facecolor=KIND_COL["stopped"], alpha=0.85,
              label="node says: stopped"),
        Patch(facecolor=KIND_COL["normal"], alpha=0.85,
              label="node says: named normal"),
        Patch(facecolor=KIND_COL["fault"], alpha=0.85,
              label="node says: named fault"),
        Patch(facecolor=KIND_COL["unknown"], alpha=0.85,
              label="node says: alarm open (asking label)"),
        Line2D([], [], color=ASK_C, lw=1.0, ls=":", label="ask"),
        Line2D([], [], color=LABEL_C, marker="v", ms=5, lw=0,
               mec="none", label="label delivered"),
    ]
    return h


def draw_stage(out, k, n_stages, a, b, name, blocks, marks,
               node_types, ask_steps, labels, posref, pdf, trim=None):
    """One stage figure: shared truth strip + per-node block.
    labels: (step, slug) tuples, already deduped.
    trim: (ta, tb) - the step range [ta, tb) is compressed to an axis
    break (the commissioning settle: the figure shows the commissioning
    start, the break, then the settle tail + vocabulary)."""
    mark_spans = []
    for j, (s, mname, mctx) in enumerate(marks):
        end = marks[j + 1][0] if j + 1 < len(marks) else 10 ** 9
        lo, hi = max(s, a), min(end, b)
        if hi > lo:
            tcol, tlabel = truth_span(mname, mctx)
            # the bench re-marks the same state every few steps; merge by
            # the compound truth identity
            if mark_spans and mark_spans[-1][:2] == (tcol, tlabel):
                mark_spans[-1] = (tcol, tlabel, mark_spans[-1][2], hi)
            else:
                mark_spans.append((tcol, tlabel, lo, hi))

    # axis break: x coordinates remap so the trimmed region becomes a
    # narrow gap; the tick formatter restores the real step numbers.
    if trim:
        ta, tb = trim
        GAP = 10.0                       # visual gap width (mapped units)
        width = (b - a) - (tb - ta)

        def X(s):
            return s if s < tb else s - (tb - ta) + GAP

        def clip(lo, hi):
            # split a raw span around the trimmed region (only the truth
            # marks can straddle it - row-derived spans cannot)
            pieces = []
            if min(hi, ta) > lo:
                pieces.append((lo, min(hi, ta)))
            if hi > max(lo, tb):
                pieces.append((max(lo, tb), hi))
            return pieces
    else:
        ta = tb = GAP = 0
        width = b - a

        def X(s):
            return s

        def clip(lo, hi):
            return [(lo, hi)]

    heights = []
    for _, rows, has_ev in blocks:
        heights += [0.5, 1.7, 1.1, 1.1] if has_ev else [0.5, 1.7]
    fig, axes = plt.subplots(
        len(heights), 1, figsize=(12.5, 2.1 + 0.62 * len(heights)),
        dpi=150, sharex=True,
        gridspec_kw=dict(height_ratios=heights, hspace=0.30))
    axes = list(axes)
    fig.subplots_adjust(top=0.845, bottom=0.06, left=0.085, right=0.99)
    ai = 0

    for i, rows, has_ev in blocks:
        rows = [r for r in rows if a <= r["step"] < b
                and not (trim and ta <= r["step"] < tb)]
        ntype = node_types[i] if i < len(node_types) else f"node{i}"

        # truth vs node-says strip in one axes
        ax = axes[ai]; ai += 1
        node_spans = clean_spans(name_spans(rows, "cond"))
        for tcol, tlabel, lo, hi in mark_spans:
            for klo, khi in clip(lo, hi):
                ax.axvspan(X(klo), max(X(khi), X(klo) + 1), 0.0, 0.48,
                           color=TRUTH_COL[tcol], alpha=0.55)
                if khi - klo > 0.09 * width:
                    ax.text((X(klo) + X(khi)) / 2, 0.24, tlabel,
                            fontsize=5.5, ha="center", va="center",
                            color="white")
        for cname, lo, hi in node_spans:
            ax.axvspan(X(lo), max(X(hi), X(lo) + 1), 0.52, 1.0,
                       color=KIND_COL[kind_of(cname)], alpha=0.85)
            if hi - lo > 0.09 * width:
                dark_text = kind_of(cname) == "other"
                ax.text((X(lo) + X(hi)) / 2, 0.76,
                        ABBREV.get(cname, cname),
                        fontsize=5.5, ha="center", va="center",
                        color="#333333" if dark_text else "white")
        ax.set_ylim(0, 1.15)
        ax.set_yticks([0.24, 0.76])
        ax.set_yticklabels(["truth", "node says"], fontsize=7)
        ax.set_xticklabels([])

        # score vs the ACTIVE gate
        ax = axes[ai]; ai += 1
        for lo, hi in runs(rows, "phase", "cal"):
            ax.axvspan(X(lo), X(hi), color=BAND["cal"], alpha=0.25)
        for lo, hi in runs(rows, "phase", "fit"):
            ax.axvspan(X(lo), X(hi), color=BAND["fit"], alpha=0.30)
        for lo, hi in runs(rows, "phase", "alarm"):
            ax.axvspan(X(lo), X(hi), color=ALARM, alpha=0.20)
        for lo, hi in spans_where(rows, lambda r: r.get("gate",
                                                        math.nan) == -1):
            ax.axvspan(X(lo), X(hi), color=GRACE_C, alpha=0.28)
        for s in ask_steps:
            if a <= s < b and not (trim and ta <= s < tb):
                ax.axvline(X(s), color=ASK_C, ls=":", lw=0.9)
                ax.text(X(s) + 1, 0.97, "ask",
                        transform=ax.get_xaxis_transform(),
                        fontsize=5.5, rotation=90, ha="left", va="top",
                        rotation_mode="anchor", color=ASK_C)
        xs, sc = series(rows, "score", default=1e-3)
        ax.plot([X(x) for x in xs], [max(v, 1e-3) for v in sc],
                color=SCORE_C, lw=1.1)
        xs, gt = series(rows, "gate", mask_below=-0.5)
        ax.plot([X(x) for x in xs], gt, color=GATE_C, lw=1.2)
        ax.plot([X(x) for x in xs],
                [r.get("theta", math.nan)
                 if r.get("theta", 0) > 0 else math.nan for r in rows],
                color=THETA_C, ls="--", lw=0.9)
        for s, slug in labels:
            if a <= s < b and not (trim and ta <= s < tb):
                ax.plot([X(s)], [1.0], marker="v", ms=5, color=LABEL_C,
                        mec="none", clip_on=False,
                        transform=ax.get_xaxis_transform())
                ax.text(X(s) + 1, 0.97,
                        ANNOT_ABBREV.get(slug, slug),
                        transform=ax.get_xaxis_transform(),
                        fontsize=5.5, rotation=90, ha="left", va="top",
                        rotation_mode="anchor", color="#0e7c7b")
        ax.set_yscale("log")
        ax.set_ylim(ymin=1e-3)
        ax.set_ylabel(f"{ntype}\nscore vs gate (log)", fontsize=8)
        ax.grid(alpha=0.3)
        ax.set_xticklabels([])

        if not has_ev:
            continue

        # positional evidence as the RATIO the accumulator consumes:
        # r_agg / gate_r, log scale (2026-11-22f). Raw r_agg spans 4+
        # decades on real data (the fset REL_CLAMP spike alone is 5.7e4)
        # and flattens every quiet state on a linear axis; the ratio is
        # scale-free - a fault reads "45x its band", the state's own rows
        # sit at ~1x, and ratio > 1 is the exact breach predicate the
        # accumulator and the adaptation block consume. gate_r = -1
        # (grace / band immature) leaves a gap: no gate, no evidence.
        ax = axes[ai]; ai += 1
        for lo, hi in runs(rows, "phase", "alarm"):
            ax.axvspan(X(lo), X(hi), color=ALARM, alpha=0.14)
        xs, rg = series(rows, "ragg")
        xs, gr = series(rows, "gate_r", mask_below=-0.5)
        ratio = [r / g if (g and not math.isnan(g) and r > 0) else math.nan
                 for r, g in zip(rg, gr)]
        ax.plot([X(x) for x in xs], ratio, color=RAGG_C, lw=1.0)
        ax.axhline(1.0, color=GRATE_C, lw=1.2)
        # WHICH condition's band the ratio is divided by (posref from the
        # [RAW] telemetry): dotted vline + slug at every switch, so a
        # ratio jump is read as a reference change, never as the machine
        # moving. '-' or a missing step = no gate (grace / immature).
        pref = posref.get(i, {})
        segs = []
        for r in rows:
            slug = pref.get(r["step"])
            if not slug or slug == "-":
                continue
            if segs and segs[-1][0] == slug:
                segs[-1][2] = r["step"]
            else:
                segs.append([slug, r["step"], r["step"]])
        for j, (slug, lo, hi) in enumerate(segs):
            if j:
                ax.axvline(X(lo), color="0.30", ls=":", lw=0.8)
            if hi - lo > 0.05 * width:
                ax.text((X(lo) + X(hi)) / 2, 0.95, ABBREV.get(slug, slug),
                        transform=ax.get_xaxis_transform(), fontsize=5.5,
                        ha="center", va="top", color="0.30")
        ax.set_yscale("log")
        ax.set_ylabel("positional\nr_agg / band (x)", fontsize=8)
        ax.set_ylim(ymin=0.02)
        ax.grid(alpha=0.3)
        ax.set_xticklabels([])

        # evidence accumulator
        ax = axes[ai]; ai += 1
        for lo, hi in runs(rows, "phase", "alarm"):
            ax.axvspan(X(lo), X(hi), color=ALARM, alpha=0.14)
        xs, ac = series(rows, "acc")
        ax.plot([X(x) for x in xs], ac, color=ACC_C, lw=1.0)
        ax.axhline(ACC_ALARM, color=GATE_C, lw=0.9, ls="--")
        ax.axhline(ACC_CLEAR, color="0.45", lw=0.9, ls=":")
        ax.set_ylim(-0.5, 21)
        ax.set_ylabel("evidence\naccumulator", fontsize=8)
        ax.grid(alpha=0.3)

    axes[-1].xaxis.set_major_locator(
        MultipleLocator(25 if width > 180 else 20))
    if trim:
        def _fmt(x, _pos):
            if ta <= x < ta + GAP:
                return ""            # inside the axis break
            return f"{x:.0f}" if x < ta else f"{x + (tb - ta) - GAP:.0f}"
        axes[-1].xaxis.set_major_formatter(FuncFormatter(_fmt))
        axes[-1].set_xlim(X(a) - 2, X(b) + 2)
        for ax_ in axes:
            xb = (X(ta) + X(tb)) / 2       # the gap centre
            for dx in (-0.9, 0.9):
                for y0, y1 in ((0.02, 0.13), (0.87, 0.98)):
                    ax_.plot([xb + dx - 1.0, xb + dx + 1.0], [y0, y1],
                             color="0.55", lw=1.0,
                             transform=ax_.get_xaxis_transform(),
                             clip_on=False)
    axes[-1].set_xlabel("monitor step", fontsize=8)

    steps_txt = (f"steps {a}-{ta - 1} + {tb}-{b - 1} (settle behind the "
                 f"break)" if trim else f"steps {a}-{b - 1}")
    fig.suptitle(f"two-way tour, stage {k}/{n_stages}: "
                 f"{name}  -  {steps_txt}", fontsize=11, y=0.995)
    fig.legend(handles=legend_handles(), fontsize=5.8, ncol=9,
               loc="upper center", bbox_to_anchor=(0.5, 0.955),
               frameon=False, columnspacing=1.1, handletextpad=0.5)
    fig.text(0.01, 0.897, STAGE_NOTES.get(name, ""), fontsize=7.0,
             color="#444444", ha="left", va="top", wrap=True)

    fig.savefig(out)
    if pdf:
        fig.savefig(str(Path(out).with_suffix(".pdf")))
    plt.close(fig)
    print("wrote", out, "(+pdf)" if pdf else "")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("run", help="runs/run-<ts>.jsonl from the bench export")
    ap.add_argument("--node", default="all", help="node index or 'all'")
    ap.add_argument("--out", default="assets/percond-tour.png")
    ap.add_argument("--stages", default="auto",
                    help="'auto' (detect tour acts from marks), 'none' "
                         "(one figure), or comma-separated boundaries "
                         "like '0,150,302,end'")
    ap.add_argument("--stage1-trim", default=None,
                    help="stage-1 axis break 'A:B': the commissioning "
                         "settle [A,B) is compressed to a break mark, so "
                         "the figure shows the commissioning start, the "
                         "break, then the settle tail + vocabulary. The "
                         "paper figures use --stage1-trim 100:460.")
    ap.add_argument("--pdf", action="store_true")
    args = ap.parse_args()

    meta, srows, events, marks, pend, libcs = load(args.run)
    node_types = meta.get("node_types") or ["node"]
    n_nodes = len(node_types)
    sel = list(range(n_nodes)) if args.node == "all" else [int(args.node)]

    blocks = []
    for i in sel:
        rows = [r for r in srows if r["node"] == i]
        has_ev = bool(rows) and "gate" in rows[0]
        blocks.append((i, rows, has_ev))
    x0 = min(r["step"] for _, rows, _ in blocks for r in rows)
    x1 = max(r["step"] for _, rows, _ in blocks for r in rows)

    ask_steps = dedupe([s for (s, t) in events
                        if "open cond=unknown" in t and x0 <= s < x1])
    label_slug = {}
    for s, t in events:
        if "'label'" in t and x0 <= s < x1:
            m = re.search(r"label='([^']+)'", t)
            if m:
                label_slug[s] = m.group(1)
    labels = dedupe(sorted(label_slug.items()))

    # which condition's band the positional evidence was judged against,
    # per step per node: the [RAW] telemetry carries posref=<slug>
    # (2026-11-22f). EV texts carry no node id, so route by the node's
    # FIRST feature name (fset= -> the vfd, rms= -> the vib).
    feat_keys = {}
    for i, ntype in enumerate(node_types):
        names = (meta.get("feature_names") or {}).get(ntype) or []
        feat_keys[i] = names[0] if names else None
    posref = {i: {} for i in range(n_nodes)}
    for s, t in events:
        if "[RAW]" not in t or not (x0 <= s < x1):
            continue
        m = re.search(r" posref=(\S+)", t)
        if not m:
            continue
        for i, key in feat_keys.items():
            if key and f" {key}=" in t:
                posref[i][s] = m.group(1)
                break

    trim1 = None
    if args.stage1_trim:
        trim1 = tuple(int(v) for v in args.stage1_trim.split(":"))

    out_base = str(Path(args.out).with_suffix(""))

    stages = None
    if args.stages == "auto":
        stages = detect_stages(marks, x0, x1)
    elif args.stages != "none":
        parts = [p.strip() for p in args.stages.split(",")]
        bounds = [x0 if p in ("start", "") else
                  x1 if p == "end" else int(p) for p in parts]
        stages = [(bounds[j], bounds[j + 1], f"custom {j + 1}")
                  for j in range(len(bounds) - 1)
                  if bounds[j + 1] > bounds[j]]

    if not stages:
        stages = [(x0, x1, "full tour")]

    for k, (a, b, name) in enumerate(stages, 1):
        out = args.out if len(stages) == 1 else f"{out_base}-stage{k}.png"
        trim = trim1 if (k == 1 and trim1) else None
        draw_stage(out, k, len(stages), a, b, name, blocks,
                   marks, node_types, ask_steps, labels, posref, args.pdf,
                   trim=trim)


if __name__ == "__main__":
    main()
