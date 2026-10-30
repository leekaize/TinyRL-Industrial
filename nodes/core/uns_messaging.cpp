// =====================================================================
// uns_messaging.cpp -- WiFi/MQTT plumbing + semantic publishers.
// WiFi brownout fix, keepalive and buffer settings preserved verbatim
// from the bench-proven vfd_streamq MVP.
// =====================================================================
#include <Arduino.h>
#include <string.h>
#include <stdio.h>
#include "uns_messaging.h"
#include "node_config.h"

void UnsMessaging::begin(const NodeIdentity& id, const FeatureProfile& prof,
                         FeedbackFn fb) {
  id_ = id;
  prof_ = &prof;
  fb_ = std::move(fb);
  snprintf(t_cond_, sizeof(t_cond_), "%s/State/condition", id_.uns_base);
  snprintf(t_evt_,  sizeof(t_evt_),  "%s/State/event",     id_.uns_base);
  snprintf(t_fb_,   sizeof(t_fb_),   "%s/Action/feedback",  id_.uns_base);
  snprintf(t_info_, sizeof(t_info_), "%s/State/node_info",  id_.uns_base);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_11dBm);        // weak-supply brownout fix
  WiFi.begin(id_.wifi_ssid, id_.wifi_pass);
  mqtt_.setServer(id_.mqtt_host, id_.mqtt_port);
  mqtt_.setCallback([this](char* t, uint8_t* p, unsigned int l) {
    callback(t, p, l);
  });
  mqtt_.setKeepAlive(15);
  mqtt_.setSocketTimeout(4);
  mqtt_.setBufferSize(512);
}

void UnsMessaging::maintain() {
  static uint32_t last_try = 0;
  if (WiFi.status() != WL_CONNECTED) {
    if (wifi_was_up_) { Serial.println("[NET] WiFi lost"); wifi_was_up_ = false; }
    if (millis() - last_try > 10000) { last_try = millis(); WiFi.reconnect(); }
    return;
  }
  if (!mqtt_.connected()) {
    if (millis() - last_try > 5000) {
      last_try = millis();
      if (mqtt_.connect(id_.dev_id)) {
        mqtt_.subscribe(t_fb_, 1);
        publishNodeInfo();
        wifi_was_up_ = true;
        Serial.println("[NET] MQTT connected - Action/feedback subscribed");
      }
    }
    return;
  }
  mqtt_.loop();
}

bool UnsMessaging::publishCondition(const char* state, const char* cond_id,
                                    float conf, uint32_t episode_s,
                                    bool retained) {
  char j[224];
  int n = snprintf(j, sizeof(j),
    "{\"state\":\"%s\",\"condition_id\":\"%s\",\"confidence\":%.2f,"
    "\"episode_s\":%lu,\"ts_ms\":%lu}",
    state, cond_id, conf, (unsigned long)episode_s, (unsigned long)millis());
  bool ok = n > 0 && mqtt_.publish(t_cond_, j, retained);
  if (ok) pub_count_++;
  return ok;
}

bool UnsMessaging::publishEvent(const char* ev, const char* sev,
                                const char* cid, uint32_t duration_s,
                                const char* summary, const float* means) {
  char j[640];
  int n = snprintf(j, sizeof(j),
    "{\"event\":\"%s\",\"condition_id\":\"%s\",\"severity\":\"%s\","
    "\"duration_s\":%lu,\"summary\":\"%s\",\"meas\":{",
    ev, cid, sev, (unsigned long)duration_s, summary);
  if (n < 0) return false;
  int o = n;
  for (int f = 0; f < prof_->n_features && o < (int)sizeof(j) - 40; ++f) {
    int w = snprintf(j + o, sizeof(j) - o, "%s\"%s\":%.4g",
                     f ? "," : "", prof_->features[f].name, means[f]);
    if (w < 0) break;
    o += w;
  }
  n = snprintf(j + o, sizeof(j) - o, "},\"ts_ms\":%lu}",
               (unsigned long)millis());
  if (n < 0) return false;
  bool ok = mqtt_.publish(t_evt_, j);
  if (ok) pub_count_++;
  return ok;
}

bool UnsMessaging::publishNodeInfo() {
  char j[512];
  int o = snprintf(j, sizeof(j),
    "{\"schema\":\"%s\",\"node_type\":\"%s\",\"dev_id\":\"%s\","
    "\"features\":[", cfg::SCHEMA_VER, prof_->node_type, id_.dev_id);
  if (o < 0) return false;
  for (int f = 0; f < prof_->n_features && o < (int)sizeof(j) - 60; ++f) {
    int w = snprintf(j + o, sizeof(j) - o, "%s{\"name\":\"%s\",\"unit\":\"%s\"}",
                     f ? "," : "", prof_->features[f].name,
                     prof_->features[f].unit);
    if (w < 0) break;
    o += w;
  }
  snprintf(j + o, sizeof(j) - o,
           "],\"lib_max\":%d,\"fw\":\"%s\",\"ts_ms\":%lu}",
           cfg::LIB_MAX, cfg::FW_VERSION, (unsigned long)millis());
  bool ok = mqtt_.publish(t_info_, j, true);
  if (ok) pub_count_++;
  return ok;
}

void UnsMessaging::callback(char* topic, uint8_t* payload, unsigned int len) {
  static char j[512];
  unsigned int n = len < sizeof(j) - 1 ? len : sizeof(j) - 1;
  memcpy(j, payload, n);
  j[n] = 0;
  if (strstr(topic, "/Action/feedback") && fb_) fb_(j);
}
