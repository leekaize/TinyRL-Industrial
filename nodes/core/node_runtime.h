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

private:
  void onEvent(const char* ev, const char* sev, const char* cid,
               uint32_t dur, const char* summary, const float* means);
  void publishCondition(bool retained);
  void saveAll();
  const char* conditionId() const;

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
  const char* dev_id_ = "";
};
