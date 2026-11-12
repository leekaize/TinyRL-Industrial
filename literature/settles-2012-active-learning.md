# Active Learning, ch. 2: Uncertainty Sampling

| | |
|---|---|
| Source | DOI 10.1007/978-3-031-01560-1 (Springer Synthesis Lectures on AI and ML, eBook 2022; © 2012) |
| Authors | Burr Settles (Carnegie Mellon) · 2012 |
| Venue | Springer book (100 pp) |
| Last viewed | 2026-11-12 (ch. 2 full text: `.workbook/settles-2012-uncertainty-sampling.pdf`) |
| Pillar | 3-link |
| Informs | Query-on-uncertainty canon: labels arrive only on open asks |
| Status | ✅ snapshot 2026-11-12 |

## Key concepts

- **Active learning**: the learner chooses which data to label. Queries go to an **oracle** (human annotator); goal is more accuracy per label, since labels are expensive and unlabeled data is cheap.
- **Uncertainty sampling (ch. 2)**: query the instance the current model is least confident about. Three measures: **least confident** (minimize the top posterior, expected 0/1-loss), **margin** (minimize the top-two posterior gap), **entropy** (maximize predictive entropy, expected log-loss, most general). For binary classification all three are monotonic transformations of each other and reduce to "closest to the decision boundary"; for three or more labels they differ (entropy suits log-loss objectives, margin suits 0/1 error). Cheapest and most used strategy.
- **Stream-based selective sampling (ch. 2.5)**: implement uncertainty as a **region of uncertainty**: threshold the uncertainty measure, query arrivals that fall inside it, discard the rest, retrain after each labeled query (their worked example discards >1,000 uninformative instances while learning a target region with 100 queries).
- **Ch. 2's stated weakness is myopia, not outliers**: the utility score comes from a single hypothesis trained on little, actively-biased data, so the learner can become confidently wrong about whole regions and never query them (the two-triangles failure: it learns a convex-hull shape and avoids the true boundary; observed to underperform random sampling in applied studies).

## Methods

- Survey/synthesis lecture: definitions, strategy frameworks, no new algorithm. Evidence = cited experiments per strategy (e.g., Lewis & Catlett 1994 text classification).
- Scope = ch. 2 only (read in full 2026-11-12): measures of uncertainty (2.3), generalization beyond classification (2.4), stream-based selective sampling and the myopia failure (2.5). Ch. 1 and ch. 3 onward intentionally out of scope.

## Snapshot

Settles defines active learning: the learner chooses which data to label, posing queries to an oracle, because labels are expensive while unlabeled data is abundant. These notes draw only on ch. 2, uncertainty sampling, the simplest and most used strategy.

Chapter 2 covers uncertainty sampling, the simplest and most used framework: query the instance the model is least confident about. Three measures formalize this: least confident (minimize the top posterior, expected 0/1-loss), margin (minimize the gap between the top two, adds discrimination information), and entropy (maximize predictive entropy, expected log-loss). For binary problems all three are monotone transformations of one another and reduce to querying nearest the decision boundary. The chapter also gives the stream-based variant used in applied work: threshold the uncertainty measure into a region of uncertainty and query only arrivals that fall inside it, discarding the rest. Its stated weakness is myopia: the utility comes from a single hypothesis trained on little, actively-biased data, so it can grow confident about whole wrong regions (the two-triangles example) and never query them, sometimes performing worse than random sampling.

Limitations: oracle assumed mostly correct and cheap enough to query; strategies are evaluated on classification, not anomaly detection with one-sided feedback.

## Schema implications (TinyRL-Industrial)

- **Our node already runs stream-based selective sampling** (verified: `nodes/core/uns_messaging.h` has no polling/label-request mechanism; the only input channel is `Action/feedback`, and an episode with `condition_id:"unknown"` is the node's ask). Stream-based selective sampling is our scenario, oracle = agent/human over MQTT (RQ1).
- **Ask mechanism is a thresholded region of uncertainty**: an ask fires only when an episode is open (θ-based alarm) AND no library centroid is within radius r. That is exactly ch. 2.5's selective-sampling recipe (threshold the uncertainty measure, query inside, discard outside), with the alarm as an extra oracle-cost gate. Decided 2026-11-12: alarm-gating stands; quiet-unmatched conditions get counted node-side (myopia tripwire), not asked about.
- **Labels stay schema-exposed** (RESEARCH.md parameter split, Settles row): the oracle's answer (`label`/`normal`/`fault`) is context only the outside world can supply; the query policy (when to ask) stays node-local.
- **Myopia applies to us, not the outlier trap**: a node confidently wrong about a whole quiet regime never alarms there, so the alarm gate never opens an ask in that region. The tripwire: count quiet-unmatched steps locally; a persistently high count = a confidently-blind region worth human attention. Mueller's one-sided feedback still requires auditing ask rate + label distribution.

## Proposed future work linked to our research

| Theirs | Ours |
|---|---|
| Stream-based selective sampling (query or discard) | `condition_id:"unknown"` ask inside an alarm episode; oracle = `Action/feedback` |
| Myopia (confidently-wrong regions) | Quiet-unmatched counter node-side + ask-rate/label audit (host-test metric) |

## Open questions

- [ ] Are our unknown-asks informative? Log ask rate + oracle label distribution per node before trusting the loop (Settles myopia + Mueller one-sided feedback both predict noise).
- [x] Decided 2026-11-12: alarm-gated asks only; quiet-unmatched conditions counted node-side (RESEARCH.md design decision). Revisit quiet asks only if the counter shows persistent drift.
- [ ] Is `condition_id:"unknown"` sufficient as the query mechanism, or should the schema carry an explicit ask action? (RQ1 schema-cleanliness; current design reuses the episode, which the schema already carries.)
