#!/usr/bin/env python3
"""Turn host-bench logs into the RESEARCH.md figures (matplotlib).

Usage:
    python3 tests/host-test/plot_bench.py [TRACE_LOG] [BENCH_LOG] [--outdir DIR]

Defaults: /tmp/trace.log, /tmp/bench.log, --outdir assets. Generate the logs
(after one `bash tests/host-test.sh` so the binaries exist):

    TINYRL_TRACE=1 /tmp/tinyrl_test_host > /tmp/trace.log
    TINYRL_LONGRUN=100000 /tmp/tinyrl_test_host > /tmp/bench.log

Figures (regenerate after any algorithm change, and before submitting):
    detection-trace.png    score vs theta across quiet + anomaly, alarm marked
    plasticity-bench.png   dead units, mean |w|, effective rank vs steps
"""
import re
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUT = ROOT / "assets"

BLUE, RED, ORANGE, GREEN, MUTED, INK = ("#1f77b4", "#d62728", "#ff7f0e",
                                        "#2ca02c", "#666666", "#333333")

TRACE_RE = re.compile(
    r"^TRACE step=(\d+) tag=(\w+) score=([-+0-9.eE]+) theta=([-+0-9.eE]+)( event=alarm)?")
PLAST_RE = re.compile(
    r"^\[PLAST\] step=(\d+)\s+dead=([\d.]+)/([\d.]+) mean\|w\|=([\d.eE+-]+) eff_rank=([\d.]+)")
REGIME_RE = re.compile(r"^F: regime shift at step (\d+)")


def parse_trace(path):
    rows = []
    for line in Path(path).read_text().splitlines():
        m = TRACE_RE.match(line.strip())
        if m:
            rows.append({"step": int(m.group(1)), "tag": m.group(2),
                         "score": float(m.group(3)), "theta": float(m.group(4)),
                         "alarm": m.group(5) is not None})
    return rows


def parse_bench(path):
    plast, regimes = [], []
    for line in Path(path).read_text().splitlines():
        m = PLAST_RE.match(line.strip())
        if m:
            plast.append({"step": int(m.group(1)), "dead1": float(m.group(2)),
                          "dead2": float(m.group(3)), "maw": float(m.group(4)),
                          "eff": float(m.group(5))})
        elif (m := REGIME_RE.match(line.strip())):
            regimes.append(int(m.group(1)))
    return plast, regimes


def fig_detection(rows, outdir):
    quiet = [r for r in rows if r["tag"] == "quiet"]
    anom = [r for r in rows if r["tag"] == "anomaly"]
    if not quiet or not anom:
        print("detection figure skipped: trace log lacks quiet/anomaly rows")
        return
    onset = anom[0]["step"]
    alarm = next((r for r in rows if r["alarm"]), None)
    latency = alarm["step"] - onset if alarm else None
    theta = rows[0]["theta"]

    fig, ax = plt.subplots(figsize=(8.2, 3.4), dpi=150)
    ax.axvspan(quiet[0]["step"], onset, color=GREEN, alpha=0.07, label="quiet")
    ax.axvspan(onset, anom[-1]["step"], color=RED, alpha=0.07, label="anomaly")
    ax.plot([r["step"] for r in quiet], [r["score"] for r in quiet], color=BLUE,
            label="score (EMA of surprise)")
    ax.plot([r["step"] for r in anom], [r["score"] for r in anom], color=BLUE)
    ax.axhline(theta, color=RED, ls="--", lw=1.2, label="theta = mu + 4 sigma")
    if alarm:
        ax.axvline(alarm["step"], color=RED, ls=":", lw=1.4, label="alarm")
        ax.annotate("alarm: +%d steps after onset" % latency,
                    xy=(alarm["step"], alarm["score"]),
                    xytext=(alarm["step"] - 85, alarm["score"] * 0.30),
                    arrowprops=dict(arrowstyle="->", color=INK, lw=0.8),
                    fontsize=9, color=INK)
    ax.set_yscale("log")
    ax.set_xlabel("monitor step")
    ax.set_ylabel("score (z-space EMA)")
    ax.set_title("One-step prediction detector: 200 quiet steps, then injected anomaly")
    ax.grid(alpha=0.3)
    ax.legend(loc="lower right", fontsize=8)
    fig.tight_layout()
    fig.savefig(outdir / "detection-trace.png")
    plt.close(fig)


def fig_plasticity(pts, regimes, outdir):
    if not pts:
        print("plasticity figure skipped: bench log has no [PLAST] rows")
        return
    steps = [p["step"] for p in pts]
    panels = [
        ("dead-unit fraction",
         [("hidden layer 1", [p["dead1"] for p in pts], BLUE),
          ("hidden layer 2", [p["dead2"] for p in pts], ORANGE)], None),
        ("mean |w|",
         [("mean |w|", [p["maw"] for p in pts], BLUE)], None),
        ("effective rank",
         [("effective rank", [p["eff"] for p in pts], BLUE)], None),
    ]
    fig, axes = plt.subplots(3, 1, sharex=True, figsize=(8.2, 6.8), dpi=150)
    for ax, (ylabel, lines, _) in zip(axes, panels):
        for label, ys, color in lines:
            ax.plot(steps, ys, color=color, label=label)
        for s in regimes:
            ax.axvline(s, color=MUTED, ls="--", lw=0.8)
        ax.set_ylabel(ylabel)
        ax.grid(alpha=0.3)
        ax.legend(fontsize=8, loc="upper left")
    axes[0].set_ylim(0, max(0.02, max(max(ys) for _, ys, _ in panels[0][1]) * 1.2))
    axes[0].set_title("Plasticity audit: 1,348-param Stream-TD net, regime shifts "
                      "marked (Dohare 2024 correlates)", fontsize=11)
    first, last = pts[0], pts[-1]
    axes[1].annotate("mean |w| %.4f -> %.4f (+%.0f%%)" % (
        first["maw"], last["maw"], 100 * (last["maw"] / first["maw"] - 1)),
        xy=(0.30, 0.82), xycoords="axes fraction", fontsize=9)
    axes[2].annotate("effective rank %.2f -> %.2f" % (first["eff"], last["eff"]),
                     xy=(0.30, 0.82), xycoords="axes fraction", fontsize=9)
    axes[2].set_xlabel("monitor step")
    fig.tight_layout()
    fig.savefig(outdir / "plasticity-bench.png")
    plt.close(fig)


def main(argv):
    args = list(argv[1:])
    outdir = DEFAULT_OUT
    if "--outdir" in args:
        i = args.index("--outdir")
        outdir = Path(args[i + 1])
        del args[i:i + 2]
    trace_log = Path(args[0]) if args else Path("/tmp/trace.log")
    bench_log = Path(args[1]) if len(args) > 1 else Path("/tmp/bench.log")
    outdir.mkdir(parents=True, exist_ok=True)
    if trace_log.exists():
        fig_detection(parse_trace(trace_log), outdir)
    else:
        print("trace log not found: %s" % trace_log)
    if bench_log.exists():
        plast, regimes = parse_bench(bench_log)
        fig_plasticity(plast, regimes, outdir)
        if plast:
            first, last = plast[0], plast[-1]
            print("mean |w| %.4f -> %.4f | effective rank %.2f -> %.2f | "
                  "last step %d" % (first["maw"], last["maw"],
                                    first["eff"], last["eff"], last["step"]))
    else:
        print("bench log not found: %s" % bench_log)


if __name__ == "__main__":
    main(sys.argv)
