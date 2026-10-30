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
constexpr uint32_t CAL_STEPS       = 300;    // ~5 min: sensor range learning
constexpr uint32_t FIT_STEPS       = 120;    // ~2 min: score stats -> theta

// ---- score = EMA of TinyRL one-step prediction error ----
constexpr float    SCORE_EMA      = 0.15f;
constexpr float    THETA_KSIG     = 4.0f;   // theta = mu + KSIG * sigma
constexpr float    SCORE_FLOOR    = 1e-4f;
constexpr int      ALARM_HI_N     = 5;      // consecutive breaches -> alarm
constexpr int      CLEAR_LO_N     = 60;     // consecutive quiet    -> clear

// ---- TinyRL agent (network + ObGD optimizer) ----
constexpr int      NET_HIDDEN     = 32;     // hidden width (2-layer MLP)
constexpr float    NET_SPARSITY   = 0.9f;   // 90% sparse init (StreamX recipe)
constexpr float    OBGD_LR        = 0.5f;
constexpr float    OBGD_GAMMA     = 0.9f;   // trace decay
constexpr float    OBGD_LAMBDA    = 0.8f;   // trace backview
constexpr float    OBGD_KAPPA     = 2.0f;   // overshooting bound gain
constexpr uint32_t SEED           = 20260101u; // 0 = seed from TRNG at boot

// ---- condition library ----
constexpr int      LIB_MAX        = 12;     // learned conditions per node
constexpr float    LIB_MATCH_D    = 0.30f;  // nearest-centroid threshold (mean dist)

// ---- persistence ----
constexpr uint32_t AUTOSAVE_MS    = 300000; // flash wear: <=12 writes/hour
constexpr uint32_t MODEL_MAGIC    = 0x54524C34u; // "TRL4"
constexpr uint32_t MODEL_VERSION  = 4;
constexpr uint32_t NET_MAGIC      = 0x54524C4Eu; // "TRLN"
constexpr uint32_t NET_VERSION    = 1;

constexpr const char* MODEL_FILE  = "/model.bin";
constexpr const char* NET_FILE    = "/net.bin";
constexpr const char* COND_FILE   = "/conditions.jsonl";

constexpr const char* SCHEMA_VER  = "uns-v1.1";
constexpr const char* FW_VERSION  = "2.0.0-modular";

} // namespace cfg
