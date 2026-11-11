# Research Notes — TinyRL-Industrial

Conventions:

- Update **Status** and tick checklists as evidence lands. ⬜ open · 🟡 in progress · ✅ answered
- Every claim in a *Current answer* links to an Experiment-log row or repo artifact. No evidence, no ✅.
- Experiment log is append-only — new runs get new rows.

## Research Keywords

Streaming reinforcement learning; TinyML; Predictive maintenance;
Time-series anomaly detection; Semantic communication; Human-in-the-loop
machine learning; LLM agents; Industry 5.0.

> Submission keywords, ordered by specificity (Industry 5.0 drops first if capped). TinyRL and "semantic-context link" live in title/abstract. Repo-discoverability terms (MQTT, UNS, PlatformIO) stay in `README.md`.

## Research Questions

| # | Question | Status | Answer summary |
|---|----------|:------:|----------------|
| [RQ1](#rq1-schema-parameters-for-two-way-semantic-context-flow) | Effective schema parameters for two-way node↔agent semantic-context flow | 🟡 | Two-way schema runs: telemetry up (`State/*`), context down (`Action/*` + `R`) |
| [RQ2](#rq2-resource-and-bandwidth-savings-vs-node-only-and-agent-only) | Resource & bandwidth saved on edge vs node-only and agent-only baselines | 🟡 | 320 KB-RAM node at 70% free, zero cloud round-trips; ~400× fewer uplink messages |
| [RQ3](#rq3-accuracy-scaling-with-more-self-contained-devices) | Accuracy improvement as self-contained devices join and communicate | 🟡 | 21.7 s detection, zero false alarms (single node); multi-node unmeasured |

### RQ1: Schema parameters for two-way semantic-context flow

> What are the effective parameters within the schema for a two-way
> semantic-context flow between node and agent — ensuring plug-and-play
> operation, low technical requirements, and self-contained devices?

**Current answer (single node):** minimal two-way schema closes the loop: telemetry up via `State/*` (`[CAL]/[ARM]/[MON]/[ALARM]`), context down via `Action/*` feedback (`R`). Effective parameters: message set, publish rates, phase control.

**Evidence:** `uns/uns_schema_v1.json`, `uns/uns_namespace.json`, `nodes/core/uns_messaging.h/.cpp`, `agent/skills.md`

**Remaining work:**

- [ ] Enumerate + justify the parameter set; ablate required vs optional
- [ ] Verify a new sensor joins with node-side changes only (`nodes/sensors/`)
- [ ] Document the minimum technical requirement for a working pairing
- [ ] Ablate node-local-only vs schema-exposed θ governance (doubles as the plug-and-play test)

**Proposed parameter split (from literature; boundary to ablate):**

| Parameter | Evidence | Proposed boundary |
|-----------|----------|-------------------|
| Input normalisation (running moments) | Stream-X ablations | node-local |
| Representation stabilisation (LayerNorm / frozen features) | Stream-X ablations | node-local |
| Bounded step size (StreamingOptimizer; v3 replaced ObGD) | Stream-X v3 App. A; Intentional Updates | node-local |
| Per-feature step sizes (IDBD) | Kearney; Degris | node-local; optional ablation — mixed evidence vs Adam heuristics |
| Detection threshold θ | ReRe; `normal`/`fault` ±% | **schema-exposed** — site-dependent value |
| Labels / condition library | Settles (active learning); GLAD | **schema-exposed** |
| Phase control (`start`, CAL/ARM/MON) | pairing protocol | **schema-exposed** |
| Reward scale / centering | De Asis | node-local, if reward enters the loop |

### RQ2: Resource and bandwidth savings vs node-only and agent-only

> How much resources (and network bandwidth) do we save on the edge and
> agents, compared to node-only and agent-only architectures?

**Current answer (preliminary):** all learning/inference on a 320 KB-RAM chip with ~70% free, zero cloud round-trips; uplink ~1 line/50 s vs 20 raw samples/s → **400× fewer messages** (confirm from payloads).

**Evidence:** `print_memory_info()` runtime output; [README § Architecture cheat sheet](README.md#architecture-cheat-sheet)

**Remaining work:**

- [ ] Define + run node-only (no agent) and agent-only (raw telemetry upstream) baselines
- [ ] Audit host-test/bench for streaming-evaluation purity: score each step with data < t only, no retrospective θ/normalizer fit (Mueller §3.1)
- [ ] Measure bytes/min up+down per architecture from MQTT payload logs
- [ ] Table: RAM / flash / CPU / bandwidth / messages per architecture
- [ ] Confirm the 400× message ratio with measured numbers

### RQ3: Accuracy scaling with more self-contained devices

> How does accuracy improve as more self-contained devices are added and
> communicate with each other?

**Current answer (N=1):** detection in 21.7 s with zero false alarms on normal and control data; z-baseline logged per run — the head-to-head "does learned prediction beat classical statistics?"

**Evidence:** `tests/host-test/` (900-step simulation + full link check); per-run z-baseline logs

**Remaining work:**

- [ ] Formalize metrics: detection latency, false-alarm rate, per-condition accuracy
- [ ] N-node runs (N = 1, 2, …) with shared condition libraries
- [ ] Plot accuracy/latency vs N; marginal gain per added device
- [ ] Compare vs single-node to isolate the communication benefit

## Literature anchors

> Verified via arXiv API / Crossref on 2026-11-02. The schema (`State` / `Action` / metric set) is an *output* of this review: each row says what the schema must expose. Per-paper snapshots in [`literature/`](literature/README.md); this section stays the analytical map. Paywalled items flagged.

### Adaptable RL parameters (core of the semantic-context link)

> No paper yet catalogues adaptable parameters for industrial condition monitoring. Strong-CPU evidence exists for streaming RL components (Stream-X: Atari + 50 continuous-control tasks, non-stationarity regimes); practice: adopt the best-evidenced standard parameters and ablate them on our multi-node system.

| Work | Source | Informs |
|------|--------|---------|
| Elsayed et al., Streaming Deep RL Finally Works (Stream-X) | arXiv:2410.14606 | what must adapt for streaming learning: running-moment normalisation, representation stabilisation, bounded updates (v3 StreamingOptimizer); ESP32-S3 demo |
| Ren et al., TinyOL | arXiv:2103.08295 | on-node MCU training feasible; rehearsal storage of previous-class data vs forgetting — backs our flash condition library |
| Dohare et al., Loss of Plasticity in Deep Continual Learning | arXiv:2310.02204 · Nature 632, 768–774 | catastrophic forgetting / plasticity canon; continual backprop; retention-vs-relearning framing for our flash condition library |
| Kearney et al., IDBD-lineage step sizes in TD learning | arXiv:1903.03252 | per-feature step sizes as the adaptable parameter; the step size *is* feature relevance |
| Degris et al., Step-size Optimization for Continual Learning | arXiv:2401.17401 | meta-gradient vs Adam/RMSProp heuristics — which adaptivity mechanism to expose |
| Sharifnassab et al., Intentional Updates for Streaming RL | arXiv:2604.19033 | step size in output units — parameterising the adaptivity knob |
| Lee et al., ReRe | arXiv:2004.02319 (COMPSAC 2020) | self-adaptive thresholds in lightweight streaming TS anomaly detection — the θ analogue |
| De Asis et al., Differential TD for Episodic Problems | arXiv:2605.04368 | reward centering — reward-scale adaptivity |
| Eschmann et al., RLtools | arXiv:2306.03530 · JMLR 25(301) · github.com/rl-tools/rl-tools | contrast: buffer-based SAC on MCUs (low-dim states, continuous control) — sharpens our no-storage claim (RQ2); its repo is the candidate baseline implementation for the small-buffer synthetic bench |

### What flows up (semantics) vs down (context) — to be settled by review

| Work | Source | Informs |
|------|--------|---------|
| Mueller, Open Challenges in TS Anomaly Detection: An Industry Perspective | arXiv:2502.05392 | industry call for streaming + human-in-the-loop — the gap statement |
| Zhou et al., Goal-Oriented Semantic Communications for 6G | arXiv:2210.09372 | task-relevant info vs raw data — uplink principle |
| Farshbafan & Saad, Curriculum Learning for Semantic Comm. with a Common Language | arXiv:2204.10429 | shared vocabulary between the two ends = the contract |
| Amershi et al., Power to the People | AI Magazine 2014 (paywalled) | operator feedback inside the learning loop — downlink principle |
| Islam et al., GLAD | arXiv:1810.01403 | label feedback retunes deployed detector parameters |
| Deng et al., HILAD | arXiv:2405.03234 | bidirectional human/model correction for TS anomaly detection |
| Settles, Active Learning | Springer 2012, DOI 10.1007/978-3-031-01560-1 | query-on-uncertainty canon — labels arrive only on open asks |
| IEC 63278; IEC 62541; OASIS MQTT 5.0 | standards | versioned machine-readable industrial contracts; MQTT = transport of record |

### Consumer + positioning

| Work | Source | Informs |
|------|--------|---------|
| Xia et al., Flexible modular production system with LLM agents | arXiv:2304.14721 | LLM agents operating industrial systems via structured interfaces |
| Romero & Suyama, Agentic AI for Intent-Based Industrial Automation | arXiv:2506.04980 | agentic AI aligned with Industry 5.0 human-centricity |
| Carvalho et al., SLR of ML for predictive maintenance | C&IE 2019, DOI 10.1016/j.cie.2019.106024 (paywalled) | batch-supervised PdM across sensor modalities |
| Andriotis & Papakonstantinou, DRL inspection & maintenance planning | RESS 2021, DOI 10.1016/j.ress.2021.107551 (paywalled) | contrast: RL for scheduling, not condition reporting |

## Experiment log

| Date | Setup | RQ | Result | Evidence |
|------|-------|:--:|--------|----------|
| 2026-11-03 | host-test 900-step sim; StreamingOptimizer (α=2e-3, β=0.99995, λ=0.8) + layer-aware sparse init | 1 | all stages green: cal/fit/quiet/alarm/persist/library; anomaly matched, label persisted | `tests/host-test.sh` |

## Design decisions & rationale

> One entry per "why did we build it this way". Append-only.

| Decision | Rationale | Alternatives rejected |
|----------|-----------|----------------------|
| ObGD, not MVP histograms | net learns joint one-step dynamics; overshooting bound keeps bursts sane | MVP histograms (README § Architecture cheat sheet) |
| StreamingOptimizer replaces ObGD (ported 2026-11-03; v2 via `-DMONITOR_USE_OBGD`) | v3 App. A: per-coordinate displacement < α, no 1/d shrinkage as nets grow; host-test green at α=2e-3, β=0.99995, λ=0.8 | keep v2-era ObGD |
| Layer-aware sparse init (global 95% budget + connectivity floor) | v3 App. C: uniform 90% zeroed ALL first-layer weights at fan-in 4 — node booted deaf; floor guarantees ≥1 nonzero input per unit | uniform 90% per-layer |
| Schema exposes only site-dependent knobs (θ, labels, phase); streaming self-adaptation stays node-local | Stream-X holds one fixed config across tasks — internal adaptation thins what the contract carries; site-relative values are what only external context supplies | expose all RL parameters; fully static node |
| Stream-TD (prediction branch), not Stream-Q/AC | condition monitoring has no actions to take; detection needs only prediction error; control variants stay in the submodule for future actuation | Stream-Q / Stream-AC on the monitor node |
| Bench protocol = streaming evaluation (R_o) only | Mueller §3.1: batch benchmarks mislead practitioners; every RQ2/RQ3 baseline scores each step with data < t, no retrospective θ/normalizer fit | batch evaluation as in TAD benchmark suites |

## Deliverables

- [ ] Report: RQ2 measured ratio table
- [ ] Report: RQ3 scaling curves
