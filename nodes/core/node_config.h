// =====================================================================
// node_config.h -- compile-time knobs for the GENERAL node core.
// Sensor-agnostic: every value here is expressed in core concepts
// (steps, features, scores) - never in units of a specific sensor.
// Per-sensor timing (e.g. Modbus poll period) lives in the sensor package.
// =====================================================================
#pragma once
#include <stdint.h>

namespace cfg {

// ---- feature vector ceiling (RAM is sized for this at compile time) ----
constexpr int      MAX_FEATURES  = 8;

// ---- stream cadence ----
constexpr uint32_t FEED_PERIOD_MS  = 1000;   // TinyRL learning step (1 Hz)
constexpr uint32_t PRINT_PERIOD_MS = 2000;   // telemetry console line
constexpr uint32_t FRESH_WINDOW_MS = 5000;   // data older than this = stale

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
// condition space already clamps (frozen 0..1 ranges); the monitor's
// frozen z needs the same guard. 5.0 keeps all in-fit noise (~+-4)
// and 2-4 sigma fault signals untouched.
constexpr float    Z_CLAMP        = 5.0f;

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
// in log2-ratio units (rel(), feature_space.h), with a PER-FEATURE radius:
// q * unit(f), unit(f) = log2(1 + CAL sigma/mu). One global radius cannot
// serve both nodes: vib bands jitter +-40% while the drive's fault
// excursions are +-30% - the radius must scale with each feature's own
// calibrated noise. Condition positions are log2(x/mu) decades, clamped.
constexpr float    REL_CLAMP      = 6.0f;   // rel() saturation (decades)
constexpr float    QUIET_Q        = 3.0f;   // alarm-quiet + episode pre-name
constexpr float    MATCH_Q        = 4.0f;   // tripwire + recognised-normal display
// Settles 2012 ch. 2.5 myopia tripwire: log line every N quiet-unmatched steps
constexpr uint32_t QUM_REPORT_EVERY = 1000;

// ---- persistence ----
constexpr uint32_t AUTOSAVE_MS    = 300000; // flash wear: <=12 writes/hour
constexpr uint32_t MODEL_MAGIC    = 0x54524C34u; // "TRL4"
constexpr uint32_t MODEL_VERSION  = 5;     // v5: condition space anchors (mu)
constexpr uint32_t NET_MAGIC      = 0x54524C4Eu; // "TRLN"
constexpr uint32_t NET_VERSION    = 1;

constexpr const char* MODEL_FILE  = "/model.bin";
constexpr const char* NET_FILE    = "/net.bin";
constexpr const char* COND_FILE   = "/conditions.jsonl";

constexpr const char* SCHEMA_VER  = "uns-v1.1";
constexpr const char* FW_VERSION  = "2.0.0-modular";

} // namespace cfg
