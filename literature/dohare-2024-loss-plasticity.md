# Maintaining Plasticity in Deep Continual Learning (Nature title: Loss of plasticity in deep continual learning)

| | |
|---|---|
| Source | arXiv:2306.13812 (v3, 2024-04-09) · Nature 632, 768-774, DOI 10.1038/s41586-024-07711-7. Note: repo rows previously recorded arXiv:2310.02204, which is an unrelated weighted-automata paper; corrected 2026-11-12 via arXiv API + Crossref |
| Authors | Shibhansh Dohare, J. Fernando Hernandez-Garcia, Parash Rahman, A. Rupam Mahmood, Richard S. Sutton (Univ. of Alberta / Amii; Nature list adds Qingfeng Lan) · 2024 |
| Venue | Nature 632 (issue 8026, published 2024-08-21); arXiv v3 titled "Maintaining Plasticity in Deep Continual Learning" |
| Last viewed | 2026-11-12 (arXiv HTML full text v3; Crossref for Nature record) |
| Pillar | 1-learning |
| Informs | Catastrophic forgetting / plasticity canon; continual backprop; retention-vs-relearning framing for our flash condition library |
| Status | ✅ snapshot 2026-11-12 |

## Key concepts

- **Plasticity**: the ability to keep learning from new data. Distinct from **stability** (keeping old knowledge; its failure = catastrophic forgetting). Their claim: plasticity loss is the more fundamental continual-learning failure, and it is not the same problem as forgetting.
- **Loss of plasticity**: standard deep networks trained through long task sequences degrade until they learn no better than a linear (or shallow) network. It happens silently: performance first rises, then decays.
- **Three correlates**: as plasticity is lost, the fraction of **dead units** grows (up to 25%), average **weight magnitude** grows, and the **effective rank** of the representation falls. All three move with the accuracy decline.
- **Continual backpropagation**: SGD plus selective reinitialization. Every example, reinitialize the lowest-utility fraction ρ of mature hidden units; **utility** = mean-corrected contribution (|activation − running mean| × Σ|outgoing weights|) × adaptation (1/Σ|incoming weights|). New units ship zero outgoing weights (function unchanged) and are protected for m updates (**maturity threshold**).
- **What helps / hurts**: L2 regularization partially mitigates; shrink-and-perturb nearly eliminates the loss but is hyperparameter sensitive. Adam (catastrophic drop), dropout, and online normalization make it worse. Guiding principle: "good continual learning algorithms should do similar computations at all times"; continual randomness injection from the initial distribution is the mechanism.

## Methods

- **Continual ImageNet**: 2000 consecutive binary tasks from 1000 classes (32×32 downsampled), 600/100 train/test per task, 250 epochs, mini-batch 100, SGD + momentum, ReLU CNN (3 conv + 3 fc); 30 runs. Accuracy 89% on early tasks → 77% by task 2000, ≈ linear baseline.
- **Online Permuted MNIST**: 800 permutations, one pass of 60k examples per task, no batches; network sizes 100 to 10,000 units/layer; task-switch rates 10k to 1M examples (48M total). Performance rises then decays at every setting.
- **Slowly-Changing Regression**: cheap testbed, 6 activation functions (sigmoid, tanh, ELU, leaky-ReLU, ReLU, Swish), all lose plasticity; 100 runs.
- **Continual backpropagation** defaults: replacement rate ρ = 1e-4, decay η = 0.99, maturity m = 100 (m = 1000 with Adam); compared against L2, shrink-and-perturb, online norm, Adam, dropout. Code: github.com/shibhansh/loss-of-plasticity.
- **Slippery Ant** (preliminary RL): PyBullet Ant, ground friction resampled log-uniformly from [1e-4, 1e4] every 10M steps, 100M total; PPO degrades severely, Continual PPO / PPO+shrink-and-perturb / PPO+L2 hold up, Continual PPO best; 100 runs.

## Snapshot

Dohare et al. separate two continual-learning failures: catastrophic forgetting, losing what was learned, and loss of plasticity, losing the ability to learn. Prior evidence was indirect (pretraining studies, RL-specific confounds); this paper gives the direct demonstration. On Continual ImageNet, standard practice (SGD with momentum, mini-batches, ReLU CNN) reaches 89% accuracy on early tasks and falls to 77%, the level of a linear network, by task 2000 of a binary-classification sequence. Online Permuted MNIST reproduces the pattern: performance rises for the first tasks, then decays steadily at every network size from 100 to 10,000 units per layer and every task-switch rate up to one permutation per million examples. Six activation functions all fail on the Slowly-Changing Regression testbed.

The mechanism study ties plasticity loss to three concurrent degradations: dead units accumulate (up to 25%), average weight magnitude grows, and the representation's effective rank falls. Among standard fixes, L2 regularization helps partially; shrink-and-perturb nearly eliminates the loss but only in a narrow hyperparameter range; Adam, dropout, and online normalization make it worse. The proposed continual backpropagation runs SGD and, on every example, reinitializes the lowest-utility fraction (replacement rate 1e-4) of mature hidden units, where utility combines mean-corrected contribution to consumers with adaptation speed. New units leave the function unchanged (zero outgoing weights) and are protected for a maturity period. It fully maintains plasticity on all supervised problems (5000-task ImageNet included) and performs best on the Slippery Ant PPO experiment.

Limitations: large-scale supervised evidence plus one preliminary RL experiment; the utility measure is heuristic; the effective alternatives are fragile to hyperparameters.

## Schema implications (TinyRL-Industrial)

- **Retention vs relearning split (RQ1)**: the flash condition library is retention; the net is relearning capacity. Verified: `persistence.cpp` saves `/model.bin` v4 (stats + θ), `/net.bin` (weights), `/conditions.jsonl` (centroids + labels, `condition_library.h` nearest-centroid + JSONL load/save); `importState` (`tinyrl_monitor.cpp`) restores frozen ranges + frozen normalizer for exact post-boot resume, zero relearning. Condition identity + labels are schema-exposed; weights and centroids stay node-local. Labels never depend on net plasticity: centroids live in the frozen 0..1 log-range space, refined by EMA 0.9/0.1 on episode close.
- **Plasticity loss is our silent failure mode (RQ2/RQ3)**: a node that trains forever at 1 Hz keeps publishing plausible scores while new conditions stop being learnable. Dead-unit fraction, mean |w|, and effective rank are node-local internals; candidate host-test logging row (their 48M-step Permuted-MNIST horizon is reachable on a host bench).
- **Our port already carries three anti-loss ingredients, code-verified**: LayerNorm (Stream-X), layer-aware sparse init with a connectivity floor (boot-time diversity is exactly the property Dohare says decays), and `streaming_optimizer.h` per-coordinate bounded moves (|move| < lr) plus the regime-change reset that zeroes traces and vmax together.
- **Adam warning supports the parameter split (RQ1)**: no adaptive-moment optimizer node-side; fixed α = 2e-3 with bounded updates. Dohare's Adam result (effective rank collapses, plasticity loss is catastrophic) is the strongest published argument for keeping moment-based adaptivity off a non-stationary stream.
- **Continual backprop = node-local ablation candidate only (RQ2 tension check)**: utility state is O(units), not O(samples), so the buffer-free claim survives; but it adds per-unit running averages, ages, and maturity counters on top of ~16 KB optimizer state for 1,348 params. Adopt only if the long-run bench shows drift.

## Proposed future work linked to our research

| Theirs | Ours |
|---|---|
| Plasticity-loss audit (dead units, weight magnitude, effective rank) | add the three counters to long-run host-test logs (queued, RQ2) |
| Continual backpropagation (selective reinit) | node-local ablation candidate; adopt only on observed drift |
| Shrink-and-perturb / L2 as cheap mitigations | bounded updates already cap weight growth; bench comparison only |
| Slippery Ant regime changes | our regime-change reset (traces + vmax zeroed) is the same trigger, untested at scale |

## Open questions

- [ ] Does the 1,348-param Stream-TD net stay plastic at 48M steps on the host bench? Log dead-unit count, mean |w|, effective rank (RQ2).
- [ ] Is LayerNorm + bounded updates + sparse init sufficient at our scale, or does effective rank still collapse without selective reinit?
- [ ] Retention test: after a long stretch of new conditions, do old condition centroids still match the old machines (frozen centroid space protects labels; the open part is whether old scores remain separable from the drifted net)? (RQ3, tension with the buffer-free claim)
- [ ] Can θ governance mask slow plasticity loss (detector looks fine while adaptivity dies)? Needs the plasticity counters, not the score (RQ1/RQ2).
