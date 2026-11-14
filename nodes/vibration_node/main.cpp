// =====================================================================
// main.cpp -- vibration_node: a complete TinyRL semantic condition node.
//
// This is the WHOLE node-specific code. Everything else is the general
// core compiled unchanged for every sensor node. The sensor package
// (nodes/sensors/vibration_adxl/) provides 6 features: vibration RMS,
// four FFT band powers, dominant frequency.
//
// LIFE CYCLE (same core as vfd_g100_node):
//   boot -> restore model + net + library from LittleFS
//     model ready -> straight back to monitoring (no relearning)
//     fresh/IDLE  -> waits for YOUR input:
//        console 's'   or   MQTT Action/feedback {"action":"start"}
//   learning: ~30 s CAL then ~30 s theta fit (1 min total). Machine at
//   NORMAL condition!
//   monitoring: an alarm opens an episode. Known condition -> labelled
//   at once. Unknown -> State/event "open" with condition_id "unknown"
//   is the implicit ASK. Answer with console 'f <label>' or MQTT.
//
// Ported from the vibration_streamq MVP (2026-11-13): the MVP's Stream-Q
// agent, console, library and raw-upload ring were replaced by the
// modular core; the ADXL345 driver and the FFT/band DSP carried over.
// =====================================================================
#include <Arduino.h>
#include <LittleFS.h>
#include "node_runtime.h"
#include "vibration_sensor.h"

// ---------------- deployment identity (EDIT THESE) -------------------
static const char* WIFI_SSID = "tinyrl-modem";
static const char* WIFI_PASS = "TinyRL-Industrial@123";
static const char* MQTT_HOST = "192.168.201.99";   // broker IP
static constexpr uint16_t MQTT_PORT = 1883;
static const char* DEV_ID  = "vib-01";             // MQTT client id

// UNS base path - must match uns/uns_namespace.json (rename in BOTH).
// NOTE: this node is NOT yet in uns/uns_namespace.json - add its tree
// (and the uns_schema entry if fields change) before deploying.
#define UNS_BASE  "v1/Plant01/Motor-Test-Area/Line1/Vibration-Stand-01"
// ----------------------------------------------------------------------

static VibrationSensor sensor;
static SensorNodeApp app;

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 4000) {}

  Serial.println("\n=== TinyRL semantic condition node (vib_adxl, modular) ===");

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
