# Literature anchors

Verified via arXiv API / Crossref on 2026-11-02. The schema (`State` / `Action` / metric set) is an output of this review: each row says what the schema must expose. Per-paper snapshots live in [`literature/`](https://github.com/leekaize/TinyRL-Industrial/tree/main/literature) in the repo (one file per paper, snapshot template `literature/_template.md`); this page stays the analytical map. Paywalled items flagged.

## Adaptable RL parameters (core of the semantic-context link)

> No paper yet catalogues adaptable parameters for industrial condition monitoring. Strong-CPU evidence exists for streaming RL components (Stream-X: Atari + 50 continuous-control tasks, non-stationarity regimes); practice: adopt the best-evidenced standard parameters and ablate them on our multi-node system.

| Work | Source | Informs |
|------|--------|---------|
| Elsayed et al., Streaming Deep RL Finally Works (Stream-X) | [arXiv:2410.14606](https://arxiv.org/abs/2410.14606) | what must adapt for streaming learning: running-moment normalisation, representation stabilisation, bounded updates (v3 StreamingOptimizer); ESP32-S3 demo |
| Ren et al., TinyOL | [arXiv:2103.08295](https://arxiv.org/abs/2103.08295) | on-node MCU training feasible; rehearsal storage of previous-class data vs forgetting - backs our flash condition library |
| Dohare et al., Loss of Plasticity in Deep Continual Learning | [arXiv:2306.13812](https://arxiv.org/abs/2306.13812) - Nature 632, 768-774 | catastrophic forgetting / plasticity canon; continual backprop; retention-vs-relearning framing for the flash condition library |
| Kearney et al., IDBD-lineage step sizes in TD learning | [arXiv:1903.03252](https://arxiv.org/abs/1903.03252) | per-feature step sizes as the adaptable parameter; the step size IS feature relevance |
| Degris et al., Step-size Optimization for Continual Learning | [arXiv:2401.17401](https://arxiv.org/abs/2401.17401) | meta-gradient vs Adam/RMSProp heuristics - which adaptivity mechanism to expose |
| Sharifnassab et al., Intentional Updates for Streaming RL | [arXiv:2604.19033](https://arxiv.org/abs/2604.19033) | step size in output units - parameterising the adaptivity knob |
| Lee et al., ReRe | [arXiv:2004.02319](https://arxiv.org/abs/2004.02319) (COMPSAC 2020) | self-adaptive thresholds in lightweight streaming TS anomaly detection - the theta analogue |
| De Asis et al., Differential TD for Episodic Problems | [arXiv:2605.04368](https://arxiv.org/abs/2605.04368) | reward centering - reward-scale adaptivity |
| Eschmann et al., RLtools | [arXiv:2306.03530](https://arxiv.org/abs/2306.03530) - JMLR 25(301) - [github.com/rl-tools/rl-tools](https://github.com/rl-tools/rl-tools) | contrast: buffer-based SAC on MCUs (low-dim states, continuous control) - sharpens the no-storage claim (RQ2); its repo is the candidate baseline implementation for the small-buffer synthetic bench |

## What flows up (semantics) vs down (context)

| Work | Source | Informs |
|------|--------|---------|
| Mueller, Open Challenges in TS Anomaly Detection: An Industry Perspective | [arXiv:2502.05392](https://arxiv.org/abs/2502.05392) | industry call for streaming + human-in-the-loop - the gap statement |
| Zhou et al., Goal-Oriented Semantic Communications for 6G | [arXiv:2210.09372](https://arxiv.org/abs/2210.09372) | task-relevant info vs raw data - the uplink principle |
| Farshbafan & Saad, Curriculum Learning for Semantic Comm. with a Common Language | [arXiv:2204.10429](https://arxiv.org/abs/2204.10429) | shared vocabulary between the two ends = the contract |
| Amershi et al., Power to the People | AI Magazine 2014 (paywalled) | operator feedback inside the learning loop - the downlink principle |
| Islam et al., GLAD | [arXiv:1810.01403](https://arxiv.org/abs/1810.01403) | label feedback retunes deployed detector parameters |
| Deng et al., HILAD | [arXiv:2405.03234](https://arxiv.org/abs/2405.03234) | bidirectional human/model correction for TS anomaly detection |
| Settles, Active Learning | Springer 2012, [DOI 10.1007/978-3-031-01560-1](https://doi.org/10.1007/978-3-031-01560-1) | query-on-uncertainty canon - labels arrive only on open asks |
| IEC 63278; IEC 62541; OASIS MQTT 5.0 | standards | versioned machine-readable industrial contracts; MQTT = transport of record |

## Consumer + positioning

| Work | Source | Informs |
|------|--------|---------|
| Xia et al., Flexible modular production system with LLM agents | [arXiv:2304.14721](https://arxiv.org/abs/2304.14721) | LLM agents operating industrial systems via structured interfaces |
| Romero & Suyama, Agentic AI for Intent-Based Industrial Automation | [arXiv:2506.04980](https://arxiv.org/abs/2506.04980) | agentic AI aligned with Industry 5.0 human-centricity |
| Carvalho et al., SLR of ML for predictive maintenance | C&IE 2019, [DOI 10.1016/j.cie.2019.106024](https://doi.org/10.1016/j.cie.2019.106024) (paywalled) | batch-supervised PdM across sensor modalities |
| Andriotis & Papakonstantinou, DRL inspection & maintenance planning | RESS 2021, [DOI 10.1016/j.ress.2021.107551](https://doi.org/10.1016/j.ress.2021.107551) (paywalled) | contrast: RL for scheduling, not condition reporting |
