# Enhancements over the base TinyRL core

What `nodes/core/` adds to the upstream TinyRL submodule
(`TinyRL/src`: autograd, matrix, layers, OBGD optimizer, streaming
updates) to make it a deployable industrial condition monitor. Each row
names the mechanism, the industrial failure it prevents, and the test
that proves it. The suite stages are `tests/run_tests.sh`; "suite log"
references are `tests/logs/host_tests_<ts>.log`.

Evaluation state, all configurations green (2026-11-22, 8/8):
ramps OFF `host_tests_20261122_130728.log`, ramps ON
`host_tests_20261122_130904.log`, shipped default
`host_tests_20261122_131114.log`.

Ablation: each detection-chain row is removable at build time via the
`ABL_*` switches in node_config.h (families listed in
tests/README.md); the leave-one-out tour results are the paper's
Table A-III (paper-evidence.md ablation section).

## A. Per-step detection chain (1 Hz, inside the monitor)

| # | Enhancement | Mechanism | Prevents | Proof |
|---|---|---|---|---|
| A1 | Settle gate v2 | Per-feature motion EMAs floored at CAL units (`SETTLE_CALM_N=8`, node_config.h:76); no train/alarm/ask mid-move; settled-only feeding keeps an ask batch = ONE state | Decisions and training on mixed-distribution data during machine moves; the poisoned-centroid bug of 2026-11-15 | Suite stage 2 (SIMCHECK); the 2026-11-15 bench finding (RESEARCH.md) |
| A2 | Era-gated observation space | Deployed cond-obs: `obs = (rel - c_matched)/u_matched`, nearest-condition frame for unknowns. z (legacy) and rel (paper A/B) arms kept behind build flags | A global observation frame that cannot see in-radius faults | Era ablation, stage 2/6/7 verdicts: z MISSES, rel MISSES, cond CATCHES the in-radius fault |
| A3 | Per-condition score bands | Each condition calibrates `gate = mu+3sd` of its own prediction error after 16 samples (`PERCOND_MIN_N`, node_config.h:278); the global fit theta only judges unknowns | A state whose converged error sits above/below the global theta | Stage 8: band maturity prints per condition; re-tour recognition with 0 asks |
| A4 | Positional evidence channel | `pos_acc_` charges `(ragg/gate_r - 1)` against the matched condition's own positional band; grace-INDEPENDENT; fires while the breach stands (tinyrl_monitor.cpp, 2026-11-22f) | Static geometry breaches masked by grace (the VFD phase_loss miss: rows sat 45x the band while acc read 0.00) | Bench paper_tour run-20261122-123134: phase_loss owner 12/0 steps; stage 7 in-radius catch |
| A5 | Leaky evidence accumulator | Both evidence terms integrate scale-free (`ACC_ALARM=5`, cap 20, leak `ACC_DECAY`); quiet steps decay, never reset | One marginal breach firing; weak breaches never accumulating | Stage 2 SIMCHECK alarm/label path; stage 4 tripwire |
| A6 | Sticky hold + tenure release | Radius-edge drift stays matched to the last condition while within MATCH_Q; a hold cannot outlive `STICKY_RELEASE_N=80` settled steps | Slow wander re-judging a known state against the fit theta; a grown radius carrying foreign rows forever | Stage 8 tour across block switches; the 2026-11-21 hardware finding |
| A7 | Re-entry grace + band recalibration | `REENTRY_GRACE_N=16` on regime entry; a matured band whose geometry is still its own recalibrates on re-entry (the net's floor moved after the regime-entry reset) | Re-alarming a known state on re-entry; a stale band that can never re-form | Stage 8 churn + re-tour: 0 asks |
| A8 | Floor-excess re-anchor + floor-move yield | 2 sustained above-gate matched steps with quiet geometry re-anchor the score band (`FLOOR_EXCESS_N=2`); at the fire check the score arm yields while `ragg < 3x gate_r` (floor_move_case, tinyrl_monitor.cpp:1124) - single-step rag excursions included | A machine's prediction-floor drift alarm-edging every visit to a known state (the 2026-11-22d churn race: 16 alarms/cycle) | Ramped churn, 20 cycles 0 alarms 0 asks (suite 131114, stage 8); 17 re-anchors landed in the tour |
| A9 | Positional-excursion re-anchor | An excursion breaching the band for `POS_EX_MIN_N=2+` steps that then SUBSIDES for `POS_EX_SUBSIDE_N=4` while matched re-anchors the positional band MATURE at the peak | The collect block's internal warm-up re-alarming its own rows every cycle | Stage 8: 5 positional re-anchors, each subsiding; suite 091038 log analysis |
| A10 | Identity adaptation guard | Centroid/yardstick adaptation blocked while `ragg > POS_ADAPT_BLOCK (3x) * gate_r`; below 3x the honest wander still adapts | An in-radius fault walking the matched condition's centroid onto itself (speed_2 became blind to window 2 of the same fault) | Stage 7 contract (CATCHES); run-110857 analysis |
| A11 | Yardstick neighborhood growth | Quiet-unmatched steps near a NORMAL condition within `YARDSTICK_FAR=24` grow its UNITS only, never the centroid; not while alarmed | Between-visit instrument drift parking a known state outside its tight batch radius for minutes | Stage 8 re-tour recognition; the 2026-11-21 rig replay finding |
| A12 | Unknown-persist gate | Evidence charges only from the 2nd consecutive settled unknown step | A single transition row (the 1-second ramp midpoint) charging the nearest band ~100x in one step | Ramped suite (the run-120222 mid-tour FA class eliminated) |
| A13 | Z-era legacy latch | The 2026-11-19 positional Schmitt latch (in QUIET_Q 3, out MATCH_Q 4) kept for the ablation arm | Losing the documented baseline that the era ablation contrasts against | Stage 5 (z-era TOURCHECK, synthetic fixture by design) |

## B. Learning, retention, and the ask loop

| # | Enhancement | Mechanism | Prevents | Proof |
|---|---|---|---|---|
| B1 | Condition library | Flash-resident JSONL per node: slug, kind (normal/fault), centroid c, yardstick u, score band, positional band (`LIB_MAX=12`); labels are fleet vocabulary, centroids per node | Retention depending on weight plasticity; re-asking named states | Stage 8 full-vocabulary assert; re-tour 0 asks |
| B2 | Ask loop (episode + pending store) | Alarm opens an episode; settled samples feed it; at `ASK_MIN_BATCH=15` a batch is stored (survives reboot) and the ask publishes; label/discard by id | Asking before evidence; losing asked questions on reboot | Stage 5/8: asks mature at >= 15 (assert); pending store round-trip |
| B3 | One-shot labels, no re-asks | A label names every open occurrence; recognition on revisit is 0-ask by construction; the sticky release bounds a hold's life | Repeated asks for the same state (operator noise) | Bench run-20261122-123134: re-tour asks 0, redundant 0 |
| B4 | False-alarm feedback | `onFeedbackNormal` desensitizes, `onFeedbackFault` sharpens; pos_acc_ voided at the label (the answered ask closes its own case) | Post-label alarm flapping from a stale reservoir (run-112814 flaps) | Stage 2; run-112814 regression fixed in 22f |
| B5 | Mixed-batch protections | A label rescaling the input frame drops optimizer traces; relabeled episodes never refine centroids; fault episodes never refine | A label poisoning the predictor or dragging a centroid across states | Frame-switch [WHY] prints; the relabelled refine guard (episode.cpp) |
| B6 | Streaming plasticity discipline | Bounded OBGD updates at the floor rate once a condition's band says converged; the Dohare correlates logged (`[PLAST]`) | Loss of plasticity / dead units over 100k steps | Plasticity audit: dead 0.00, mean abs w +128%, eff rank 4.51 -> 1.88 (100k bench, 2026-11-20) |
| B7 | Persistence | model.bin + net.bin + conditions.jsonl + pending.jsonl on LittleFS; autosave <= 5 min (flash-wear budget); versioned blobs | Relearning after every reboot; flash wear | Stage 3: save/reload round-trip asserted; stage 2 persistence |
| B8 | Determinism | Fixed `SEED=12345`; assert-based suite; every number regenerates from `tests/` | Undocumented seed dependence | Suite green on demand; the 2026-11-20 seed resolution |

## C. The industrial wrapper (schema, fixture, observability)

| # | Enhancement | Mechanism | Prevents | Proof |
|---|---|---|---|---|
| C1 | UNS two-way schema | 4 MQTT topics, UNS-pattern paths, publish on state/condition change only; labels flow down as Action/feedback | Raw telemetry flooding upstream; oracle-locked integrations | RQ2: 113 semantic msgs vs 4,800 raw vectors = 42x fewer (2,400-step tour) |
| C2 | Real-data fixture | The MODBUS collect campaign (150 rows/condition, labeled-only loading) is the default world for suite + bench + figures | Evaluating only on synthetic data | REALMODE line, suite stage 8 (131114) |
| C3 | 1-second ramp synthesis | One synthesized midpoint row per ordered block pair (linear in raw feature space, confirmed tail -> confirmed head); DEFAULT ON; `TINYRL_RAMP_SYNTH=0` ablates | Evaluating on unphysical instant switches; unconfirmed operator rows entering the model (labeled-only loader) | Suite green 8/8 in all three fixture configurations |
| C4 | Explainability telemetry | `[WHY]` names the evidence chain per decision (which term charged, which band judged, which grace applies); `[RAW]` carries score/gate/rag/gate_r/acc/posref; prints carry the node id | Undiagnosable field behavior; two-node log ambiguity (2026-11-22d first task) | The 2026-11-22f/g churn traces were read from these lines |
| C5 | Operator-facing tripwire | Quiet-unmatched counter (`qum=`, report every 1000) watching the Settles-myopia blind region | A gated learner staying confidently blind to whole regions | Stage 4 TRIPCHECK |
| C6 | Per-modality feature identity | Feature order/units = model identity; the vib locked to 4 features (1,348 params both nodes); META ships true names/units to the bench | Figure/axis mismatches; param creep | Stage 8 META; tools/feature_selection.py lock |
| C7 | MCU deployment discipline | PlatformIO envs per era; static footprints measured per env (17-18% RAM / 31-32% flash); 5 us/step host proxy | Shipping a regression that breaks the MCU budget | run_mcu.sh builds; Table III footprints |

## What the base core provides (unchanged)

The Stream-X recipe (running-moment normalisation, LayerNorm + sparse
init, bounded per-parameter updates), the OBGD optimizer, the sequential
net, ESP32 kernel optimizations: used as-is from the submodule. The
paper's claim is the wrapper, not a modification of the learner.
