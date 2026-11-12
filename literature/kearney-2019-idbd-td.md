# Learning Feature Relevance Through Step Size Adaptation in Temporal-Difference Learning

| | |
|---|---|
| Source | arXiv:1903.03252 (v1, 2019-03-08) |
| Authors | Alex Kearney, Vivek Veeriah, Jaden Travnik, Patrick M. Pilarski, Richard S. Sutton (Borealis AI / Univ. of Alberta) · 2019 |
| Venue | arXiv preprint (full version of the RLDM 2017 workshop paper "Every step you take") |
| Last viewed | 2026-11-12 (arXiv HTML full text v1) |
| Pillar | 1-learning |
| Informs | Per-feature step sizes as the adaptable parameter; the step size itself encodes feature relevance |
| Status | ✅ snapshot 2026-11-12 |

## Key concepts

- **IDBD (Sutton 1992)**: per-feature step sizes learned by stochastic meta-descent. α_i = exp(β_i); meta-weights β_i ← β_i + θ δ x_i h_i, where **h** is a decaying trace of recent meta-updates. Consecutive updates in the same direction grow α_i; alternating updates (overshoot) shrink it.
- **Step size = feature relevance**: a feature with a large step size generalizes more from training examples, so the adapted step sizes read as learned, semantic feature relevance. The paper's framing: meta-learning as representation learning by weighting given features.
- **TIDBD**: IDBD generalized to TD(λ), where the target is bootstrapped and the gradient is biased. **Semi-gradient** version (β_i += θ δ φ_i(s) h_i) is robust to the meta step size; **ordinary-gradient** version (uses γφ(s') − φ(s)) matches tuned TD when well tuned. The h trace is now modulated by eligibility traces.
- **AutoTIDBD**: adds AutoStep's two normalizations: (1) divide the meta-update by η, a **decayed running max** of recent weight updates; (2) cap the step size by the **effective step size** (the error reduction the update would cause), so an update never overshoots the current example.
- **Robustness claim**: AutoTIDBD performs as well as or better than tuned fixed-α TD for every initial step size tried, and its good meta-step-size region is broad and stable across prediction problems. The tuning burden is lifted, not shifted. Symbol note: their θ is the meta step size, unrelated to our detection θ; below it is written θ_meta.

## Methods

- Derivations: Alg. 1 IDBD (supervised LMS), Alg. 3 TIDBD(λ) semi-gradient, Alg. 4 TIDBD(λ) ordinary gradient, Alg. 5 AutoStep, Alg. 6 AutoTIDBD.
- **Gridworld** (single shared step size, to isolate meta-learning benefits): 5×5 MRP from Sutton & Barto, equiprobable random policy, γ = 0.99, λ = 0, 15,000-step trials, 30 trials, θ_meta swept 0-0.2, α_0 ∈ {0.01, 0.05, 0.5}.
- **Robotic prediction**: BentoArm myoelectric-control dataset (Edwards et al. 2016), 4 participants × 6 trials = 24 series; tile coding with 2^10 features, 8 tilings, 9 active features (incl. bias); predictions of position/velocity/load for all 5 servos; γ = 0.95; λ 0-0.9; baselines SID, NOSID, AutoSID, AlphaBound, RMSProp.
- **Representation-learning test**: 25% of features replaced by equiprobable noise; step-size magnitudes of noisy features shrink steadily and separate cleanly from real features (no overlap with the normal range).

## Snapshot

Kearney et al. treat step sizes as the carrier of representation learning: rather than constructing features, learn how much to generalize to each one. In IDBD a linear learner keeps one step size per feature, α = exp(β), and updates the meta-weights by stochastic meta-descent: β_i += θ_meta δ x_i h_i, with h a decaying trace of recent meta-updates. Consecutive same-direction updates raise a feature's step size; alternating updates lower it. The learned values read as feature relevance: features that keep agreeing with the error gain influence on the learned function.

The contribution is carrying this to temporal-difference learning, where the bootstrapped target makes the gradient biased. They derive TIDBD(λ) twice: a semi-gradient variant (meta-update θ_meta δ φ_i h_i) that is insensitive to the meta step size but has a slightly higher error floor, and an ordinary-gradient variant that includes γφ(s') − φ(s) and matches tuned TD at the best setting. AutoTIDBD then adds AutoStep's normalizations: a decayed running max of recent weight updates scales the meta-update, and the effective step size caps the step so it never overshoots the current example.

Experiments: on a 5×5 gridworld, AutoTIDBD matches or beats tuned TD for every initial step size and is far less sensitive to θ_meta. On the myoelectric arm dataset (24 trials, 2^10 tile-coded features, λ 0-0.9), it attains lower or equal error to TD and beats SID, NOSID, AutoSID, and RMSProp, with a broad, problem-invariant θ_meta valley across ten distinct prediction problems. With 25% noisy features, their step sizes shrink and separate cleanly. Limitations: linear function approximation, on-policy predictions, replacing/accumulating traces only.

## Schema implications (TinyRL-Industrial)

- **Parameter split (RQ1)**: per-feature step sizes are node-local, per the RESEARCH.md row ("Kearney; Degris | node-local; optional ablation, mixed evidence vs Adam heuristics"). The step size is a semantic quantity (feature relevance), which makes exposing it tempting; the contract stays thin because the node already ships per-feature summaries (`State/event` `meas`) for explanation. Default: no schema change.
- **AutoStep ↔ our optimizer, code-verified**: AutoStep's decayed running max of recent updates is the same normalization shape as our StreamingOptimizer leaky max, `v_i = max(β·v_i, |u_i|)`, `|move| < lr` (`streaming_optimizer.h`). Kearney supplies TD-lineage evidence that decayed-max normalization yields broad, problem-invariant robustness: one fixed internal config across sites, i.e., nothing site-dependent to expose (RQ1).
- **Static floor vs dynamic cap**: our `eps = 1.0` unit-signal floor in `w += lr·u/(v + eps)` is a static guarantee for z-scored signals; AutoStep's effective-step-size cap is the dynamic version (never overshoot on the current example). Compare the two update rules on paper before porting anything (RQ1 ablation note).
- **Feasibility**: eligibility traces already exist per weight (`etraces_` in `streaming_optimizer.h`), so per-weight β with α = exp(β) would slot into the same step loop; the input layer is 4 features, hidden layers multiply state. TIDBD is derived for linear TD; our Stream-TD net is a small MLP, which is why the Degris 2024 row follows this one in the anchor table.

## Proposed future work linked to our research

| Theirs | Ours |
|---|---|
| AutoTIDBD per-feature step sizes | optional RQ1 ablation vs fixed-α StreamingOptimizer (detection latency, false alarms) |
| Step sizes as learned feature relevance | node-local diagnostic; `meas` summaries already serve the agent side |
| AutoStep decayed-max normalization | already in our optimizer (leaky max v); cite as robustness evidence for the fixed-config design |
| Linear-TD theory | Degris 2024 (next anchor row) extends step-size optimization to continual learning |

## Open questions

- [ ] Ablate per-feature step sizes vs fixed α = 2e-3 on the host bench: detection latency + false alarms (RQ1 optional-ablation row).
- [ ] Do the 4 input-layer step sizes track sensor relevance in practice (a free node-local diagnostic beside the `meas` summaries)?
- [ ] Does our static eps = 1.0 floor already give the no-overshoot guarantee AutoStep buys with the effective step size? Paper-level comparison first.
- [ ] Symbol hygiene for the writeup: the paper's θ (meta step size) vs our detection θ; never use θ for both.
