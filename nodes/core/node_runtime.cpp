// =====================================================================
// node_runtime.cpp -- orchestration preserved from the vfd_streamq MVP,
// generalized over the FeatureProfile (nothing here knows about VFDs).
// =====================================================================
#include <Arduino.h>
#include <LittleFS.h>
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
  if (mon_.alarmed()) return ep_.isOpen() ? ep_.cond() : "unknown";
  // quiet: surface the recognised state, ANY kind (user decision
  // 2026-11-15: a learned fault shows as recognised while quiet - the
  // net predicts it, so it no longer alarms; the name is the evidence)
  int m = lib_.match(last_raw_, mon_.space());
  if (m >= 0) return lib_.at(m).id;
  return "normal";
}

float SensorNodeApp::score() const { return mon_.score(); }
float SensorNodeApp::theta() const { return mon_.theta(); }

const char* SensorNodeApp::phase() const {
  if (!mon_.learning() && !mon_.thetaReady()) return "idle";
  if (!mon_.thetaReady()) return mon_.space().armed() ? "fit" : "cal";
  return mon_.alarmed() ? "alarm" : "mon";
}

bool SensorNodeApp::episodeOpen() const { return ep_.isOpen(); }
uint32_t SensorNodeApp::stepCount() const { return mon_.steps(); }
uint32_t SensorNodeApp::libSize() const { return lib_.size(); }

void SensorNodeApp::episodeMeans(float* out) const { ep_.means(out); }

int SensorNodeApp::shareLabel(const char* slug, const float* mean_raw,
                              CondKind kind) {
  // the asker's raw means, mapped through THIS node's own frozen space;
  // no spread travels with a share, so units keep the CAL fallback until
  // the sibling labels the condition from its own batch
  float rmean[cfg::MAX_FEATURES];
  for (int f = 0; f < mon_.space().nFeatures(); ++f)
    rmean[f] = mon_.space().rel(f, mean_raw[f]);
  return lib_.label(slug, rmean, nullptr, "shared", kind);
}

int SensorNodeApp::libMatch(const float* raw) const {
  return lib_.match(raw, mon_.space());
}

const char* SensorNodeApp::libName(int idx) const {
  return (idx >= 0 && idx < lib_.size()) ? lib_.at(idx).id : "";
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
  quiet_unmatched_ = 0;

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
  mon_.attachLibrary(&lib_);   // normal-kind centroids quiet the alarm

  pend_.begin(sensor.profile().n_features);
  pend_.load(cfg::PENDING_FILE);   // stored unknown batches survive reboot

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
  Serial.printf("[FS] %d pending batch(es)\n", pend_.size());
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
  char action[16] = "", label[32] = "", by[20] = "", kindstr[8] = "";
  char batch[12] = "";
  jsonStr(j, "action", action, sizeof(action));
  jsonStr(j, "label",  label,  sizeof(label));
  jsonStr(j, "by",     by,     sizeof(by));
  jsonStr(j, "kind",   kindstr, sizeof(kindstr));
  jsonStr(j, "batch",  batch,  sizeof(batch));
  // kind defaults to FAULT: conservative - only an explicitly named normal
  // condition ever auto-accepts into the alarm baseline
  CondKind kind = (strcmp(kindstr, "normal") == 0) ? COND_NORMAL : COND_FAULT;
  Serial.printf("[FB] action='%s' label='%s' kind='%s' by='%s'\n",
                action, label, kindstr, by);

  if (strcmp(action, "start") == 0) { startLearning(); return; }

  if (strcmp(action, "label") == 0 && label[0]) {
    if (batch[0]) {
      // batch flow (user workflow 2026-11-15): learn from the STORED
      // settled batch - works long after the episode closed
      int pi = pend_.find(batch);
      if (pi < 0) {
        Serial.printf("[FB] no pending batch '%s'\n", batch);
        return;
      }
      char slug[24];
      slugify(label, slug, sizeof(slug));
      bool existed = lib_.has(slug);
      int idx = lib_.label(slug, pend_.at(pi).rmean, pend_.at(pi).rstd,
                           by, kind);
      if (idx < 0) {
        Serial.println("[FB] condition library full (LIB_MAX)");
        return;
      }
      pend_.remove(batch);
      pend_.save(cfg::PENDING_FILE);
      ep_pend_ = -1;                 // the episode's batch identity is gone
      if (ep_.isOpen())
        ep_.relabel(slug, kind, millis());   // the operator named the
                                             // state the node is IN
      // governance: a fault label sharpens; mapping a batch onto an
      // EXISTING known state desensitises (user point 6). A NEW normal
      // condition needs no theta change: the net now trains on it and
      // surprise falls on its own.
      if (kind == COND_FAULT) {
        mon_.onFeedbackFault();
        Serial.printf("[FB] theta lowered to %.2f\n", mon_.theta());
      } else if (existed) {
        mon_.onFeedbackNormal();
        Serial.printf("[FB] theta raised to %.2f\n", mon_.theta());
      }
      lib_.save(cfg::COND_FILE);
      saveAll();
      publishCondition(true);
      Serial.printf("[FB] batch %s labeled '%s' (%s)\n", batch, slug,
                    kindstr);
      return;
    }
    // legacy live-episode flow: the ask is being answered right now
    if (!ep_.isOpen()) { Serial.println("[FB] no open episode to label"); return; }
    char slug[24];
    slugify(label, slug, sizeof(slug));
    float rmean[cfg::MAX_FEATURES], rstd[cfg::MAX_FEATURES];
    ep_.relMeans(rmean);
    ep_.relStd(rstd);
    int idx = lib_.label(slug, rmean, rstd, by, kind);
    if (idx < 0) {
      Serial.println("[FB] condition library full (LIB_MAX)");
      return;
    }
    ep_.relabel(slug, kind, millis());
    if (ep_pend_ >= 0) {             // the answered episode's stored batch
      char bid[12];                  // is consumed with it: no orphan batch
      snprintf(bid, sizeof(bid), "%s", pend_.at(ep_pend_).id);
      pend_.remove(bid);
      pend_.save(cfg::PENDING_FILE);
      ep_pend_ = -1;
    }
    if (kind == COND_FAULT)
      mon_.onFeedbackFault();        // confirmed fault -> sharpen (plan.md:
                                     // normal +10% / fault -5% governance)
    else
      mon_.onWorldChange();          // regime changed -> drop traces
    lib_.save(cfg::COND_FILE);
    saveAll();
    publishCondition(true);
    return;
  }

  if (strcmp(action, "discard") == 0 && batch[0]) {
    // discard deletes the batch only: the model is untouched, so the
    // same unknown will ask again on its next occurrence (user point 6)
    if (pend_.remove(batch)) {
      pend_.save(cfg::PENDING_FILE);
      ep_pend_ = -1;
      Serial.printf("[FB] batch %s discarded (model untouched)\n", batch);
    } else {
      Serial.printf("[FB] no pending batch '%s'\n", batch);
    }
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

void SensorNodeApp::listPending() { pend_.list(); }

bool SensorNodeApp::batchMoved(const float* raw) const {
  if (ep_pend_ < 0) return false;
  const PendingBatch& b = pend_.at(ep_pend_);
  for (int f = 0; f < mon_.space().nFeatures(); ++f) {
    float unit = fmaxf(b.rstd[f], 1e-3f);
    if (fabsf(mon_.space().rel(f, raw[f]) - b.rmean[f])
        > cfg::QUIET_Q * unit)
      return true;
  }
  return false;
}

uint32_t SensorNodeApp::pendingSize() const { return pend_.size(); }

bool SensorNodeApp::pendingAt(int i, char* id, size_t cap,
                              uint32_t* n, uint32_t* step) const {
  if (i < 0 || i >= (int)pend_.size()) return false;
  snprintf(id, cap, "%s", pend_.at(i).id);
  if (n) *n = pend_.at(i).n;
  if (step) *step = pend_.at(i).last_step;
  return true;
}

bool SensorNodeApp::libCentroid(int idx, float* out, CondKind* kind) const {
  if (idx < 0 || idx >= lib_.size()) return false;
  const Condition& c = lib_.at(idx);
  for (int f = 0; f < mon_.space().nFeatures(); ++f) out[f] = c.c[f];
  if (kind) *kind = c.kind;
  return true;
}

float SensorNodeApp::libUnit(int idx, int f) const {
  if (idx < 0 || idx >= lib_.size() || f < 0 || f >= mon_.space().nFeatures())
    return 0.0f;
  return lib_.at(idx).u[f];
}

void SensorNodeApp::printStatus() {
  char line[160];
  sensor_->logLine(line, sizeof(line));
  Serial.printf("[NODE] %s | state=%s cond=%s conf=%.2f\n",
                line, mon_.stateString(), conditionId(), mon_.confidence());
  Serial.printf("[RL] step=%lu score=%.3f theta=%.3f params=%u lib=%d pubs=%lu "
                "qum=%lu heap=%u\n",
                (unsigned long)mon_.steps(), mon_.score(), mon_.theta(),
                (unsigned)mon_.paramCount(), lib_.size(),
                (unsigned long)msg_.publishes(), (unsigned long)quiet_unmatched_,
                (unsigned)ESP.getFreeHeap());
}

void SensorNodeApp::factoryReset() {
  mon_.factoryReset();
  lib_.clear();
  pend_.begin(sensor_->profile().n_features);
  LittleFS.remove(cfg::PENDING_FILE);
  persist::factoryReset();
  last_cond_key_[0] = 0;
  quiet_unmatched_ = 0;
  ident_drift_ = 0;
  baseline_registered_ = false;
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

      // NOTE: no supersede on transitions here. A noise flicker looks
      // exactly like a small transition to a vibration node (its fault
      // shifts are within its own noise envelope), so killing open
      // episodes on unsettled steps destroyed batches routinely. An open
      // episode instead stays until it clears quietly, or until a NEW
      // alarm overwrites it (ep_.open resets the sums); its stored batch
      // keeps accumulating settled samples either way.

      if (mon_.thetaReady() && !baseline_registered_) {
        baseline_registered_ = true;
        if (lib_.size() == 0) {
          // first commissioning: the machine is at STOPPED (user procedure
          // 2026-11-15), so the CAL baseline IS the stopped state. Its
          // condition position IS the origin (rel of the CAL mean = 0 on
          // every feature); units stay 0 -> CAL fallback, because a
          // baseline registered without a batch has no measured spread.
          float zero[cfg::MAX_FEATURES] = {};
          lib_.label("op_stopped", zero, zero, "baseline", COND_NORMAL);
          lib_.save(cfg::COND_FILE);
          Serial.println("[LIB] baseline 'op_stopped' registered (normal)");
        }
      }

      if (out.alarm_open) {
        if (ep_.isOpen())
          ep_.close("superseded", mon_.space(), now);   // new alarm: the
        // old episode's sums are replaced; its STORED batch (if any)
        // stays for late labeling - only ep_pend_ resets
        ep_pend_ = -1;
        ep_.open(last_raw_, mon_.space(), now);
        if (strcmp(ep_.cond(), "unknown") == 0) {
          // settled unknown state -> store the batch and ask (user
          // workflow 2026-11-15: label or discard by id, late labels ok)
          float rm[cfg::MAX_FEATURES], rs[cfg::MAX_FEATURES];
          ep_.relMeans(rm);
          ep_.relStd(rs);
          int pi = pend_.add(rm, rs, ep_.count(), now, mon_.steps());
          if (pi >= 0) {
            ep_pend_ = pi;
            Serial.printf("[PEND] batch %s stored (%lu samples) - "
                          "label or discard by id\n",
                          pend_.at(pi).id, (unsigned long)pend_.at(pi).n);
            pend_.save(cfg::PENDING_FILE);
          }
        }
        publishCondition(true);
        saveAll();
      } else if (out.alarm_clear) {
        ep_.close("cleared", mon_.space(), now);
        if (ep_pend_ >= 0) {          // freeze the batch at the settled mean
          pend_.save(cfg::PENDING_FILE);
          ep_pend_ = -1;
        }
        publishCondition(true);
        saveAll();
      } else if (mon_.alarmed() && ep_.isOpen()) {
        // settled-only feeding (2026-11-15 bench finding): a batch that
        // keeps feeding across a state change holds a MIXED distribution -
        // huge spread, huge radius, and the poisoned centroid then
        // captures states it was never labelled for. Feeding settled
        // steps only keeps the batch = ONE state; the identity check
        // below then sees a frozen reference and actually fires.
        if (mon_.settled()) {
          ep_.feed(last_raw_, mon_.space());
          if (ep_pend_ >= 0) {        // the batch tracks the settled mean
            float rm[cfg::MAX_FEATURES], rs[cfg::MAX_FEATURES];
            ep_.relMeans(rm);
            ep_.relStd(rs);
            pend_.update(ep_pend_, rm, rs, ep_.count(), now, mon_.steps());
          }
          // settled identity check (2026-11-15 bench finding): an episode
          // that stays open across a state change otherwise feeds a MIXED
          // batch forever - no new ask, stale cond (the speed_1 episode
          // swallowing speed_2 was the tour's missing asks). Only SETTLED
          // steps are judged: mid-transition the state is meaningless.
          // A drift needs IDENT_DRIFT_N consecutive confirmations: single
          // noise vetoes must not churn episodes.
          bool stale = false;
          int m = lib_.match(last_raw_, mon_.space(), cfg::QUIET_Q);
          if (strcmp(ep_.cond(), "unknown") == 0) {
            if (m >= 0) {
              stale = true;           // the state returned to a KNOWN one
            } else if (ep_pend_ >= 0) {
              stale = batchMoved(last_raw_);
            }
          } else if (m < 0 ||
                     strcmp(lib_.at(m).id, ep_.cond()) != 0) {
            stale = true;             // named episode, state moved away
          }
          if (stale) ++ident_drift_; else ident_drift_ = 0;
          if (ident_drift_ >= cfg::IDENT_DRIFT_N) {
            ident_drift_ = 0;
            Serial.printf("[EP] state moved under the episode - "
                          "re-identifying\n");
            ep_.close("superseded", mon_.space(), now);
            ep_pend_ = -1;            // the old batch stays for late labels
            ep_.open(last_raw_, mon_.space(), now);
            if (strcmp(ep_.cond(), "unknown") == 0) {
              float rm[cfg::MAX_FEATURES], rs[cfg::MAX_FEATURES];
              ep_.relMeans(rm);
              ep_.relStd(rs);
              int pi = pend_.add(rm, rs, ep_.count(), now, mon_.steps());
              if (pi >= 0) {
                ep_pend_ = pi;
                Serial.printf("[PEND] batch %s stored (%lu samples) - "
                              "label or discard by id\n",
                              pend_.at(pi).id, (unsigned long)pend_.at(pi).n);
                pend_.save(cfg::PENDING_FILE);
              }
            }
            publishCondition(true);
            saveAll();
          }
        }
      } else if (mon_.thetaReady() && !mon_.alarmed() && !out.alarm_clear &&
                 lib_.size() > 0) {
        // Settles 2012 ch. 2.5 myopia tripwire (RESEARCH.md design decisions):
        // a quiet step whose profile matches no centroid within the tripwire
        // radius (MATCH_Q per-feature units, condition space) may be a
        // confidently-blind region. Counted and logged locally only: never
        // published, never asked about. Volatile by design (tripwire reads
        // recent history).
        if (lib_.match(last_raw_, mon_.space()) < 0) {
          ++quiet_unmatched_;
          if (quiet_unmatched_ % cfg::QUM_REPORT_EVERY == 0)
            Serial.printf("[LIB] quiet-unmatched=%lu steps (no centroid within "
                          "q=%.1f units) - possible blind region\n",
                          (unsigned long)quiet_unmatched_, cfg::MATCH_Q);
        }
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
