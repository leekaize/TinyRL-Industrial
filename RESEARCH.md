# Research Notes — TinyRL-Industrial

Tracking conventions:

- Update the **Status** column and tick checklists as evidence lands.
  - Legend: ⬜ open · 🟡 in progress · ✅ answered.
- Every claim in a *Current answer* must link to a row in the Experiment log (or a repo artifact). No evidence, no ✅.
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
- [ ] Ablate the parameter split below: node-local-only vs schema-exposed θ
      governance (doubles as the plug-and-play parameter test)

**Proposed parameter split (from literature; boundary to ablate):**

| Parameter | Evidence | Proposed boundary |
|-----------|----------|-------------------|
| Input normalisation (running moments) | Stream-X ablations | node-local |
| Representation stabilisation (LayerNorm / frozen features) | Stream-X ablations | node-local |
| Bounded step size (ObGD + overshooting bound) | Stream-X; Intentional Updates | node-local |
| Per-feature step sizes (IDBD) | Kearney; Degris | node-local; optional ablation — mixed evidence vs Adam heuristics |
| Detection threshold θ | ReRe; `normal`/`fault` ±% | **schema-exposed** — site-dependent value |
| Labels / condition library | Settles (active learning); GLAD | **schema-exposed** |
| Phase control (`start`, CAL/ARM/MON) | pairing protocol | **schema-exposed** |
| Reward scale / centering | De Asis | node-local, if reward enters the loop |

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

## Literature anchors

> Verified via arXiv API / Crossref on 2026-11-02. The schema (`State` /
> `Action` / metric set) is an *output* of this review, not an input: each row
> states what the work informs about what the schema must expose. Paywalled
> items flagged; all others open (arXiv / standards). Per-paper snapshots live
> in [`literature/`](literature/README.md) (one file per paper, from the
> template); this section stays the analytical map.

### Adaptable RL parameters (core of the semantic-context link)

> No paper yet catalogues adaptable parameters for industrial condition
> monitoring. Strong-CPU evidence exists for streaming RL components
> (Stream-X ablations: Atari + 50 continuous-control tasks, non-stationarity
> regimes); practice: adopt the best-evidenced standard parameters and ablate
> them on our multi-node system.

| Work | Source | Informs |
|------|--------|---------|
| Elsayed et al., Streaming Deep Reinforcement Learning Finally Works (Stream-X) | arXiv:2410.14606 | components that must adapt for streaming learning: input normalisation (running moments), representation stabilisation, bounded ObGD updates; validated at workstation scale, ESP32-S3 demo |
| Ren et al., TinyOL: TinyML with Online-Learning on Microcontrollers | arXiv:2103.08295 | on-node training on MCUs is feasible (supervised online-learning predecessor); baseline for what must adapt vs stay static |
| Kearney et al., Learning Feature Relevance Through Step Size Adaptation in Temporal-Difference Learning (IDBD lineage) | arXiv:1903.03252 | per-feature step sizes as the adaptable parameter; the adapted step size *is* feature relevance — a semantic quantity |
| Degris et al., Step-size Optimization for Continual Learning | arXiv:2401.17401 | IDBD-style meta-gradient vs Adam/RMSProp heuristics — which adaptivity mechanism to expose |
| Sharifnassab et al., Intentional Updates for Streaming Reinforcement Learning | arXiv:2604.19033 | step size specified in output units — how the adaptivity knob is parameterised |
| Lee et al., ReRe: A Lightweight Real-time Ready-to-Go Anomaly Detection Approach for Time Series | arXiv:2004.02319 (COMPSAC 2020) | self-adaptive thresholds in lightweight streaming TS anomaly detection — the θ analogue |
| De Asis et al., Extending Differential Temporal Difference Methods for Episodic Problems | arXiv:2605.04368 | reward centering — reward-scale adaptivity |

### What flows up (semantics) vs down (context) — to be settled by review

| Work | Source | Informs |
|------|--------|---------|
| Mueller, Open Challenges in Time Series Anomaly Detection: An Industry Perspective | arXiv:2502.05392 | industry call for streaming + human-in-the-loop in industrial TS anomaly detection — the gap statement |
| Zhou et al., Goal-Oriented Semantic Communications for 6G Networks | arXiv:2210.09372 | task-relevant information vs raw data — uplink principle |
| Farshbafan & Saad, Curriculum Learning for Goal-Oriented Semantic Communications with a Common Language | arXiv:2204.10429 | shared vocabulary between the two ends = the contract |
| Amershi et al., Power to the People: The Role of Humans in Interactive Machine Learning | AI Magazine 2014 (paywalled) | operator feedback inside the learning loop — downlink principle |
| Islam et al., GLAD: GLocalized Anomaly Detection via Human-in-the-Loop Learning | arXiv:1810.01403 | label feedback retunes deployed detector parameters |
| Deng et al., HILAD: A Reliable Framework for Human-in-the-Loop Anomaly Detection in Time Series | arXiv:2405.03234 | bidirectional human/model correction for TS anomaly detection |
| Settles, Active Learning | Springer Synthesis Lectures 2012, DOI 10.1007/978-3-031-01560-1 | query-on-uncertainty canon — labels arrive only on open asks |
| IEC 63278 (Asset Administration Shell); IEC 62541 (OPC UA); OASIS MQTT 5.0 | standards | precedent for versioned machine-readable industrial contracts; MQTT = transport of record |

### Consumer + positioning

| Work | Source | Informs |
|------|--------|---------|
| Xia et al., Towards autonomous system: flexible modular production system enhanced with LLM agents | arXiv:2304.14721 | LLM agents operating industrial systems via structured interfaces |
| Romero & Suyama, Agentic AI for Intent-Based Industrial Automation | arXiv:2506.04980 | agentic AI aligned with Industry 5.0 human-centricity |
| Carvalho et al., A systematic literature review of machine learning methods applied to predictive maintenance | Computers & Industrial Engineering 2019, DOI 10.1016/j.cie.2019.106024 (paywalled) | batch-supervised PdM across sensor modalities — sensor-agnostic framing (vibration, sound, current) |
| Andriotis & Papakonstantinou, Deep reinforcement learning driven inspection and maintenance planning | Reliability Engineering & System Safety 2021, DOI 10.1016/j.ress.2021.107551 (paywalled) | contrast: RL for maintenance scheduling, not condition reporting |

## Experiment log

| Date | Setup | RQ | Result | Evidence |
|------|-------|:--:|--------|----------|
| | | | | |

## Design decisions & rationale

> One entry per "why did we build it this way". Append-only.

| Decision | Rationale | Alternatives rejected |
|----------|-----------|----------------------|
| ObGD, not MVP histograms | histograms only saw consecutive bin pairs; net learns joint one-step dynamics; overshooting bound keeps updates sane during bursts | MVP histograms (`README.md` § Architecture cheat sheet) |
| Schema exposes only site-dependent knobs (θ governance, labels, phase); streaming self-adaptation stays node-local | Stream-X holds one fixed hyperparameter config across tasks — internal adaptation thins what the contract carries; site-relative values (thresholds, labels) are what only external context can supply | expose all RL parameters through the schema; fully static node (no two-way link) |

## Deliverables

- [ ] Report: RQ2 measured ratio table
- [ ] Report: RQ3 scaling curves
