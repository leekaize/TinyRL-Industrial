# GLAD: GLocalized Anomaly Detection via Human-in-the-Loop Learning

| | |
|---|---|
| Source | arXiv:1810.01403 (v4, 2020-07-15) · code: [github.com/shubhomoydas/ad_examples](https://github.com/shubhomoydas/ad_examples) |
| Authors | Md Rakibul Islam, Shubhomoy Das, Janardhan Rao Doppa (WSU), Sriraam Natarajan (UT Dallas) · 2018/2020 |
| Venue | ICML 2020 Workshop on Human in the Loop Learning |
| Last viewed | 2026-11-11 (arXiv HTML full text v4) |
| Pillar | 3-link |
| Informs | Label feedback retunes deployed detector parameters |
| Status | ✅ snapshot 2026-11-11 |

## Key concepts

- **Glocal**: both global and local. A global detector (one rule for the whole feature space) is wrong in at least some local regions; the fix is to learn *where* to trust it, not to discard it.
- **Local relevance**: p_m(x) ∈ [0,1], the probability that ensemble member m is trustworthy at instance x. Final score: `Score(x) = Σ_m s_m(x)·p_m(x)`.
- **FSSN (Feature Space Suppression Network)**: shallow net with M sigmoid outputs (one per detector). Primed to output a constant b = 0.5 everywhere = **uniform prior** over relevance; a uniform ensemble average is a good starting point before any labels.
- **AAD hinge loss**: anomalies must score above the τ-quantile instance's score q_τ, nominals below it (τ = expected anomaly fraction). FSSN loss = AAD loss over labeled set + λ·prior loss (λ = 1).
- **Active loop**: greedy query of the current top-scoring unlabeled instance, analyst labels nominal/anomaly, FSSN retrained on all labeled data (up-sampled 5×, L2-regularized, max(50, 3M) hidden nodes). Pool-based.
- **Explanations**: pick the most relevant member for an instance, train a per-member decision tree describing its relevant subspaces, then apply LIME/ANCHORS to that member.

## Methods

- Detector: LODA ensemble (≤15 sparse random 1-d histogram projections, Pevný 2015), pre-trained and frozen; only relevance weights adapt.
- Data: toy synthetic + 6 real datasets (Abalone 1.5%, ANN-Thyroid-1v3 2.25%, Cardiotocography 2.65%, KDD-Cup-99 3.83%, Mammography 2.32%, Yeast 4.6% anomalies).
- Metric: anomalies discovered vs number of labeled queries; 10-run averages.
- Result: GLAD beats plain LODA and LODA-AAD (global per-member weights, Das et al. 2016) on all tested datasets; toy maps show each projection's relevance localized to a half-space.
- Limitations (authors'): explanations qualitative, unevaluated on real data; pool-based batch setting assumes a responsive oracle; FSSN retrained per label is not streaming.

## Snapshot

Practitioners keep using simple global anomaly detectors because they are explainable, but unsupervised global detectors are biased somewhere: every projection, histogram, or rule fails in some regions of the feature space, which is where false alarms come from. GLAD keeps the ensemble and learns the local relevance of each member from human label feedback. A Feature Space Suppression Network is primed so every detector looks equally relevant everywhere (uniform prior, so scoring starts as the plain ensemble average), then each label is turned into AAD hinge-loss constraints (anomalies above the τ-quantile score, nominals below) plus the prior term, and the network is retrained on all labeled instances. Greedy active learning queries the top-scoring unlabeled instance each round.

On six real datasets with a ≤15-projection LODA ensemble, GLAD discovers more anomalies per analyst query than both the unweighted ensemble and LODA-AAD's globally learned weights, showing relevance must be local, not one weight per detector. GLAD also produces explanations: identify the most relevant member for an instance, describe its relevant subspace with a decision tree, and apply LIME/ANCHORS on that member.

Limitations: pool-based batch setting with a responsive oracle; per-label retraining; explanation quality unevaluated.

## Schema implications (TinyRL-Industrial)

- **Per-condition θ is the "glocal" lesson applied to our knob**: our global θ = μ + 4σ is a global detector whose local failure modes GLAD predicts; labels from `Action/feedback` could tune per-condition μ/σ thresholds instead (RQ1: makes θ a per-condition, schema-exposed value; candidate synthetic-bench experiment, already an open question from Mueller).
- **The split GLAD supports**: labels flow down through the schema (schema-exposed); the specialization they induce (relevance weights, per-condition stats) is learned and stored on-node (node-local). Strengthens the existing design decision without changing it.
- **Uniform-prior priming = our CAL phase**: before feedback arrives, all conditions and the global θ are treated equally; the library's condition centroids are the local regions GLAD's FSSN would discover, pre-seeded by our pipeline instead of learned from scratch.
- **`meas` field = our explanation payload** (verified in `uns_messaging.h`: episode summaries carry `{<feature>:<value>}`). GLAD's LIME-style explanations need per-feature contributions; our summaries already ship them, so the agent-side explanation needs no schema change.
- **Oracle model differs**: GLAD assumes a responsive analyst for any queried instance (pool-based); Mueller's one-sided feedback reality and our alarm-gated asks are the weaker oracle. GLAD is evidence that even weak, occasional labels can retune deployed detectors (RQ2 relevance: label volume needed is small).
- **FSSN retrain-per-label is anti-MCU**: our net trains per sample incrementally; the analogue is that per-condition stats update on each labeled episode, no batch retraining (RQ2: keeps learning buffer-free).

## Proposed future work linked to our research

| Theirs | Ours |
|---|---|
| Local relevance weights per detector (glocal) | Per-condition μ/σ threshold instead of global θ (RQ1 ablation, Mueller open question) |
| AAD loss on labels | `normal`/`fault` feedback already enforces score ordering via θ governance |
| Explanations via most-relevant member + LIME | Agent-side explanation from episode `meas` + `condition_id`; no schema change |
| Pool-based greedy queries with responsive oracle | Stream-based alarm-gated asks (Settles scenario 2 + Mueller one-sided reality) |

## Open questions

- [ ] Per-condition threshold experiment: does labeled-episode-driven per-condition μ/σ beat global θ = μ + 4σ on false alarms? (Synthetic bench; RQ1.)
- [ ] Does per-condition θ need a schema field, or do per-condition stats live only in `/conditions.jsonl` on-node? (RQ1 boundary.)
- [ ] GLAD's τ (anomaly fraction) is known; ours is not. Does an assumed τ break the AAD-style ordering constraint on our low-incident-rate site?
- [ ] RQ3: per-site relevance (which conditions matter where) is exactly the site-local knowledge multi-node sharing must respect; test whether shared labels from sibling nodes help or hurt a site's per-condition stats.
