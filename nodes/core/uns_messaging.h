// =====================================================================
// uns_messaging.h -- the node's ONLY contact with the outside world.
//
// Semantic contract (schema uns-v1.1, see uns/uns_schema_v1.json):
//   State/condition  retained, on change:
//     {"state","condition_id","confidence","episode_s","ts_ms"}
//   State/event      per episode transition, not retained:
//     {"event","condition_id","severity","duration_s","summary",
//      "meas":{<feature>:<value>,...},"ts_ms"}
//   State/node_info  retained, republished on every (re)connect:
//     {"schema","node_type","dev_id","features":[{name,unit}...],
//      "lib_max","fw"}   <- lets an AI agent discover the node
//   Action/feedback  subscribed QoS1, the ONLY input channel:
//     {"action":"start"}
//     {"action":"label","label":"belt_slip","by":"agent-01"}
//     {"action":"normal"}  {"action":"fault"}
//
// Design rule: publish CONDITION + STATUS, not telemetry. Raw values
// travel only inside episode summaries (already aggregated on-device).
// =====================================================================
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <functional>
#include <WiFi.h>
#include <PubSubClient.h>
#include "sensor_interface.h"

struct NodeIdentity {
  const char* dev_id;       // "g100-01"
  const char* uns_base;     // "v1/Plant01/Motor-Test-Area/Line1/Drive-Station-01"
  const char* wifi_ssid;
  const char* wifi_pass;
  const char* mqtt_host;
  uint16_t    mqtt_port;
};

class UnsMessaging {
public:
  using FeedbackFn = std::function<void(const char* json)>;

  // builds topic strings from identity; wires WiFi+MQTT but does not block
  void begin(const NodeIdentity& id, const FeatureProfile& prof, FeedbackFn fb);

  void maintain();                       // call every loop(): reconnect logic
  bool connected() { return mqtt_.connected(); }   // PubSubClient::connected() is non-const
  uint32_t publishes() const { return pub_count_; }

  bool publishCondition(const char* state, const char* cond_id,
                        float confidence, uint32_t episode_s, bool retained);
  bool publishEvent(const char* ev, const char* sev, const char* cid,
                    uint32_t duration_s, const char* summary,
                    const float* means);
  bool publishNodeInfo();                // retained discovery document

private:
  void callback(char* topic, uint8_t* payload, unsigned int len);

  NodeIdentity   id_{};
  const FeatureProfile* prof_ = nullptr;
  FeedbackFn     fb_;
  WiFiClient     net_;
  PubSubClient   mqtt_{net_};    // bound to the client at construction

  char t_cond_[112], t_evt_[112], t_fb_[112], t_info_[112];
  uint32_t pub_count_ = 0;
  bool     wifi_was_up_ = false;
};
