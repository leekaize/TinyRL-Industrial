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
  void factoryReset();
  void applyFeedback(const char* json);

  ISensorNode& sensor() { return *sensor_; }

  // read-only state views (host tools + dashboards; no core state exposed)
  float       score() const;
  float       theta() const;
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

private:
  void onEvent(const char* ev, const char* sev, const char* cid,
               uint32_t dur, const char* summary, const float* means);
  // settled identity check: has the state moved beyond the open
  // episode's stored batch spread on some feature?
  bool batchMoved(const float* raw) const;
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
  bool     baseline_registered_ = false;   // CAL baseline -> 'op_stopped'
  int      ep_pend_ = -1;   // pending index of the OPEN episode's batch
  int      ident_drift_ = 0;  // consecutive settled steps the identity looked stale
  char     last_cond_key_[48];
  uint32_t last_model_save_ = 0;
  uint32_t quiet_unmatched_ = 0;   // Settles myopia tripwire (volatile, local log only)
  const char* dev_id_ = "";
};
