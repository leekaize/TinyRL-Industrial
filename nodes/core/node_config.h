// =====================================================================
// node_config.h -- compile-time knobs for the GENERAL node core.
// Sensor-agnostic: every value here is expressed in core concepts
// (steps, features, scores) - never in units of a specific sensor.
// Per-sensor timing (e.g. Modbus poll period) lives in the sensor package.
// =====================================================================
#pragma once
#include <stdint.h>

// ---- paper ablation switches (leave-one-out table) ------------------
// Default: every switch 1 = the shipped detector, unchanged. One
// mechanism family is ablated per build with -DABL_<NAME>=0 through
// TINYRL_BENCH_FLAGS (tests/bench/app.py build()); families pair
// mechanisms that share a dependency, so each row removes one coherent
// capability:
//   ABL_SETTLE_GATE      the motion gate; the regime-entry optimizer
//                        reset and the re-entry grace arm on ITS edge,
//                        so both go quiet with it
//   ABL_PERCOND_BAND     per-condition score edges for matched AND
//                        unknown states + the fresh-label/re-entry
//                        grace those bands define
//   ABL_FLOOR_REANCHOR   the sustained-floor re-anchor + the score
//                        arm's floor-move yield (rides ABL_PERCOND_BAND)
//   ABL_POS_EVIDENCE     the positional channel: ragg/gate_r, the pos
//                        fire arm, the excursion re-anchor and the
//                        identity-adaptation guard, which all read it
//   ABL_ACCUMULATOR      the leaky scale-free accumulator AND the
//                        standing proofs; fallback = the pre-accumulator
//                        m-of-N Schmitt (ALARM_HI_N consecutive breaches)
//   ABL_STICKY_HOLD      radius-edge stickiness + tenure release; run
//   ABL_YARDSTICK_GROWTH with ABL_STICKY_HOLD=0 as the drift-recovery
//                        family (quiet-unmatched UNITS growth)
//   ABL_UNKNOWN_PERSIST  charge unknown evidence only from the 2nd
//                        consecutive settled step; 0 = charge from the 1st
#ifndef ABL_SETTLE_GATE
#define ABL_SETTLE_GATE 1
#endif
#ifndef ABL_PERCOND_BAND
#define ABL_PERCOND_BAND 1
#endif
#ifndef ABL_FLOOR_REANCHOR
#define ABL_FLOOR_REANCHOR 1
#endif
#ifndef ABL_POS_EVIDENCE
#define ABL_POS_EVIDENCE 1
#endif
#ifndef ABL_ACCUMULATOR
#define ABL_ACCUMULATOR 1
#endif
#ifndef ABL_STICKY_HOLD
#define ABL_STICKY_HOLD 1
#endif
#ifndef ABL_YARDSTICK_GROWTH
#define ABL_YARDSTICK_GROWTH 1
#endif
#ifndef ABL_UNKNOWN_PERSIST
#define ABL_UNKNOWN_PERSIST 1
#endif

namespace cfg {

// ---- feature vector ceiling (RAM is sized for this at compile time) ----
constexpr int      MAX_FEATURES  = 8;

// ---- stream cadence ----
constexpr uint32_t FEED_PERIOD_MS  = 1000;   // TinyRL learning step (1 Hz)
constexpr uint32_t PRINT_PERIOD_MS = 2000;   // telemetry console line
constexpr uint32_t FRESH_WINDOW_MS = 5000;   // data older than this = stale

// ---- node liveness beat (State/keepalive, non-retained, QoS 0) ----
// The dashboard flips a node to OFFLINE once no beat arrived for
// KEEPALIVE_MAX_AGE_MS (3 missed beats + slack). NOT the MQTT protocol
// keepalive (setKeepAlive below) - this is an application-level signal
// an UNS subscriber can watch without a broker session view.
constexpr uint32_t KEEPALIVE_MS         = 10000;  // beat cadence
constexpr uint32_t KEEPALIVE_MAX_AGE_MS = 30000;  // offline threshold (dashboards)

// ---- learning schedule (units = FEED_PERIOD steps) ----
// 1-minute total LEARN (user decision 2026-11-13), justified by the
// motor-faithful fixture (test_host.cpp): a fixed-load induction motor
// is noise-stationary at 1 Hz with slow thermal drift only, so 30
// samples characterise the noise band. If the node ever watches a load
// with its own multi-minute cycle, LEARN must span one full cycle
// (probes 2026-11-13: on the old 4-min-sine fixture, 60- and 120-step
// LEARNs false-alarmed; 240 steps passed).
constexpr uint32_t CAL_STEPS       = 30;     // ~30 s: sensor range learning
constexpr uint32_t FIT_STEPS       = 30;     // ~30 s: score stats -> theta

// ---- score = EMA of TinyRL one-step prediction error ----
constexpr float    SCORE_EMA      = 0.15f;
constexpr float    THETA_KSIG     = 4.0f;   // theta = mu + KSIG * sigma
constexpr float    SCORE_FLOOR    = 1e-4f;
constexpr int      ALARM_HI_N     = 5;      // consecutive breaches -> alarm
constexpr int      CLEAR_LO_N     = 60;     // consecutive quiet    -> clear
// Frozen-z clamp (found via the CI timetable, 2026-11-14): after fit, a
// regime far outside the fit window (machine STOPPED while theta was fit
// at running) produces z-scores in the hundreds. Unbounded, they blast
// the net weights and leave the steady surprise baseline above theta -
// the alarm latches and the node goes deaf to every later fault. The
// condition space is anchored-ratio for the same reason (feature_space.h);
// the monitor's frozen z needs the same guard. 5.0 keeps all in-fit noise
// (~+-4) and 2-4 sigma fault signals untouched.
constexpr float    Z_CLAMP        = 5.0f;

// ---- settle gate (user requirement 2026-11-15: transitions never train,
// never alarm, never ask - the node decides only on settled states) ----
// The criterion is NET DISPLACEMENT in the condition space (log2-ratio
// decades, feature_space.h rel()): settled when, for EVERY feature, the
// rel position moved less than SETTLE_NET_DRIFT decades over the last
// SETTLE_WINDOW_N steps (ring buffer). Rationale (2026-11-22, real vib
// replay): a running machine is never instantaneously still - the
// speed_1 block's band power wobbles +-0.5-1.0 decades per step, so the
// old "SETTLE_CALM_N consecutive fast<=MARGIN*max(slow,floor)" test
// never fired on real rows and the node could not learn the state it sat
// in (dead-lock: cannot settle without learning, cannot learn without
// settling; the g100 passed only because its registers are steady to
// 0.1%). Net displacement separates exactly what matters: a state's own
// wander CANCELS over the window, a machine ramp ACCUMULATES (2-3
// decades/step, same direction), and a knock transient RETURNS within a
// step. A single-feature fault onset still moves the net (transition),
// and slow thermal drift (0.03 decades/step) stays settled.
constexpr int      SETTLE_WINDOW_N    = 8;      // net-drift window (steps)
constexpr float    SETTLE_NET_DRIFT   = 2.0f;   // max net rel drift (decades)

// Re-entry grace (2026-11-21, real-data replay finding): a regime entry to
// a KNOWN condition whose score band already matured has no fresh-label
// grace, but the same transient applies - the net's one-step error spikes
// while it re-adapts (optimizer scale state just dropped), and the matured
// band gate sits at the CONVERGED level, so the accumulator reaches
// ACC_ALARM within ~5 steps of every known-state revisit (the tour's
// stopped revisit: score 3.54 / band gate 0.45 = 7.9x, alarm on rows that
// are the condition's own recording). For REENTRY_GRACE_N steps after a
// regime entry the matched state's gate is inactive and the accumulator
// flushes 2x/step, like the fresh-label grace; a fault that appears during
// the window still alarms once it persists past it (bounded latency, the
// same trade the label grace already makes). Cond era. The Z era consumes
// the same window by holding its score streak (2026-11-22: the net-drift
// settle gate exposed the bare 5-step streak on churn revisits - the
// post-entry surprise decay needs the grace to finish first).
constexpr int      REENTRY_GRACE_N    = 16;
// Sustained-floor recalibration (cond era, 2026-11-22 real-data tour): a
// band that matured from the post-label DECAY tail (or from a block's
// first rows) can sit BELOW the state's true surprise floor once the
// inside the same recording block (the collect campaign's thermal
// warm-up lifts the g100 speed_2 floor ~60% above its matured gate within
// one 150-row block; a regime-entry reset lifts it ~5x over a band that
// matured on near-zero scores). The ratchet never widens the band from
// inside, so the fix fires on the state's own witness: 2 consecutive
// settled MATCHED steps above the state's own gate WITH QUIET GEOMETRY
// (r_agg under 3x its positional band - a fault drives 4-8x and crosses
// ACC_ALARM in ~4 steps) restart the score band, re-arm the grace and
// VOID the stale evidence (the accumulator's content was judged against
// the band that just got replaced). The 2-step trigger beats the
// accumulator to ACC_ALARM (a mild excess needs 5+ steps; a hard one ties
// at 2 and the recalibration runs first in the step order). The score
// excess size is deliberately NOT gated: a 5x excess over a near-zero
// matured band is the same floor move as a 1.2x excess over a converged
// one - the geometry, not the ratio, separates fault from drift.
constexpr int      FLOOR_EXCESS_N     = 2;
// Geometry-jump guard for the re-anchor (cond era, 2026-11-22 suite
// finding): the re-anchor's quiet-geometry test cannot use a ragg-ratio
// bound - the suite's in-radius fault sits at only ~2.5x its positional
// band (2.5 sigma per feature in sane batch units) while the real
// thermal drift reads ~47x (its walk is measured in degenerate batch
// units), so magnitude inverts between the two cases. The stable
// discriminator is the RATE: a fault is a step change (+21 ragg in one
// step on the suite fixture), a floor drift creeps (~+2/step over 150
// rows). A settled matched step whose ragg jumps more than
// RAGG_JUMP_MAX over one step resets the re-anchor counter - the
// geometry moved like a fault, so the accumulator alarms unhindered.
constexpr float    RAGG_JUMP_MAX      = 5.0f;
// Positional-band re-anchor (cond era, 2026-11-22b stage-8 finding): the
// collect campaign's blocks carry an internal thermal warm-up - the same
// labeled block's tail rows sit up to ~30x the band a fresh label forms on
// its cool head rows (speed_1 gate_r 0.079, tail r_agg 1.7-2.4). The
// outlier-frozen band re-alarms its OWN rows every block cycle, and the
// accumulator latch (capped at ACC_CAP, leaking ~60 steps to ACC_CLEAR)
// cannot clear between cycles: the stage ends phase=alarm on rows that are
// the condition's own recording. Magnitude cannot separate this from the
// suite's in-radius fault (which also reads 5-30x its band); DURATION can:
// a fault is STATIC (it re-charges the accumulator every step until named),
// while the block's own wander SUBSIDES - r_agg falls back inside the gate
// while the state is still matched and settled. So: an excursion that
// breaches the gate for POS_EX_MIN_N+ settled matched steps and then
// subsides for POS_EX_SUBSIDE_N consecutive matched steps re-anchors the
// band MATURE at the excursion's peak (r_floor, reseedPosBand) and voids
// the stale evidence - the state's own wander is inside the gate from then
// on. A fault never subsides, so it never re-anchors and alarms unhindered
// (the 2026-11-22 stage-7 contract). A fault-like ARRIVAL (ragg jump over
// RAGG_JUMP_MAX in one step) disarms the tracker instead: the excursion
// may have been a real fault that was later repaired, and repaired-fault
// geometry must not loosen the band. This replaces the reverted grow-only
// widening (POS_BAND_GROW_ALPHA, removed with this change): growing DURING
// the excess absorbed the suite's fault; growing only AFTER a subside
// proof cannot - the fault has no subside.
constexpr int      POS_EX_MIN_N       = 2;      // breach steps before a
                                                  // subside can re-anchor
constexpr int      POS_EX_SUBSIDE_N   = 4;      // consecutive inside matched
                                                  // steps that prove the fold
// Pos-arm FIRE gate (2026-11-22, net-drift settle gate): the breach must
// STAND for this many consecutive settled steps, not just at the firing
// step. The collect blocks' warm-up tail rows breach for 1-3 rows and
// subside when the block loops; the old settle gate flapped unsettled on
// them (suppressing the charge) and the net-drift gate removed that
// accident - without this gate the pos arm alarm-edged every churn visit.
// A real fault stands indefinitely, so the latency cost is bounded
// (+POS_FIRE_STAND_N-1 steps vs the old one-step fire).
// 12 (2026-11-22f evening): the per-loop chunk re-permutation of the
// replay (the fix for the visibly cyclic figure) gives the one-step
// predictor a NEW adjacency to mispredict every loop - the seam transient
// spikes r_agg 6-18x the band for 4-10 rows, then subsides (the net
// re-adapts). Each burst also trips the score-band recalibration, which
// arms the re-entry grace, and the pos arm then fires on the same
// transient during the grace (run-201742: 3 vfd FAs, 2.1x/8.5x/18.3x,
// all self-clearing). 4-10 rows is a seam transient; a real fault has no
// subside - 12 standing steps covers every observed transient with
// margin and delays a genuine fault by ~8 steps.
constexpr int      POS_FIRE_STAND_N   = 12;
// Score-arm FIRE gate (2026-11-22, same finding as POS_FIRE_STAND_N): the
// 1-second machine ramp between two states passes THROUGH intermediate
// geometries (speed_2 -> stop decelerates through speed_1's operating
// point; the fset clamp makes the ramp row match speed_1) and its never-
// seen shape spikes the net's surprise ~20x the band - the old one-step
// fire alarm-edged the transition (run-173133, vfd FA at the stop ramps;
// the old paper run predated the ramps default). A real fault's excess
// stands; a transition transient's does not.
constexpr int      SCORE_FIRE_STAND_N = 4;
// Identity-adaptation block (2026-11-22d, the bench phase_loss finding):
// adaptCentroid/adaptUnits learn from matched steps - and an in-radius
// fault IS matched (the phase_loss rows sat d=2.72 inside speed_2's
// radius), so the centroid walked onto the fault rows (ragg 18.9 -> 0.01
// across one grace window) and the second fault window became invisible.
// The block rides the same convention the floor-excess guard already
// uses: geometry under 3x the state's own positional band is the state's
// own wander (thermal drift tracks inside the radius, yardstick lag
// ~0.5 units) and still adapts; past it the geometry says foreign - a
// fault drives 4-8x, the phase_loss rows 45-105x - and the identity
// must not learn it. A subsided excursion re-anchors the band first
// (reseedPosBand), which drops ragg/gate_r back under the block and
// reopens adaptation on the widened, honest band.
constexpr float    POS_ADAPT_BLOCK    = 3.0f;   // x the positional gate
constexpr float    POS_RESEED_SD_FRAC = 0.25f;  // the re-anchored band's sd
                                                  // as a fraction of its
                                                  // peak (gate = peak x
                                                  // 1 + PERCOND_KSIG x this)
// Per-condition unit floor (cond era, 2026-11-22 real-data tour): a
// 15-row batch of a near-constant register measures a spread orders of
// magnitude finer than the register's true wander (the collect campaign's
// pf: batch sd 0.0001 decades, block wander 0.046 - the condition's pf
// radius rejected its own state's tail rows and they re-alarmed as
// "unknown"). The batch spread stands, but never below a fraction of
// the CAL sigma - the CAL window is the machine's observed operating
// resolution. z/rel keep 0.0 (byte-identical arms).
#if defined(MONITOR_COND_OBS)
constexpr float    UNIT_FLOOR_FRAC    = 0.5f;
#else
constexpr float    UNIT_FLOOR_FRAC    = 0.0f;
#endif
// Sticky-hold release (cond era, 2026-11-22 real-data tour): the
// radius-edge hold (MATCH_Q on yardstick-grown units) can span the gap to
// a NEIGHBOURING state - the speed_2 first visit sat d~3.5 inside the
// grown 'speed_1' radius and never alarmed, never asked, never learned.
// The held condition's own band is the witness: STICKY_RELEASE_N
// consecutive settled held steps above its own gate mean the rows are not
// this state - release the hold and burn it until a genuine match
// re-acquires, so the unknown path alarms and asks. The threshold must
// outlast the longest recorded transition ramp held at an excess (the
// speed_1->speed_2 ramp: 58 calm rows) but stay well inside a state
// visit (~150 rows), so transition traverses stay quiet while a foreign
// state visit releases.
constexpr int      STICKY_RELEASE_N   = 80;
// Yardstick ceiling (cond era, 2026-11-22 real-data tour): the yardstick
// exists to cover the state's OWN wander (thermal drift, between-visit
// instrument wander - a few CAL sigmas). Uncapped, it also grows toward
// whatever rows merely pass nearby: the neighborhood path (quiet-
// unmatched steps within YARDSTICK_FAR) bridged the 16-unit gap to the
// unlearned speed_2 state - the grown radius re-matched the foreign rows,
// the centroid followed, and the two conditions merged. Cap the unit at
// a multiple of the CAL sigma: the state's wander never plausibly
// exceeds it, a setpoint step is a new state by the fset dimension (its
// CAL unit ~0 keeps the step-detector), and a foreign state's distance
// stays outside the match radius.
constexpr float    YARDSTICK_U_CAP    = 2.0f;

// ---- ask policy (user decision 2026-11-19: never ask on a raw onset) ----
// An alarm on a state matching no known condition opens an episode, but
// the node ASKS only once that episode holds ASK_MIN_BATCH SETTLED
// samples; before that it publishes condition_id "unidentified" (alarmed,
// batch maturing, no upstream call). A label is built from the stored
// batch - asking earlier stores mostly transition noise. While maturing
// the episode keeps re-identifying, so a state that resolves into a known
// condition is relabelled and never asks at all.
constexpr int      ASK_MIN_BATCH    = 15;

// ---- TinyRL agent (network + streaming optimizer) ----
// Default optimizer: StreamingOptimizer (Stream-X v3, nodes/core/
// streaming_optimizer.h) -- per-coordinate bound |move| < lr.
// Ablation fallback: build with -DMONITOR_USE_OBGD for the v2 ObGD rule
// (global trace-norm scaling; kappa applies).
constexpr int      NET_HIDDEN     = 32;     // hidden width (2-layer MLP)
constexpr float    NET_SPARSITY   = 0.95f;  // global sparse-init budget (StreamX v3); per-layer density is budget-allocated with a connectivity floor
constexpr float    MONITOR_LR     = 2e-3f;  // step size (paper-scale, Stream-X prescribed range)
constexpr float    OBGD_GAMMA     = 0.9f;   // trace decay (both optimizers)
constexpr float    OBGD_LAMBDA    = 0.8f;   // trace backview (both optimizers)
constexpr float    STREAM_BETA    = 0.99995f; // leaky-max retention (paper-prescribed)
constexpr float    STREAM_EPS     = 1.0f;   // unit-signal floor (z-scored signals)
constexpr float    OBGD_LR        = 0.5f;   // (ObGD ablation path only)
constexpr float    OBGD_KAPPA     = 2.0f;   // (ObGD ablation path only) overshooting bound gain
constexpr uint32_t SEED           = 20260101u; // 0 = seed from TRNG at boot

// ---- condition library ----
constexpr int      LIB_MAX        = 12;     // learned conditions per node
// Matching is MAX-COMPONENT (bench finding 2026-11-14: the mean over 4-6
// features lets one discriminative feature be outvoted by the flat ones),
// in anchored log2-ratio units (rel(), feature_space.h), with a PER-
// CONDITION radius: q * u(f), the condition's own measured spread
// (2026-11-15: CAL-only units were 20 decades on the drive's dead
// features - blind to every running state - and 0.15 decades on the
// vib's bands - narrower than their own noise). u=0 falls back to the
// CAL unit. Positions are rel decades, clamped wide for float sanity.
constexpr float    REL_CLAMP      = 12.0f;  // rel() sanity bound (decades)
constexpr float    QUIET_Q        = 3.0f;   // alarm-quiet + episode pre-name
constexpr float    MATCH_Q        = 4.0f;   // tripwire + recognised-normal display
// Yardstick adaptation (band eras, 2026-11-21 rig replay, handoff 7.3):
// batch-derived u(f) come from ONE 15-sample window and underestimate the
// between-visit wander (the meter's i channel walks ~0.04 decades with
// temperature) - the tight radius flickered the match out and the
// unknown-state judgment re-alarmed a KNOWN state. On quiet matched steps
// each unit grows toward the condition's own observed steady-state spread
// (alpha per step, grow-only). A fault cannot inflate it: units only adapt
// on steps matched INSIDE the current radius, and the fault's offset
// exceeds that radius in one step.
constexpr float    YARDSTICK_ALPHA = 0.05f;  // per quiet matched step
// How far outside the radius a NORMAL neighbor's yardstick/centroid may
// learn from (units): between-visit drift parks a known state here for
// whole minutes while its score band is still forming. Faults sit far
// beyond (the phase_loss onset is ~90 units from its nearest normal
// neighbor) and FAULT-kind neighbors never adapt from outside their radius
// at all; a fault's centroid never moves at all (identity = the label).
constexpr float    YARDSTICK_FAR   = 24.0f;
// Settles 2012 ch. 2.5 myopia tripwire: log line every N quiet-unmatched steps
constexpr uint32_t QUM_REPORT_EVERY = 1000;

// Settled identity check (2026-11-15): an open episode whose state moved
// is re-identified only after this many CONSECUTIVE settled confirmations
// (single noise vetoes must not churn episodes and batches)
constexpr int      IDENT_DRIFT_N   = 5;

// ---- per-condition score calibration (2026-11-19) ----
// The global theta is fit once at commissioning and goes stale as soon as
// the net trains on other regimes (bench: vib quiet score ~3 vs theta
// ~0.3; the vfd ~9-16 vs 1.4). Every condition therefore learns its OWN
// alarm threshold mu + k sigma from the surprise scores observed on its
// settled matched steps, and quiet is judged against THAT band. Until a
// condition has PERCOND_MIN_N samples its threshold is the global theta.
constexpr int      PERCOND_MIN_N   = 16;     // samples before the band rules
constexpr float    PERCOND_KSIG    = 3.0f;    // theta_c = mu + k sigma

// ---- training policy (user decision 2026-11-15) ----
// The net learns every LABELED condition (normal AND fault) while the
// state is SETTLED; unknown regimes never train, so a novel condition
// keeps alarming until the operator names it. The rate is
// novelty-modulated: full MONITOR_LR while surprise is high against its
// own slow baseline (new regime), a small floor at the baseline
// (stability), never zero (a very slow drift keeps creeping the baseline
// and stays trackable; drift beyond the match radius reads as unknown).
constexpr float    LR_MIN_FRAC     = 0.1f;    // floor fraction of MONITOR_LR
constexpr float    NOVELTY_K       = 4.0f;    // frac = (surp-base)/(K*base)
constexpr float    NOVELTY_EMA     = 0.01f;   // slow surprise baseline EMA

// ---- persistence ----
constexpr uint32_t AUTOSAVE_MS    = 300000; // flash wear: <=12 writes/hour
constexpr uint32_t MODEL_MAGIC    = 0x54524C34u; // "TRL4"
// MONITOR_REL_OBS (build flag, paper A/B, 2026-11-20): the TinyRL agent's
// observation is the anchored-ratio rel coordinate (feature_space.h)
// instead of frozen z. Legacy z pins every running regime at the +-5
// clamp - one input point, so the one-step prediction error (the Stream-TD
// surprise, the paper's detector) cannot discriminate regimes or faults.
// rel-obs restores per-regime stationarity; matched states alarm against
// their OWN matured surprise band (PERCOND) instead of the positional
// latch; unknown states keep the global fit theta. Select via env
// vfd_g100_node_rel in platformio.ini.
//
// MONITOR_COND_OBS (build flag, 2026-11-21): PER-CONDITION CENTERED
// observation. The agent sees residuals in the matched condition's own
// units, obs = (rel - c_matched) / u_matched (nearest condition's frame
// when nothing matches, CAL-unit normalized rel before the first
// condition exists). rel LEVELS still interpolate across regimes: a fault
// sitting between two trained regimes is predicted straight through
// (stage-H in-radius miss, 2026-11-21). Residual space removes the trap:
// every condition's training manifold sits at 0, a 2.5-sigma offset is
// off-manifold no matter which regimes were learned, and in a static
// regime the conditional-mean predictor regresses to 0, so ANY static
// offset scores its squared distance from the regime's own centre.
// Gate/threshold/rate/band logic is shared with rel-obs (the band-gated
// eras); only the observation differs. Both flags are A/B arms against
// the legacy z env. The theta/score SCALE differs between eras, so
// persisted models are era-locked (a model.bin from another env is
// rejected -> fresh commissioning):
#if defined(MONITOR_REL_OBS)
constexpr uint32_t MODEL_VERSION  = 7;     // rel-obs era (per-cond streaming TD)
#elif defined(MONITOR_COND_OBS)
constexpr uint32_t MODEL_VERSION  = 8;     // per-cond centered-obs era
#else
constexpr uint32_t MODEL_VERSION  = 6;     // v6: anchored condition space (z era)
#endif
#if defined(MONITOR_REL_OBS) || defined(MONITOR_COND_OBS)
// Band-gated eras (rel + cond): minimum learning-rate fraction while a
// matched state's score is ABOVE its gate (first learning pass,
// interference heal, real excursion). 1.0 burst-thrashed (the leaky-max
// keeps burst capacity for ~beta^-1 steps, so the score self-oscillated
// around the band - stage H 2026-11-20); 0.5 damped the landing but
// stretched an interference heal past 4700 steps (still alarming at the
// fixture window). 0.7 both damps and heals within the stage-H window.
// Bench-tuned on the rel build 2026-11-21; the cond build re-validates it
// in stage H (shared rate rule).
constexpr float    RELEARN_FRAC   = 0.7f;

// ---- evidence accumulator (band eras, 2026-11-21) ----
// Replaces the m-of-N consecutive-breach Schmitt for alarm decisions.
// Each settled step contributes its EXCESS past the active alarm edge,
// normalized by that edge's own gate (score/gate, ragg/gate_r - scale-
// free across conditions). The accumulator LEAKS ACC_DECAY per step and
// a quiet step never resets it (user requirement 2026-11-21: normal
// evidence does not wipe accumulated badness). One marginal breach
// cannot fire, weak breaches accumulate, one strong one almost suffices.
// Sized so the in-radius fault (evidence ~1.3/step) fires in ~4 settled
// steps while marginal drift breaches (evidence ~0.05/step, the stale-
// band re-entry false alarm the 5-consecutive rule produced, stage H
// 2026-11-21) integrate sub-threshold. The level is the paper's alarm-
// approach/confidence trace (1 - acc/ACC_ALARM).
constexpr float    ACC_DECAY       = 0.05f;  // per-step leak fraction
constexpr float    ACC_ALARM       = 5.0f;   // trigger level
constexpr float    ACC_CLEAR       = 1.0f;   // hysteresis floor (clear)
constexpr float    ACC_CAP         = 20.0f;  // bounds leak-back time + trace scale
#endif
#ifdef MONITOR_COND_OBS
// Sanity bound for centered-residual inputs, same philosophy as rel()'s
// REL_CLAMP: matched states are bounded by construction (within ~4 units
// of the centroid or the net never trains), but far unknowns can read
// tens of units - clamping keeps the score scale finite without hiding
// anything (an unknown alarms at >= 3 units already).
constexpr float    RESID_CLAMP    = 12.0f;
#endif
constexpr uint32_t NET_MAGIC      = 0x54524C4Eu; // "TRLN"
constexpr uint32_t NET_VERSION    = 1;

constexpr const char* MODEL_FILE  = "/model.bin";
constexpr const char* NET_FILE    = "/net.bin";
constexpr const char* COND_FILE   = "/conditions.jsonl";

// pending batches of settled unknown states (user workflow 2026-11-15:
// store in flash, label or discard BY ID, labels may arrive late)
constexpr int      PENDING_MAX     = 8;
constexpr const char* PENDING_FILE = "/pending.jsonl";

constexpr const char* SCHEMA_VER  = "uns-v1.1";
constexpr const char* FW_VERSION  = "2.0.0-modular";

} // namespace cfg
