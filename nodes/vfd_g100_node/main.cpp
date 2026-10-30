// =====================================================================
// main.cpp -- vfd_g100_node: a complete TinyRL semantic condition node.
//
// This is the WHOLE node-specific code. Everything else is the general
// core (experiments/core) compiled unchanged for every sensor node.
//
// LIFE CYCLE (unchanged from the vfd_streamq MVP):
//   boot -> restore model + net + library from LittleFS
//     model ready -> straight back to monitoring (no relearning)
//     fresh/IDLE  -> waits for YOUR input:
//        console 's'   or   MQTT Action/feedback {"action":"start"}
//   learning: ~5 min CAL then ~2 min theta fit. Machine at NORMAL load!
//   monitoring: an alarm opens an episode. Known condition -> labelled
//   at once (nearest centroid). Unknown -> State/event "open" with
//   condition_id "unknown" is the implicit ASK. Answer it with console
//   'f <label>' or MQTT on Action/feedback:
//     {"action":"label","label":"belt_slip","by":"me"}   label episode
//     {"action":"normal"}                                false alarm
//     {"action":"fault"}                                 confirmed fault
//
// TinyRL difference vs the MVP: the anomaly score now comes from a real
// online learner (nn::Sequential + ObGD eligibility traces, examples/
// stream_x headers) doing one-step prediction on the 4 features, instead
// of hand-rolled transition-count NLL. Semantics (theta, episodes,
// labels, persistence) are preserved. Old /model.bin (v3) is ignored.
// =====================================================================
#include <Arduino.h>
#include <LittleFS.h>
#include "node_runtime.h"
#include "g100_sensor.h"

// ---------------- deployment identity (EDIT THESE) -------------------
static const char* WIFI_SSID = "tinyrl-modem";
static const char* WIFI_PASS = "TinyRL-Industrial@123";
static const char* MQTT_HOST = "192.168.201.99";   // broker IP
static constexpr uint16_t MQTT_PORT = 1883;
static const char* DEV_ID  = "g100-01";            // MQTT client id

// UNS base path - must match uns/uns_namespace.json (rename in BOTH).
#define UNS_BASE  "v1/Plant01/Motor-Test-Area/Line1/Drive-Station-01"
// ----------------------------------------------------------------------

static G100Sensor  sensor;
static SensorNodeApp app;

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 4000) {}

  Serial.println("\n=== TinyRL semantic condition node (vfd_g100, modular) ===");

  NodeIdentity id{};
  id.dev_id    = DEV_ID;
  id.uns_base  = UNS_BASE;
  id.wifi_ssid = WIFI_SSID;
  id.wifi_pass = WIFI_PASS;
  id.mqtt_host = MQTT_HOST;
  id.mqtt_port = MQTT_PORT;

  app.begin(sensor, id);
}

void loop() {
  app.tick();
  delay(2);
}
