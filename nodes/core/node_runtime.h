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
  void        episodeMeans(float* out) const;   // raw units, open episode

  // multi-node label sharing: another node's episode means (raw) mapped
  // through THIS node's own frozen feature space
  int         shareLabel(const char* slug, const float* mean_raw);
  int         libMatch(const float* raw) const;
  const char* libName(int idx) const;

private:
  void onEvent(const char* ev, const char* sev, const char* cid,
               uint32_t dur, const char* summary, const float* means);
  void publishCondition(bool retained);
  void saveAll();

  ISensorNode*           sensor_ = nullptr;
  TinyRLConditionMonitor mon_;
  ConditionLibrary       lib_;
  EpisodeManager         ep_;
  UnsMessaging           msg_;
  Console                con_;

  float    last_raw_[cfg::MAX_FEATURES];
  bool     have_raw_ = false;
  char     last_cond_key_[48];
  uint32_t last_model_save_ = 0;
  uint32_t quiet_unmatched_ = 0;   // Settles myopia tripwire (volatile, local log only)
  const char* dev_id_ = "";
};
