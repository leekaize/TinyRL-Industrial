// =====================================================================
// node_runtime.cpp -- orchestration preserved from the vfd_streamq MVP,
// generalized over the FeatureProfile (nothing here knows about VFDs).
// =====================================================================
#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include "node_runtime.h"
#include "persistence.h"
#include "json_mini.h"

// ---------------- helpers --------------------------------------------
static void slugify(const char* in, char* out, size_t len) {
  size_t o = 0;
  for (size_t i = 0; in[i] && o < len - 1; i++) {
    char c = in[i];
    if (isalnum((int)c)) out[o++] = (char)tolower((int)c);
    else if (o == 0 || out[o - 1] != '_') out[o++] = '_';
  }
  while (o > 0 && out[o - 1] == '_') o--;
  out[o] = 0;
}

const char* SensorNodeApp::conditionId() const {
  if (!mon_.thetaReady()) return mon_.learning() ? "learning" : "idle";
  if (!mon_.alarmed()) return "normal";
  return ep_.isOpen() ? ep_.cond() : "unknown";
}

void SensorNodeApp::publishCondition(bool retained) {
  uint32_t now = millis();
  msg_.publishCondition(mon_.stateString(), conditionId(),
                        mon_.confidence(), ep_.duration(now), retained);
}

void SensorNodeApp::saveAll() {
  persist::modelSave(mon_);
  persist::netSave(mon_);
  last_model_save_ = millis();
}

void SensorNodeApp::onEvent(const char* ev, const char* sev, const char* cid,
                            uint32_t dur, const char* summary,
                            const float* means) {
  msg_.publishEvent(ev, sev, cid, dur, summary, means);
}

// ---------------- lifecycle ------------------------------------------
void SensorNodeApp::begin(ISensorNode& sensor, const NodeIdentity& id) {
  sensor_ = &sensor;
  dev_id_ = id.dev_id;
  last_cond_key_[0] = 0;
  last_model_save_ = millis();

  sensor.begin();

  persist::beginFS();
  mon_.begin(sensor.profile());

  bool restored = persist::modelLoad(mon_);
  bool net_ok = false;
  if (restored && mon_.thetaReady()) {
    net_ok = persist::netLoad(mon_);
    if (!net_ok) {                       // theta without weights = broken pair
      Serial.println("[FS] theta found but network missing/corrupt - relearning");
      mon_.factoryReset();
      restored = false;
    }
  }

  lib_.begin(sensor.profile().n_features);
  lib_.load(cfg::COND_FILE);

  const FeatureProfile& prof = sensor.profile();
  ep_.begin(&prof, &lib_, dev_id_,
            [this](const char* ev, const char* sev, const char* cid,
                   uint32_t dur, const char* sum, const float* means) {
              onEvent(ev, sev, cid, dur, sum, means);
            });
  msg_.begin(id, prof, [this](const char* j) { applyFeedback(j); });
  con_.begin(this);

  // boot report (v3 semantics)
  if (restored && mon_.thetaReady()) {
    Serial.printf("[FS] TinyRL model+net restored (theta=%.2f) - monitoring, no relearn\n",
                  mon_.theta());
  } else if (restored && mon_.learning()) {
    Serial.println("[FS] learning was in progress - CAL restarts automatically");
  } else {
    Serial.println("[FS] fresh model - IDLE. Press 's' (or MQTT {\"action\":\"start\"}) "
                   "to start learning at NORMAL load");
  }
  Serial.printf("[FS] %d known condition(s)\n", lib_.size());
}

void SensorNodeApp::startLearning() {
  if (!mon_.thetaReady()) {
    mon_.startLearning();
    saveAll();
    publishCondition(true);
  } else {
    mon_.startLearning();                // prints the "already monitoring" hint
  }
}

// ---------------- feedback (Action/feedback + console 'f') -----------
void SensorNodeApp::applyFeedback(const char* j) {
  char action[16] = "", label[32] = "", by[20] = "";
  jsonStr(j, "action", action, sizeof(action));
  jsonStr(j, "label",  label,  sizeof(label));
  jsonStr(j, "by",     by,     sizeof(by));
  Serial.printf("[FB] action='%s' label='%s' by='%s'\n", action, label, by);

  if (strcmp(action, "start") == 0) { startLearning(); return; }

  if (strcmp(action, "label") == 0 && label[0]) {
    if (!ep_.isOpen()) { Serial.println("[FB] no open episode to label"); return; }
    char slug[24];
    slugify(label, slug, sizeof(slug));
    float mean[cfg::MAX_FEATURES];
    ep_.means(mean);
    int idx = lib_.label(slug, mean, mon_.space(), by);
    if (idx < 0) {
      Serial.println("[FB] condition library full (LIB_MAX)");
      return;
    }
    ep_.relabel(slug, millis());
    mon_.onWorldChange();                // regime changed -> drop traces
    lib_.save(cfg::COND_FILE);
    saveAll();
    publishCondition(true);
    return;
  }

  if (strcmp(action, "normal") == 0) {   // false alarm -> desensitise
    mon_.onFeedbackNormal();
    ep_.close("normal", mon_.space(), millis());
    saveAll();
    publishCondition(true);
    Serial.printf("[FB] theta raised to %.2f\n", mon_.theta());
    return;
  }

  if (strcmp(action, "fault") == 0) {    // confirmed -> sharpen
    mon_.onFeedbackFault();
    saveAll();
    publishCondition(true);
    Serial.printf("[FB] theta lowered to %.2f\n", mon_.theta());
    return;
  }
  // unknown action: ignored
}

// ---------------- console / maintenance commands ---------------------
void SensorNodeApp::sensorPollNow() {
  sensor_->poll();
  char line[160];
  sensor_->logLine(line, sizeof(line));
  Serial.println(line);
}

void SensorNodeApp::listLibrary() { lib_.list(); }

void SensorNodeApp::printStatus() {
  char line[160];
  sensor_->logLine(line, sizeof(line));
  Serial.printf("[NODE] %s | state=%s cond=%s conf=%.2f\n",
                line, mon_.stateString(), conditionId(), mon_.confidence());
  Serial.printf("[RL] step=%lu score=%.3f theta=%.3f params=%u lib=%d pubs=%lu heap=%u\n",
                (unsigned long)mon_.steps(), mon_.score(), mon_.theta(),
                (unsigned)mon_.paramCount(), lib_.size(),
                (unsigned long)msg_.publishes(), (unsigned)ESP.getFreeHeap());
}

void SensorNodeApp::factoryReset() {
  mon_.factoryReset();
  lib_.clear();
  persist::factoryReset();
  last_cond_key_[0] = 0;
  Serial.println("[FS] back to IDLE - press 's' to start learning again");
}

// ---------------- main tick ------------------------------------------
void SensorNodeApp::tick() {
  con_.tick();
  msg_.maintain();

  uint32_t now = millis();
  static uint32_t t_next_poll = 0, t_next_print = 0, t_next_feed = 0;
  bool fresh = sensor_->fresh();

  if ((int32_t)(now - t_next_poll) >= 0) {
    t_next_poll = now + sensor_->pollPeriodMs();
    sensor_->poll();
  }

  if ((int32_t)(now - t_next_print) >= 0) {
    t_next_print = now + cfg::PRINT_PERIOD_MS;
    char line[160];
    if (fresh) {
      sensor_->logLine(line, sizeof(line));
      Serial.println(line);
    } else {
      Serial.printf("[NODE] no fresh data (failed polls=%lu) - check wiring/bus master\n",
                    (unsigned long)sensor_->failCount());
    }
  }

  if ((int32_t)(now - t_next_feed) >= 0) {
    t_next_feed = now + cfg::FEED_PERIOD_MS;
    if (fresh && (mon_.learning() || mon_.thetaReady())) {
      sensor_->features(last_raw_);
      have_raw_ = true;
      TinyRLConditionMonitor::StepOut out;
      mon_.step(last_raw_, out);

      if (out.alarm_open) {
        ep_.open(last_raw_, mon_.space(), now);
        publishCondition(true);
        saveAll();
      } else if (out.alarm_clear) {
        ep_.close("cleared", mon_.space(), now);
        publishCondition(true);
        saveAll();
      } else if (mon_.alarmed() && ep_.isOpen()) {
        ep_.feed(last_raw_);
      }
    }
  }

  if (msg_.connected() && fresh && have_raw_) {
    // State/condition: retained, publish only when state/condition changes
    char key[48];
    snprintf(key, sizeof(key), "%s|%s", mon_.stateString(), conditionId());
    if (strcmp(key, last_cond_key_) != 0) {
      if (msg_.publishCondition(mon_.stateString(), conditionId(),
                                mon_.confidence(), ep_.duration(now), true))
        snprintf(last_cond_key_, sizeof(last_cond_key_), "%s", key);
    }
  }

  if ((mon_.learning() || mon_.thetaReady()) &&
      now - last_model_save_ > cfg::AUTOSAVE_MS)
    saveAll();
}
