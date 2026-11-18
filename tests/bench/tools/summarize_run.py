#!/usr/bin/env python3
"""Paper metrics from an exported web-bench run.

    python3 tests/host-test/webbench/summarize_run.py runs/run-<ts>.jsonl

Reads the export (meta, S rows, EV events, MARK machine truth) and prints
a markdown metrics block per run, plus a per-fault CSV next to the run
file (runs/run-<ts>-summary.csv).

Metrics:
- identification latency per fault: fault MARK step -> first NEW alarm
  or local fault match (state transition into alarm/match) on a node
  within the fault period. A state already active at inject time is a
  run-on episode from an earlier fault, not an identification.
  Reported for the fault's OWNING sensing node and the partner node separately: a fleet fault is owned
  by one node type (imbalance -> vib_adxl, phase_loss -> vfd_g100);
  a partner alarm in the same window is cross-modal support, its miss is expected.
- false alarm: alarm span starting while machine truth is not a fault
- asks opened: EV "open cond=unknown" count. In node-only mode these
  never leave the node, so upstream asks = 0 there
- labels: [FB] label events received

Machine truth = MARK rows (step, machine name) written by the bench
export. Fault names: imbalance / phase_loss.
"""
import argparse
import csv
import json
from pathlib import Path

FAULTS = ("imbalance", "phase_loss")
OWNER = {"imbalance": "vib_adxl", "phase_loss": "vfd_g100"}


def spans(rows, key, want):
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


def load(path):
    meta, srows, events, marks = {}, [], [], []
    with open(path) as f:
        for line in f:
            d = json.loads(line)
            if d["type"] == "meta":
                meta = d
            elif d["type"] == "S":
                srows.append(d)
            elif d["type"] == "EV":
                events.append((d["step"], d["text"]))
            elif d["type"] == "MARK":
                marks.append((d["step"], d["name"]))
    return meta, srows, events, sorted(marks)


def truth_at(marks, step, default="op_stopped"):
    name = default
    for s, n in marks:
        if s <= step:
            name = n
        else:
            break
    return name


def identify(rows, fault, start, end):
    """First alarm or fault-match state that STARTS in [start, end).

    A state already active when the fault is injected (run-on episode
    from an earlier fault) is not an identification of this fault.
    Fault conditions are compound slugs (speed_1_imbalance): a quiet
    match names the fault too, so the suffix counts as identification.
    """
    prev = False
    for r in rows:
        cur = r["phase"] == "alarm" or r["cond"] == fault \
            or r["cond"].endswith("_" + fault)
        if cur and not prev and start <= r["step"] < end:
            return r["step"]
        prev = cur
    return None


def summarize(path) -> tuple:
    """Returns (markdown_block, metrics_dict). The block is the per-run
    report; the metrics dict feeds the cross-run contrast table
    (compare()) - the two-way vs node-only headline for the paper."""
    meta, srows, events, marks = load(path)
    types = meta.get("node_types") or []
    n_nodes = len(types) or 1
    types = types or ["node"] * n_nodes
    mode = meta.get("mode", "two-way")
    asks = sum(1 for _, t in events if "open cond=unknown" in t)
    upstream = 0 if mode == "node-only" else asks
    labels = sum(1 for _, t in events if "[FB]" in t and "'label'" in t)

    fault_events = []
    for idx, (s, n) in enumerate(marks):
        if n in FAULTS:
            end = marks[idx + 1][0] if idx + 1 < len(marks) else 10 ** 9
            fault_events.append((s, end, n))

    rows_by_node = {i: [r for r in srows if r["node"] == i]
                    for i in range(n_nodes)}
    alarms_by_node = {i: spans(rows_by_node[i], "phase", "alarm")
                      for i in range(n_nodes)}
    false_by_node = {}
    for i in range(n_nodes):
        false_by_node[i] = sum(1 for a, b in alarms_by_node[i]
                               if truth_at(marks, a) not in FAULTS)

    out = [f"### {Path(path).name}  mode={mode}  "
           f"faults injected={len(fault_events)}", "",
           f"asks opened: {asks} (upstream: {upstream}) "
           f"- labels received: {labels}",
           "false alarms: "
           + "  ".join(f"{types[i]}={false_by_node[i]}"
                       for i in range(n_nodes)), "",
           "| fault | inject step | owner | owner ident (steps) "
           "| partner | partner ident (steps) |",
           "|---|---|---|---|---|---|"]
    csv_rows = []
    for s, end, n in fault_events:
        cells = [n, str(s)]
        for role, tname in (("owner", OWNER.get(n)),
                            ("partner",
                             next((t for t in types if t != OWNER.get(n)),
                                  None))):
            i = types.index(tname) if tname in types else None
            if i is None:
                cells += ["-", "-"]
                continue
            hit = identify(rows_by_node[i], n, s, end)
            cells += [tname, str(hit - s) if hit is not None else "missed"]
            csv_rows.append([mode, n, s, i, tname, role,
                             "" if hit is None else hit,
                             "" if hit is None else hit - s,
                             "identified" if hit is not None else "missed"])
        out.append("| " + " | ".join(cells) + " |")
    out.append("")
    for i in range(n_nodes):
        conds = sorted({r["cond"] for r in rows_by_node[i]
                        if r["cond"] not in ("idle", "unknown", "")})
        if conds:
            out.append(f"- node {i} ({types[i]}) named conditions: "
                       f"{', '.join(conds)}")

    stem = Path(path).with_suffix("")
    csv_path = f"{stem}-summary.csv"
    with open(csv_path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["mode", "fault", "inject_step", "node", "node_type",
                    "role", "ident_step", "latency_steps", "outcome"])
        w.writerows(csv_rows)
    out.append("")
    out.append(f"per-fault table: {csv_path}")

    owner_lat = [r[7] for r in csv_rows if r[5] == "owner"
                 and r[8] == "identified"]
    metrics = {
        "mode": mode,
        "asks": asks,
        "upstream": upstream,
        "labels": labels,
        "false_alarms": {types[i]: false_by_node[i] for i in range(n_nodes)},
        "owner_hits": len(owner_lat),
        "owner_total": sum(1 for r in csv_rows if r[5] == "owner"),
        "owner_latencies": owner_lat,
        "vocabulary": {types[i]: len([c for c in
                       {r["cond"] for r in rows_by_node[i]
                        if r["cond"] not in ("idle", "unknown", "learning",
                                              "normal", "")}])
                       for i in range(n_nodes)},
    }
    return "\n".join(out), metrics


def compare(runs) -> str:
    """Cross-run contrast: the paper's headline table. Two-way shows the
    semantic loop's payoff (vocabulary growth, few upstream calls); the
    node-only baseline shows the same detections with zero learning."""
    parsed = [(Path(p).name, summarize(p)[1]) for p in runs]
    if not parsed:
        return ""
    headers = [f"{name} ({m['mode']})" for name, m in parsed]
    out = ["", "## Cross-run contrast", "",
           "| metric | " + " | ".join(headers) + " |",
           "|---|" + "---|" * len(parsed)]

    def row(label, fn):
        out.append(f"| {label} | "
                   + " | ".join(str(fn(m)) for _, m in parsed) + " |")

    row("upstream asks (semantic calls)",
        lambda m: f"{m['asks']} (upstream {m['upstream']})")
    row("labels consumed", lambda m: m["labels"])
    row("false alarms",
        lambda m: ", ".join(f"{k}={v}" for k, v in m["false_alarms"].items()))
    row("fault identification (owner)",
        lambda m: f"{m['owner_hits']}/{m['owner_total']} hits" +
                  (f", latency " + "/".join(str(v) for v in m["owner_latencies"])
                   + " steps" if m["owner_latencies"] else ""))
    row("condition vocabulary (final)",
        lambda m: ", ".join(f"{k}={v}" for k, v in m["vocabulary"].items()))
    return "\n".join(out)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("runs", nargs="+", help="runs/run-<ts>.jsonl exports")
    args = ap.parse_args()
    texts, metrics = [], []
    for run in args.runs:
        text, m = summarize(run)
        print(text)
        texts.append(text)
        metrics.append(m)
    if len(args.runs) > 1:
        print(compare(args.runs))


if __name__ == "__main__":
    main()
