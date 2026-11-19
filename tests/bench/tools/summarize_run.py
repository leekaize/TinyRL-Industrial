#!/usr/bin/env python3
"""Paper metrics from an exported web-bench run.

    python3 tests/bench/tools/summarize_run.py runs/run-<ts>.jsonl

Metric set (RESEARCH.md RQ3, formalized 2026-11-19; rationale in
literature/mueller-2025-ts-ad-industry.md, settles-2012 ch. 2,
islam-2018-glad): evaluated at the DEPLOYED operating point - theta as fit
on-node, every step scored with data < t only (streaming). Accuracy/F1 are
deliberately NOT reported: they are functions of theta, and sweeping theta
over a recorded series needs full-series ground truth, exactly the
batch-benchmark practice Mueller rejects. "Better" = detection latency or
ask count improves while false alarms stay flat.

- identification latency per fault PERIOD: fault mark -> the node SHOWS
  the fault (a fresh alarm edge, or its condition column naming it).
  Already named at inject = latency 0 (run-on knowledge of a repeated
  fault is correct, not a miss). Owner and partner reported separately:
  a fleet fault is owned by one node type (imbalance -> vib_adxl,
  phase_loss -> vfd_g100); a partner hit is cross-modal support, its miss
  is expected.
- false alarm: alarm span opening while machine truth is not a fault and
  the node asserted a NAMED condition - it alarmed a state it claims to
  know. An alarm opening unknown/unidentified is an ASK, not a false
  alarm; an ask for a state the node had already named is counted
  separately as a redundant ask.
- asks opened: EV "open cond=unknown" count + stored batch sizes (the ask
  matures only at >= ASK_MIN_BATCH settled samples, 2026-11-19).
- labels: [FB] label events received.

Machine truth = MARK rows (exact application step, machine name). Marks
with the same name in a row are ONE period: a setpoint change inside a
fault period is not a new injection.
"""
import argparse
import csv
import json
from pathlib import Path

FAULTS = ("imbalance", "phase_loss")
OWNER = {"imbalance": "vib_adxl", "phase_loss": "vfd_g100"}
NON_COND = ("", "idle", "learning", "normal", "unknown", "unidentified")
TRUTH_SLUG = {"op_stopped": "op_stopped",
              "op_speed_1": "speed_1", "op_speed_2": "speed_2"}


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


def merge_marks(marks):
    """Consecutive same-name marks -> one period boundary list."""
    out = []
    for s, n in marks:
        if out and out[-1][1] == n:
            continue
        out.append((s, n))
    return out


def truth_at(periods, step, default="op_stopped"):
    name = default
    for s, n in periods:
        if s <= step:
            name = n
        else:
            break
    return name


def fault_periods(periods):
    """[(start, end, name)] for every merged fault period."""
    out = []
    for i, (s, n) in enumerate(periods):
        if n in FAULTS:
            end = periods[i + 1][0] if i + 1 < len(periods) else 10 ** 9
            out.append((s, end, n))
    return out


def alarm_spans(rows):
    spans, start, prev = [], None, False
    for r in rows:
        cur = r["phase"] == "alarm"
        if cur and not prev:
            start = r["step"]
        if not cur and prev:
            spans.append((start, r["step"]))
        prev = cur
    if prev and rows:
        spans.append((start, rows[-1]["step"]))
    return spans


def cond_at(rows, step):
    for r in rows:
        if r["step"] == step:
            return r["cond"]
    return ""


def vocab_of(rows):
    """slug -> first step the node showed it as a named condition."""
    v = {}
    for r in rows:
        if r["cond"] not in NON_COND:
            v.setdefault(r["cond"], r["step"])
    return v


def identify(rows, fault, start, end):
    """First step in [start, end) the node shows the fault: a FRESH alarm
    edge (a run-on alarm from another state is not evidence) or its cond
    naming the fault (compound slug suffix; persists while quiet - named
    knowledge is exactly what the re-tour tests)."""
    prev_alarm = False
    for r in rows:
        if r["step"] >= end:
            break
        named = r["cond"] == fault or r["cond"].endswith("_" + fault)
        fresh_alarm = r["phase"] == "alarm" and not prev_alarm
        if (fresh_alarm or named) and r["step"] >= start:
            return r["step"]
        prev_alarm = r["phase"] == "alarm"
    return None


def summarize(path) -> tuple:
    """Returns (markdown_block, metrics_dict). The block is the per-run
    report; the metrics dict feeds the cross-run contrast table."""
    meta, srows, events, raw_marks = load(path)
    periods = merge_marks(raw_marks)
    types = meta.get("node_types") or []
    n_nodes = len(types) or 1
    types = types or ["node"] * n_nodes
    mode = meta.get("mode", "two-way")

    asks = sum(1 for _, t in events if "open cond=unknown" in t)
    batch_ns = []
    for _, t in events:
        if "[PEND] batch" in t and "stored (" in t:
            try:
                batch_ns.append(int(t.split("stored (")[1].split(" ")[0]))
            except (ValueError, IndexError):
                pass
    upstream = 0 if mode == "node-only" else asks
    labels = sum(1 for _, t in events if "[FB]" in t and "'label'" in t)

    rows_by_node = {i: [r for r in srows if r["node"] == i]
                    for i in range(n_nodes)}
    vocab = {i: vocab_of(rows_by_node[i]) for i in range(n_nodes)}
    spans_by_node = {i: alarm_spans(rows_by_node[i]) for i in range(n_nodes)}

    false_by_node, redundant_by_node = {}, {}
    for i in range(n_nodes):
        false_by_node[i] = 0
        redundant_by_node[i] = 0
        for a, _b in spans_by_node[i]:
            truth = truth_at(periods, a)
            if truth in FAULTS:
                continue                      # detection, ident metric owns it
            cond_open = cond_at(rows_by_node[i], a)
            if cond_open in ("unknown", "unidentified"):
                slug = TRUTH_SLUG.get(truth, truth)
                first = vocab[i].get(slug)
                if first is not None and first < a:
                    redundant_by_node[i] += 1   # re-ask of a known state
            else:
                false_by_node[i] += 1           # alarmed a state it names

    periods_f = fault_periods(periods)
    out = [f"### {Path(path).name}  mode={mode}  "
           f"fault periods={len(periods_f)}", "",
           f"asks opened: {asks} (upstream: {upstream}, redundant: "
           f"{sum(redundant_by_node.values())})"
           + (f" - batch sizes min={min(batch_ns)} median="
              f"{sorted(batch_ns)[len(batch_ns)//2]}" if batch_ns else "")
           + f" - labels received: {labels}",
           "false alarms (alarmed a named state during non-fault truth): "
           + "  ".join(f"{types[i]}={false_by_node[i]}"
                       for i in range(n_nodes)), "",
           "| fault | inject step | owner | owner ident (steps) "
           "| partner | partner ident (steps) |",
           "|---|---|---|---|---|---|"]
    csv_rows = []
    for s, end, n in periods_f:
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
        conds = sorted(vocab[i])
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
        "redundant_asks": sum(redundant_by_node.values()),
        "min_batch": min(batch_ns) if batch_ns else None,
        "labels": labels,
        "false_alarms": {types[i]: false_by_node[i] for i in range(n_nodes)},
        "owner_hits": len(owner_lat),
        "owner_total": sum(1 for r in csv_rows if r[5] == "owner"),
        "owner_latencies": owner_lat,
        "vocabulary": {types[i]: len(vocab[i]) for i in range(n_nodes)},
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
        lambda m: f"{m['asks']} (redundant {m['redundant_asks']})")
    row("min ask batch (settled samples)", lambda m: m["min_batch"])
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
