# Research Notes — TinyRL-Industrial

Tracking conventions:

- Update the **Status** column and tick checklists as evidence lands.
  Legend: ⬜ open · 🟡 in progress · ✅ answered.
- Every claim in a *Current answer* must link to a row in the Experiment log
  (or a repo artifact). No evidence, no ✅.
- Experiment log is append-only — new runs get new rows, old rows are never edited.

## Research Questions

| # | Question | Status | Answer summary |
|---|----------|:------:|----------------|
| [RQ1](#rq1-schema-parameters-for-two-way-semantic-context-flow) | Effective schema parameters for two-way node↔agent semantic-context flow | 🟡 | Two-way schema runs: telemetry up (`State/*`), context down (`Action/*` + `R`) |
| [RQ2](#rq2-resource-and-bandwidth-savings-vs-node-only-and-agent-only) | Resource & bandwidth saved on edge vs node-only and agent-only baselines | 🟡 | 320 KB-RAM node at 70% free, zero cloud round-trips; ~400× fewer uplink messages |
| [RQ3](#rq3-accuracy-scaling-with-more-self-contained-devices) | Accuracy improvement as self-contained devices join and communicate | 🟡 | 21.7 s detection, zero false alarms (single node); multi-node scaling unmeasured |

### RQ1: Schema parameters for two-way semantic-context flow

> What are the effective parameters within the schema for a two-way
> semantic-context flow between node and agent — ensuring plug-and-play
> operation, low technical requirements, and self-contained devices?

**Current answer (single node):** minimal two-way schema closes the loop —
telemetry up via `State/*` (`[CAL]/[ARM]/[MON]/[ALARM]`), context down via
`Action/*` feedback (`R`). Effective parameters appear to be: message set,
publish rates, phase control.

**Evidence:** `uns/uns_schema_v1.json`, `uns/uns_namespace.json`,
`nodes/core/uns_messaging.h/.cpp`, `agent/skills.md`

**Remaining work:**
- [ ] Enumerate + justify the full parameter set (message set, rates, phase control)
- [ ] Ablate parameters: which are required for plug-and-play vs optional?
- [ ] Verify a new sensor joins with node-side changes only (`nodes/sensors/`)
- [ ] Document the minimum technical requirement for a working pairing

### RQ2: Resource and bandwidth savings vs node-only and agent-only

> How much resources (and network bandwidth) do we save on the edge and
> agents, compared to node-only and agent-only architectures?

**Current answer (preliminary):** all learning/inference on a 320 KB-RAM chip
with ~70% free, zero cloud round-trips at run time; uplink ~1 line/50 s vs
20 raw samples/s → **400× fewer messages** (confirm from measured payloads).

**Evidence:** `print_memory_info()` runtime output; cost audit in
[README § Architecture cheat sheet](README.md#architecture-cheat-sheet)

**Remaining work:**
- [ ] Define + run the two baselines: node-only (no agent) and agent-only (raw telemetry upstream)
- [ ] Measure bytes/min up+down per architecture from MQTT payload logs
- [ ] Table: RAM / flash / CPU / bandwidth / messages per architecture
- [ ] Confirm the 400× message ratio with measured numbers

### RQ3: Accuracy scaling with more self-contained devices

> How does accuracy improve as more self-contained devices are added and
> communicate with each other?

**Current answer (N=1):** detection in 21.7 s with zero false alarms on
normal and control data; z-baseline comparison logged per run — the
head-to-head for "does learned prediction beat classical statistics?"

**Evidence:** `tests/host-test/` (900-step simulation + full link check);
per-run z-baseline comparison logs

**Remaining work:**
- [ ] Formalize the metric (detection latency, false-alarm rate, per-condition accuracy)
- [ ] Run N-node experiments (N = 1, 2, …) with shared condition libraries
- [ ] Plot accuracy/latency vs N; quantify marginal gain per added device
- [ ] Compare against single-node accuracy to isolate the communication benefit

## Experiment log

| Date | Setup | RQ | Result | Evidence |
|------|-------|:--:|--------|----------|
| | | | | |

## Design decisions & rationale

> One entry per "why did we build it this way". Append-only.

| Decision | Rationale | Alternatives rejected |
|----------|-----------|----------------------|
| ObGD, not MVP histograms | histograms only saw consecutive bin pairs; net learns joint one-step dynamics; overshooting bound keeps updates sane during bursts | MVP histograms (`README.md` § Architecture cheat sheet) |

## Deliverables

- [ ] Report: RQ2 measured ratio table
- [ ] Report: RQ3 scaling curves
