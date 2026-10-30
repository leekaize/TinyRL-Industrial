// =====================================================================
// sensor_interface.h -- THE porting contract of the general node core.
//
// A "sensor package" is the ENVIRONMENT-SPECIFIC half of a node. It only
// has to implement this interface; everything downstream (TinyRL learning,
// episodes, condition library, UNS messaging, console, persistence) is
// sensor-agnostic and compiled unchanged for every node.
//
// Contract:
//   begin()      once at boot; init busses, return false if hardware absent
//   poll()       read the hardware ONCE (non-blocking-ish); the app drives
//                the timer using pollPeriodMs()
//   pollPeriodMs()  cadence the hardware can sustain (e.g. Modbus: 300 ms)
//   fresh()      true if last poll succeeded within its freshness window
//   features()   fill n_features engineering-unit values; the ORDER and
//                MEANING are declared by profile() and must stay stable:
//                they index the TinyRL model and the condition library.
//   logLine()    one human-readable telemetry line for the console.
//
// See sensors/vfd_g100/ for the reference implementation.
// =====================================================================
#pragma once
#include <stddef.h>
#include <stdint.h>

struct FeatureInfo {
  const char* name;   // short slug, used in UNS payloads + console  e.g. "i"
  const char* unit;   // engineering unit string                       e.g. "A"
};

struct FeatureProfile {
  const char* node_type;      // e.g. "vfd_g100"
  int         n_features;
  const FeatureInfo* features;
};

class ISensorNode {
public:
  virtual ~ISensorNode() = default;

  virtual bool begin() = 0;
  virtual void poll() = 0;
  virtual uint32_t pollPeriodMs() const = 0;

  virtual bool     fresh() const = 0;
  virtual uint32_t lastOkMs() const = 0;
  virtual uint32_t failCount() const = 0;

  virtual const FeatureProfile& profile() const = 0;

  // out must hold profile().n_features floats (engineering units).
  virtual void features(float* out) const = 0;

  // Human-readable one-liner, e.g. "[G100] I=3.10A Vdc=585V ...".
  virtual size_t logLine(char* out, size_t cap) = 0;
};
