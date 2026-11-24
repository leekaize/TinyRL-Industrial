// =====================================================================
// node_runtime.h -- SensorNodeApp: the composition root of EVERY node.
//
// Wires ENVIRONMENT (one ISensorNode) to the GENERAL core:
//
//   console ─┐                         ┌─ TinyRLConditionMonitor (TinyRL)
//            ├─ SensorNodeApp ──────────┼─ ConditionLibrary
//   MQTT  ───┘        │                 ├─ EpisodeManager
//        UnsMessaging │                 └─ persist (LittleFS v4 + net.bin)
//                     └─ ISensorNode (sensor package, swapped per product)
//
// setup()/loop() of the sketch reduce to app.begin(sensor, identity) and
// app.tick(). All vfd_streamq MVP semantics (episode lifecycle, feedback
// actions, condition-change publishing, autosave policy) live here.
// =====================================================================
#pragma once
#include <stdint.h>
#include "sensor_interface.h"
#include "tinyrl_monitor.h"
#include "condition_library.h"
#include "pending_store.h"
#include "episode.h"
#include "uns_messaging.h"
#include "console.h"
#include "node_config.h"

class SensorNodeApp {
public:
  void begin(ISensorNode& sensor, const NodeIdentity& id);
  void tick();                          // call from loop(), non-blocking

  // console + MQTT feedback entry points
  void startLearning();
  void sensorPollNow();
  void listLibrary();
  void listPending();
  void printStatus();
  void toggleVerbose();
  void toggleTrace();
  void toggleRawStream();   // passthrough: the sensor owns the stream
  void factoryReset();
  void applyFeedback(const char* json);

  ISensorNode& sensor() { return *sensor_; }

  // read-only state views (host tools + dashboards; no core state exposed)
  float       score() const;
  float       theta() const;
  // per-cond era evidence from the LAST monitor step (bench figures):
  // the ACTIVE alarm gate this step (per-condition band or global theta,
  // -1 = grace/latch), the positional aggregate vs its band, and the
  // evidence-accumulator level (see tinyrl_monitor.h StepOut)
  float       gate() const;
  float       ragg() const;
  float       gateR() const;
  float       acc() const;
  const char* phase() const;       // idle | cal | fit | mon | alarm
  const char* conditionId() const;
  bool        episodeOpen() const;
  uint32_t    stepCount() const;
  uint32_t    libSize() const;
  const char* libName(int idx) const;

  // host tools + dashboards: pending batches (ids + sample count + step)
  // and library centroids for the report figures (read-only views)
  uint32_t    pendingSize() const;
  bool        pendingAt(int i, char* id, size_t cap, uint32_t* n,
                        uint32_t* step) const;
  bool        libCentroid(int idx, float* out, CondKind* kind) const;
  float       libUnit(int idx, int f) const;   // condition's own match unit
  float       libTheta(int idx) const;   // condition's own score threshold
                                            // (per-condition band, 2026-11-19)
  float       fsAnchor(int f) const;     // condition-space anchor (rel plots)

private:
  void onEvent(const char* ev, const char* sev, const char* cid,
               uint32_t dur, const char* summary, const float* means);
  // settled identity check: has the state moved beyond the open
  // episode's stored batch spread on some feature?
  bool batchMoved(const float* raw) const;
  // store the open episode's batch + publish the ask, once it matured
  void askIfMature(uint32_t now);
  void publishCondition(bool retained);
  void saveAll();

  ISensorNode*           sensor_ = nullptr;
  TinyRLConditionMonitor mon_;
  ConditionLibrary       lib_;
  PendingStore           pend_;
  EpisodeManager         ep_;
  UnsMessaging           msg_;
  Console                con_;

  float    last_raw_[cfg::MAX_FEATURES];
  bool     have_raw_ = false;
  // evidence from the last monitor step (bench figures; see gate()/acc())
  float    last_gate_ = -1, last_ragg_ = 0, last_gate_r_ = -1, last_acc_ = 0;
  int      last_posref_ = -1;   // which condition's band gate_r belongs to
  bool     baseline_registered_ = false;   // CAL baseline -> 'op_stopped'
  int      ep_pend_ = -1;   // pending index of the OPEN episode's batch
  bool     ep_ask_dismissed_ = false;  // operator discarded this episode's
                                      // ask: do not re-ask the SAME
                                      // occurrence, only the next one
  int      ident_drift_ = 0;  // consecutive settled steps the identity looked stale
  char     last_cond_key_[48];
  uint32_t last_model_save_ = 0;
  uint32_t quiet_unmatched_ = 0;   // Settles myopia tripwire (volatile, local log only)
  bool     sensor_verbose_ = false;   // bus-level TX/reply trace ('v' toggles)
  bool     trace_ = false;            // per-step model decision trace ('d')
  const char* dev_id_ = "";
};
