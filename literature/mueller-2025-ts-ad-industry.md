# Open Challenges in Time Series Anomaly Detection: An Industry Perspective

| | |
|---|---|
| Source | arXiv:2502.05392 (v1, 2025-02-08) |
| Authors | Andreas C. Müller (Microsoft, Redmond) · 2025 |
| Venue | arXiv preprint (position paper; no new algorithm) |
| Last viewed | 2026-11-11 (arXiv HTML full text) |
| Pillar | 2-monitoring |
| Informs | Industry call for streaming + human-in-the-loop: the gap statement |
| Status | ✅ snapshot 2026-11-11 |

## Key concepts

- **TAD (time-series anomaly detection)**: flag unusual moments in sensor or business series.
- **Tenet 1 (Alerting)**: deployments exist to trigger a reaction now (human or automation). Alerting and mitigation, not offline analysis.
- **Tenet 2 (Application-specific)**: whether an event is "anomalous" depends on the consumer of the signal, not the data alone. Freezer: −81 °C dip (power outage) is ops-irrelevant; −72 °C spikes (door open) is business-critical. Both are statistical outliers.
- **Streaming evaluation**: score each point using only data that existed before it, `R_o = Σ ℓ(l_t, A(x_<t)_t)`, vs batch benchmarks that fit on the whole series. The requirement is streaming *evaluation*, not just streaming compute.
- **Populations**: real alerting runs on fleets of similar series (all freezers, all rooms, all nodes); alert at the most general useful level (subpopulation/region), not per-series.
- **Conditional anomalies**: judge `x_t` given context at that time, `p(x_t | x_<t, y_<t+1, z_<t+1)` (his Eq 4), so zero sales on a holiday is normal. Literature only studies the joint multivariate question (Eq 3).
- **One-sided human feedback**: operators only see raised alerts, so they label false positives only; missed anomalies never reach them → censored, bandit-like feedback (you only observe outcomes of alerts you actually sent).
- **Plumbing**: point processes need resampling (can create/hide anomalies); periodicity/window-size selection is unsolved; threshold setting: benchmarks use ideal thresholds computed from the full series + ground truth.

## Methods

- Position paper; evidence = systems deployed at a cloud provider + one worked example (ultra-low-temperature freezers, pharma storage).
- §4 formalization: online algorithm `A(x_<t, L_<t)` where `L_<t` = labels of previously flagged points only; regret `R_hil` with separate costs λ_fp, λ_fn. Framed as censored-feedback bandit or RL in an augmented state.
- Appendix A: 1000 synthetic series (random walk + noise + periodic random walk). "Peaks" ACF heuristic 0.754 accuracy vs FFT 0.169, SuSS 0.008, random 0.007: simple heuristics beat named methods; all fail on visually-solvable cases.

## Snapshot

Müller, drawing on anomaly-detection systems deployed at a cloud provider, argues that time-series anomaly detection research optimizes the wrong problem. Two tenets from practice: deployments exist for alerting (a human or automation must react now), and whether an event is an anomaly is application-specific, not intrinsic to the data. His freezer example: a −81 °C dip (power outage) and −72 °C spikes (door left open) are both statistical outliers, but only the door event matters to food-safety operations.

From the tenets he derives requirements the literature ignores. Streaming evaluation: score each point using only data that existed before it; all major benchmarks instead fit on the full series, so their rankings give practitioners no guidance. Populations: deployments monitor fleets of similar series; alerting should happen at the most general useful level. Conditional anomalies: judge a value given context at that time, `p(x_t | x_<t, y_<t+1, z_<t+1)`, so zero sales on a holiday is normal; the literature only studies the joint multivariate question. Human-in-the-loop: since relevance is application-specific, feedback is essential, but it is one-sided: operators label only raised alerts (false positives), never missed anomalies, giving censored, bandit-like feedback he formalizes as an online problem with separate false-positive/false-negative costs.

He also flags plumbing: resampling point processes can create or hide anomalies; window-size selection is unsolved (his synthetic benchmark: autocorrelation heuristics beat named methods); and benchmark thresholds are computed from the full series plus ground truth, which is unusable when streaming.

Limitations: proposals only, no deployed algorithm; evidence is cloud telemetry, not industrial control.

## Schema implications (TinyRL-Industrial)

- His **conditional anomalies** = our condition library: episodes are judged relative to learned centroids (`/conditions.jsonl`). Condition identity + labels are schema-exposed (RQ1); scoring stays node-local.
- His **one-sided feedback** = uns-v1.1 `Action/feedback` downlink (`label`/`normal`/`fault`) answering `condition_id:"unknown"` asks, matching his censored-feedback formulation. Schema-exposed (RQ1).
- **Application-specific** ⇒ θ cannot be derived on-node → θ governance (`normal` +10% / `fault` −5%) stays schema-exposed (design decision); his §5.3 makes θ a first-class object → supports the planned node-local-vs-schema θ ablation (RQ1).
- **Streaming evaluation** = methodology rule for every bench: score each step with only past data (R_o-style); no full-series fits in RQ2/RQ3 baselines.
- **Populations** = RQ3: `State/node_info` + condition labels give the agent what subpopulation mining needs; cohort-level alerting is agent-side (Skill 4), nothing added to the node.
- **Point-process warning**: the node resamples raw streams into windows before learning, so resampling semantics stay node-local; the agent gets episode summaries (`meas`), never raw streams (Skill 5 invariant).

## Proposed future work linked to our research

| Theirs | Ours |
|---|---|
| Censored-feedback bandit formulation (§4) | uns-v1.1 one-sided loop: unknown → agent/human label |
| Conditional TAD (Eq 4) | per-condition scoring on our z-space (untested) |
| Populations analysis | multi-node shared condition libraries (RQ3) |

## Open questions

- [ ] Is our host-test/bench strictly streaming evaluation (no retrospective fit of normalizer or θ)? Audit before the RQ2/RQ3 baselines.
- [ ] His λ_fp/λ_fn cost asymmetry: is +10%/−5% governance justified or arbitrary? Ablate.
- [ ] Would per-condition μ/σ (conditional scoring) cut false alarms vs the global θ = μ + 4σ? Candidate synthetic-bench experiment.
- [ ] Does the schema need anything beyond `node_info` for cohort grouping, or is it purely agent-side? (RQ3)
