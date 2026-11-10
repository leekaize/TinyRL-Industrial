# Streaming Deep Reinforcement Learning Finally Works (Stream-X)

| | |
|---|---|
| Source | arXiv:2410.14606 (v3, 2026-09-21) · code: [github.com/mohmdelsayed/streaming-drl](https://github.com/mohmdelsayed/streaming-drl) · related: [github.com/mohmdelsayed/tinyrl](https://github.com/mohmdelsayed/tinyrl) |
| Authors | Elsayed, Lupu, Vasan, Mahmood (U. Alberta / Amii) · 2024, expanded v3 2026 |
| Venue | arXiv preprint |
| Last viewed | 2026-11-03 (arXiv full text) |
| Pillar | 1-machine-learning |
| Informs | What must adapt for streaming learning: running-moment normalisation, representation stabilisation, bounded updates; workstation scale + ESP32-S3 demo |
| Status | ✅ snapshot 2026-11-03 |

## Key concepts

- **Streaming learning**: **learn from each transition as it arrives**; no buffer, no batches.
- **Stream barrier**: DQN/PPO/SAC at buffer size 1, and deep Q-learning/actor-critic, sit near random across 58 Atari + 50 control tasks even after per-task tuning.
- **Stream-X methods for streaming DRL**:
  1. Running-moment input normalisation + reward scaled by variance of the discounted reward trace (rescale inputs and learning signal by running average)
  2. LayerNorm (re-standardizes numbers inside the net) + layer-aware 95% sparse init (start with 95% of connections cut, small layers keep more)
  3. StreamingOptimizer — per-coordinate leaky max `v = max(βv, |u|)`, every displacement < α. (each knob capped: never moves more than α per sample, leaky max = remembers recent biggest nudge, slowly forgets)
- **Note for v2→v3**: ObGD (global trace-norm scaling; updates shrink ~1/d as nets grow) replaced by StreamingOptimizer. (The net would learn slower and slower as it grows — that’s why we ported)

## Methods

- Family: Stream-Q/SARSA/AC/RAC + Stream-TD (prediction). One fixed config per algorithm across all tasks (α ≈ 1e-3, λ = 0.8, γ = 0.99, β = 0.99995, 30 runs).
- Nonstationary Ant: friction alternates 2.0 ↔ 0.02 every 2M steps; Stream-AC recovers every time, SAC collapses, PPO stagnates (buffer sweep 1 → 1M doesn't fix SAC).
- Open Ant robot: heading tracking ~10 hours; frozen DINO features + proprioception.
- ESP32-S3 (512 KB SRAM): Stream-Q learns pixel Pong on-device via TinyRL (4,539 params, ~70 KB state; 45% → 97% win rate over 500k steps).
- Ablations: leave-one-out, 10 tasks; sensitivity is task-dependent. (Removing any one fix hurts some task, another task needs a different one)

## Snapshot

Deep RL learns from replay buffers and batch updates; the paper shows that learning directly from the stream fails. DQN/PPO/SAC with one-transition buffers and deep versions of Q-learning and actor-critic aggregate near random over 58 Atari and 50 control tasks despite per-task tuning; called the "stream barrier". Performance returns only as buffer size grows, isolating storage, not the algorithms, as the stabiliser.

Stream-X adds three enhancements to streaming bases: running-moment input normalisation with reward scaled by the variance of a discounted reward trace; LayerNorm plus layer-aware 95% sparse initialisation; and StreamingOptimizer, which bounds every parameter's displacement by α (v3 replaces ObGD, whose global scaling shrinks updates ~1/d in larger networks).

With one fixed configuration per algorithm, Stream-X nears batch RL: Stream-Q median 0.99 vs 0.15 (deep Q-learning) and <0.001 (DQN1) on Atari; Stream-AC 0.73 vs 0.04 (SAC1) on control. Stream-AC repeatedly recovers across alternating-friction regimes and learns heading tracking on a real robot over ~10 h; Stream-Q learns pixel Pong on an ESP32-S3.

Limitations (authors'): model-free, MLP/CNN, mostly on-policy; leave-one-out ablations don't prove each ingredient necessary; recovery mechanism unattributed.

## Schema implications (TinyRL-Industrial)

- Our node runs Stream-TD mechanics on next-feature prediction; Stream-Q/AC control variants are unwired.
- Confirms the node-local split (RQ1): all three ingredients are internal, one fixed config across 108 tasks → schema carries only site-relative knobs (θ, labels, phase).
- CAL phase = normalisation warm-up → proposed `State/norm_ready` gate.
- If prediction error is internally standardised, θ may be site-agnostic → ablation: one default θ across loads (RQ1).
- RQ2 anchors: ~70 KB learning state vs ~1 GB buffer; small buffers don't rescue batch RL (untested for low-dim telemetry → host-test experiment).
- Our board (~8 MB PSRAM) makes sample storage a choice: reserve for agent-requested data windows + baselines; learning stays buffer-free.
- RQ3 gap: single-agent paper — warm-starting a joining node's moments/labels from siblings is ours to test.

## Proposed future work linked to our research

| Theirs | Ours |
|---|---|
| Continuing (non-episodic) setting | Monitoring has no episodes — our task; pairs with the De Asis row |
| Incrementally learned models | Our detector already is an online one-step model |
| RTRL / partial observability | Cheap version now: stacked telemetry history |

## Open questions

- [x] Design v2 to v3 algorithm port on TinyRL — **last commit 2026-11-03**: `nodes/core/streaming_optimizer.h` (default; ObGD behind `-DMONITOR_USE_OBGD`) + layer-aware sparse init in `tinyrl_monitor.cpp`. Two documented adaptations: ε = 1.0 as unit-signal floor (paper's "small ε" is unspecified; without it tiny gradients take ~α-sized steps), and a connectivity floor on the sparse budget (paper's Alg. 8 alone still allows all-zero low-fan-in layers — our first layer was exactly that case). Host-test green: α=2e-3, β=0.99995, λ=0.8.
- [ ] Stream-TD config on the real VFD task: α = 2e-3, β = 0.99995, λ = 0.8 sim-validated; field validation (detection latency / false alarms vs ObGD baseline) still open.
- [ ] PSRAM ring-buffer sizing for agent-requested windows.
- [ ] Small-buffer batch RL on low-dim telemetry (host-test; doubles as RQ2 baseline).
