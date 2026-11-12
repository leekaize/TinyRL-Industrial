# Extending Differential Temporal Difference Methods for Episodic Problems

| | |
|---|---|
| Source | arXiv:2605.04368 (v1, 2026-05-06) |
| Authors | Kris De Asis, Mohamed Elsayed, Jiamin He (Univ. of Alberta) · 2026 |
| Venue | RLC 2026 |
| Last viewed | 2026-11-12 (arXiv HTML full text v1) |
| Pillar | 1-learning |
| Informs | Reward centering: reward-scale adaptivity, made safe for episodic problems |
| Status | ✅ snapshot 2026-11-12 |

## Key concepts

- **Differential TD / reward centering**: subtract a running estimate b of the average reward from every reward. Keeps undiscounted returns bounded and removes the large state-independent offset r(π)/(1−γ) in the value function's Laurent-series decomposition.
- **The episodic trap**: constant reward shifts change the optimal policy when episodes end. Subtract a big constant → terminate as fast as possible; add one → prolong the episode. This is why centering was limited to continuing problems.
- **Terminal differential value (their fix)**: view centering as potential-based reward shaping with Φ(s) = b/(1−γ). The potential must vanish at terminals, so define VΔ(S_T) = −b/(1−γ) for γ < 1, or VΔ(S_T) = −b in the bias-unit form, which also covers undiscounted (γ = 1) episodic problems. Optimal-policy ordering is preserved; differential TD extends to episodic problems.
- **Bias-unit equivalence**: the centered value is V(s;w,b) = VΔ(s;w) + b, one shared output-level bias (not per-output like DQN heads). Reparameterized, the update is exactly linear TD with one extra feature, inheriting its convergence guarantees; proved for γ < 1 and γ = 1 episodic via an "unrolled MDP".
- **When it helps**: where values deviate most across states (dense "painful" rewards, large return magnitudes). Tuned, it never does worse than the base algorithm, since η = 0 recovers it exactly.

## Methods

- **Episodic 10×10 gridworld**: differential Q-learning (Alg. 1, terminal differential value) vs vanilla Q-learning; γ = 0.9, ε-greedy 0.1, 100 runs. Painful rewards (−1/step): large improvement. Sparse (0/step, +1 at goal): no difference.
- **MinAtar** (Asterix, Breakout, Freeway, Seaquest, SpaceInvaders): differential Stream Q(0.8) vs Stream Q(0.8) vs PopArt Stream Q(0.8); improvement in 4 of 5 games, similar on Breakout; PopArt inconsistent; 30 runs each.
- **MuJoCo**: differential Stream AC(0.8) vs Stream AC(0.8) vs PopArt; considerable gains in Ant-v4 and HalfCheetah-v4 (the two largest return magnitudes), no worse elsewhere; PopArt not significant; 30 runs.
- **Painful Reacher** (DMC, −1/step, easy/hard goal sizes): significant improvement over uncentered.
- Hyperparameters: η swept 1e-2 to 1e3 in streaming experiments; base Stream Q/AC settings from Elsayed et al. 2024 (α = 1, λ = 0.8, γ = 0.99).

## Snapshot

De Asis et al. remove the last restriction on reward centering. Differential TD methods subtract a running estimate of the average reward from every reward, which keeps undiscounted returns bounded and cancels the dominant state-independent term in the value function. The mechanism was confined to continuing problems because in episodic problems a constant reward shift changes which policy is optimal: shifting rewards down makes early termination optimal, shifting up rewards stalling. Their fix comes from reading centering as potential-based reward shaping with potential Φ(s) = b/(1−γ). Since a shaping potential must vanish at terminal states, they define a terminal differential value, VΔ(S_T) = −b/(1−γ) when discounting and VΔ(S_T) = −b in the equivalent bias-unit form, and prove the transformation preserves optimal-policy ordering. This extends differential TD to discounted and undiscounted episodic problems.

A second contribution makes the theory clean: the centered value is exactly a value function plus one shared, state- and action-independent bias unit at the output. Reparameterized, the update is linear TD with one extra feature, so the known convergence guarantees carry over; the appendix proves convergence for both γ < 1 and undiscounted episodic settings via an unrolled MDP.

Empirically, in the streaming deep RL setting (no buffers, no target networks), differential Stream Q(λ) improves on 4 of 5 MinAtar games and differential Stream AC(λ) improves substantially on Ant and HalfCheetah, matching the prediction that centering pays where values deviate most across states. PopArt, the nearest alternative, is inconsistent by comparison. Tuned, the differential variants never lose to their bases because η = 0 recovers them exactly.

Limitations: one more hyperparameter (η, which the authors flag, suggesting meta-learning it later); benefit is unreliable on sparse-reward problems; theory assumes ergodicity of the unrolled chain.

## Schema implications (TinyRL-Industrial)

- **Parameter split (RQ1)**: reward centering is node-local, exactly as pre-registered in the RESEARCH.md row ("Reward scale / centering | De Asis | node-local, if reward enters the loop"). The centering scalar b is site-independent (it estimates the node's own reward stream), so there is nothing to expose.
- **No direct hook today (RQ1)**: the monitor learns a one-step dynamics model (Stream-TD), not a value function; "reward = −surprise" only describes the update signal. The hook activates only if the control variants (Stream-Q/AC, kept in the submodule for future actuation) are wired.
- **The episodic caveat is load-bearing for us (RQ1)**: machine life is episodic (normal → fault → repair → normal; alarm episodes open and close). If values ever enter the node, plain centering would bias toward short episodes; the paper's terminal differential value is the version to port, with our episode close as the terminal event.
- **When-to-expect-benefit rule**: gains concentrate where value deviation across states is largest (their painful-vs-sparse contrast). Our dense, negative surprise stream is the "painful" case, so centering would be the first candidate if a value head is ever added.
- **Cost**: one scalar b plus one meta-parameter η on top of the ~16 KB optimizer state; η = 0 makes it a no-op, so it is a safe ablation axis.

## Proposed future work linked to our research

| Theirs | Ours |
|---|---|
| Differential Stream Q/AC (episodic) | the parked Stream-Q/AC variants: if actuation is added, port the episodic differential form, never plain centering |
| Terminal differential value VΔ(S_T) = −b | our episode lifecycle (open/close) is the natural termination signal |
| Painful-vs-sparse rule for when centering pays | our −surprise reward is the painful case; expect gains if a value head lands |
| η meta-learning (their future work) | node-local only; η = 0 fallback keeps the base algorithm |

## Open questions

- [ ] If Stream-Q/AC enter the node for actuation, does episodic differential TD replace or stack with the reward-scaling ingredient (`ScaleReward` in the Stream-X port)? (RQ1)
- [ ] Is our alarm-episode lifecycle equivalent to their terminal state (no learning on close), i.e., is the −b terminal value well-defined when episodes are opened by the detector rather than by the environment?
- [ ] Their b interpretation in episodic problems (expected value over non-terminal states, subdivided by expected remaining episode length) reads like a free node-local health signal; worth checking if a value head ever lands.
