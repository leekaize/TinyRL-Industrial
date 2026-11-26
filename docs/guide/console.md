# Serial console and replay

The serial console is the operator's direct line to a node: commission it, read its state, answer its asks, wipe it when needed. Every command below is exercised by suite stage 3 on the linked binary.

## Console keys

| Key | Action |
|---|---|
| `s` | [start] start learning |
| `r` | [read] poll sensor now |
| `l` | [library] print condition library |
| `p` | [pending] print stored unknown batches (label/discard by id) |
| `f <label> [normal\|fault]` | [feedback] label the OPEN episode; the second word is the KIND, not the action |
| `f <id> <label> [normal\|fault]` | [feedback] label a STORED batch by id: `f 1 belt_slip fault` labels batch P0001. The first token is read as a batch id when it is all digits or P+digits (console.cpp:36) |
| `f normal` / `f fault` | [feedback] false alarm (theta +10%) / confirmed fault (theta -5%) |
| `m` | [monitor] status (step/score/theta/params/lib/pubs/qum/heap) |
| `v` | [verbose] toggle per-transaction bus trace |
| `d` | [decision] toggle the per-step decision trace |
| `R` | [raw] toggle raw x,y,z streaming (vib node; 200 Hz, one line per sample, feeds `tools/feature_selection.py --capture`) |
| `?` | [usage] print the command list (also printed on boot) |
| `reset` | [wipe] factory reset stored state, typed in full |

Two grammar notes that have bitten people:

- `f normal` and `f fault` are reserved feedback words (theta governance). `f speed_1_imbalance fault` labels a condition; the second word is the kind. The old parser read the second word as the action, which made a console path to a normal-kind condition impossible (fixed 2026-11-20; RESEARCH.md design decisions).
- Batch-id mode (2026-11-21): `f <id> <label> [kind]` answers a STORED batch, so you can label long after the episode closed - the pending store survives reboots (`p` lists the ids). A slug never starts with a digit, so the first token being digits or P+digits is unambiguous (console.cpp:36-38).
- `reset` must be typed in full; there is no single-key wipe.

## Capture a hardware session, replay it offline

The node prints a machine line every 2 s (`[HDR]` feature identity once on boot, then `[RAW] ts_ms=<ms> <features...> state= cond= conf= score= theta=`). Capture it, analyse it, rerun it through the real core without hardware:

```bash
python3 tools/seriallog.py                    # capture: live view + raw .log + pandas .csv
python3 tools/rig_tour.py                     # scripted rig tour: prompts you at each state
                                              # change, labels every ask by itself, records
                                              # every [WHY] decision line; --trace adds the
                                              # per-step decision dump
bash tests/replay.sh data/serial/<file>.log   # rerun the session in the host core
```

The replay re-learns from an empty model at the captured start moment, applies the captured labels, and compares every decision against the captured ground truth. Change a `cfg::` knob, rerun, and watch the match rate move. Captures land in `data/serial/` (gitignored; force-add a benchmark run if you want it in history).

The per-decision explainability lines (`[WHY]` names the evidence chain, `[RAW]` carries score/gate/rag/gate_r/acc/posref) are documented under [enhancements C4](/system/enhancements).
