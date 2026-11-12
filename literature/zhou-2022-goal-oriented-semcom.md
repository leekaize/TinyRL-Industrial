# Goal-Oriented Semantic Communications for 6G Networks

| | |
|---|---|
| Source | arXiv:2210.09372 (v3, 2024-04-06) · DOI 10.1109/iotm.001.2300269 |
| Authors | Hui Zhou, Yansha Deng, Xiaonan Liu, Nikolaos Pappas, Arumugam Nallanathan (King's College London, QMUL, Linköping) · 2022 |
| Venue | IEEE Internet of Things Magazine, Sep 2024 |
| Last viewed | 2026-11-11 (arXiv HTML full text v3; Crossref) |
| Pillar | 3-link |
| Informs | Task-relevant info vs raw data — uplink principle |
| Status | ✅ snapshot 2026-11-11 |

## Key concepts

- **Three levels (Shannon/Weaver)**: technical = bits arrive correctly; semantic = the meaning the sender intended arrives; effectiveness = the receiver's action completes the task. Classic comms solve level 1 only.
- **Goal-oriented semantic communication**: transmit the task-relevant part of the data, and score success by task outcome metrics, not channel metrics (bit error rate).
- **Semantic info differs per data type** (their Table I): text/speech → embedding (a vector summarizing meaning, via BERT); image → edges/corners; video → temporal correlation (what changed between frames); 360° video → field of view (only render where the eye looks); haptic → JND (just noticeable difference: changes too small to feel; Weber's law = detectable change scales with baseline magnitude); **sensor/control → AoI + VoI**; ML models → FL/SL weights, "no explicit semantic definition yet" (their words).
- **AoI (age of information)**: how stale the newest received update is. **VoI (value of information)**: how much this sample changes the decision it feeds.
- **Effectiveness-aware metrics**: per-task outcomes — F-measure/accuracy (speech), IoU/mAP (detection), LQG cost (control; how well the controller tracks), convergence speed (FL/SL).
- **Effectiveness feedback**: extraction is driven by feedback of how well the task executed — the loop that decides what is worth sending next.

## Methods

- Magazine framework paper: taxonomy of data types → semantic info → extraction method → recovery method → per-task metrics (their Tables I-II); time-critical vs non-critical variants; one-hop and chain (multi-link) tasks.
- Case study (only quantitative part): UAV command & control (C&C). Ground station repeats commands periodically (e.g., yaw rate [30,30,30,0,0,0] °/s). DRL (deep RL) at the base station learns to transmit only high-VoI C&C packets: repeats collapse to [30,0].
- Result: >60% fewer transmissions, same effective transmission rate as conventional (Fig 3); transmission count stays flat as repeat count k grows.
- No new general algorithm; no dataset beyond the case study.

## Snapshot

The paper frames communication at three levels: bits arrive (technical), intended meaning arrives (semantic), and the receiver's action completes the task (effectiveness). Conventional systems approaching Shannon's capacity limit solve only the first level, so emerging 6G workloads (XR, UAVs) miss latency requirements by carrying raw data. Their proposal: a goal-oriented framework that transmits semantic-level information and scores it with effectiveness-aware metrics. The semantic content depends on data type: embeddings for text, edges for images, temporal correlation for video, field of view for 360° video, just-noticeable-difference for haptics, and for sensor/control data, freshness (AoI) plus importance (VoI). Extraction is driven by feedback of task-execution effectiveness, closing the loop between what the task needs and what gets sent. For each task they fix a recovery method and outcome metrics (F-measure, IoU, PSNR, LQG control cost, convergence speed) instead of channel metrics.

The UAV case study validates the uplink principle: command & control packets repeat consecutively, so a deep-RL scheduler at the base station transmits only packets whose value of information is high. Repeated commands collapse ([30,30,30,0,0,0] → [30,0]), cutting over 60% of transmissions at the same effective transmission rate.

Limitations (authors'): framework and taxonomy, not a general algorithm; sensor semantics reduced to AoI/VoI; no explicit semantic definition exists yet for ML-model data; accuracy-latency tradeoff unsolved; single-task validation.

## Schema implications (TinyRL-Industrial)

- **Their uplink principle is our verified design rule**: `nodes/core/uns_messaging.h` header states "publish CONDITION + STATUS, not telemetry. Raw values travel only inside episode summaries (already aggregated on-device)". Grep of `nodes/` finds no periodic telemetry or heartbeat publish. Node-local extraction = episode/condition logic on-device (RQ1: what the schema must carry; RQ2: what it must not).
- **VoI ≈ our episode trigger**: `State/condition` retained, published on change; `State/event` per episode transition. Publishing on change (not on schedule) is exactly their repeat-collapse rule; our ~400× message ratio (RQ2) is the same claim at larger gain, to confirm from payload logs.
- **Effectiveness feedback loop = `Action/feedback`** (`label`/`normal`/`fault`/`start`, the only subscribed input): task outcome flows back and tunes θ, fills the condition library. Schema-exposed (RQ1); mirrors their framework's effectiveness-feedback component.
- **Sensor row of their Table I** justifies episode metadata: `ts_ms`, `episode_s`, `severity`, `meas` in `State/event` are freshness + importance fields; nothing more needed for their AoI/VoI definition.
- **Recovery column**: control tasks need no receiver-side reconstruction; our agent likewise never rebuilds raw streams (Skill 5 invariant; matches Mueller's resampling warning). Summaries (`meas`) are the payload.
- **Chain tasks (robot swarms with deadlines) = RQ3 sketch**: multiple self-contained nodes + one shared vocabulary; our schema uns-v1.1 is that contract (Farshbafan row develops the "common language" side).
- **Their "no explicit semantic definition for ML models" gap**: our condition library (z-space centroids + labels in `/conditions.jsonl`) is a machine-readable semantic definition of learned model state — a contribution angle for the writeup.

## Proposed future work linked to our research

| Theirs | Ours |
|---|---|
| Uplink = high-VoI samples only (UAV C&C case) | Episode/state-change-only uplink, zero periodic telemetry (verified in `uns_messaging.h`) |
| Effectiveness feedback drives extraction | `Action/feedback` drives θ governance + condition-library growth (RQ1 ablation queued) |
| Chain tasks: swarm of robots, shared protocol, deadline | RQ3: N-node runs sharing the condition library; marginal accuracy per added device |

## Open questions

- [ ] RQ2: confirm the ~400× uplink message ratio from MQTT payload logs (their Fig 3(a) is the analogue to reproduce on our link).
- [ ] Is `State/event` severity + confidence sufficient as an explicit VoI field, or should the schema carry one? Default: no — event-on-change already encodes importance.
- [ ] Measure feedback-channel traffic (downlink messages/min) so the RQ2 architecture table covers both directions, as their framework requires.
