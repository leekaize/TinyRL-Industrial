// Host-side WiFi/PubSubClient/LittleFS stubs for verification only.
#pragma once
#include <Arduino.h>
#include <map>
#include <vector>
#include <string>

// ---- WiFi --------------------------------------------------------------
class WiFiClient {
public:
  operator bool() const { return true; }
};

class WiFiClass {
public:
  void mode(int) {}
  void setSleep(bool) {}
  void setTxPower(int) {}
  void begin(const char*, const char*) {}
  void reconnect() {}
  int status() { return WL_CONNECTED; }
};
extern WiFiClass WiFi;

// ---- PubSubClient -------------------------------------------------------
class PubSubClient {
public:
  using Callback = std::function<void(char*, uint8_t*, unsigned int)>;
  PubSubClient(WiFiClient&) {}
  void setServer(const char*, uint16_t) {}
  void setCallback(Callback cb) { cb_ = cb; }
  void setKeepAlive(uint16_t) {}
  void setSocketTimeout(uint16_t) {}
  void setBufferSize(uint16_t) {}
  bool connect(const char*) { return true; }
  bool subscribe(const char*, int) { return true; }
  bool publish(const char*, const char*, bool = false) { return true; }
  bool connected() { return true; }   // non-const: matches PubSubClient 2.8
  bool loop() { return true; }
  void deliver(char* t, uint8_t* p, unsigned int n) { if (cb_) cb_(t, p, n); }
private:
  Callback cb_;
};

// ---- LittleFS (RAM-backed: real behavior for round-trip tests) ----------
class File {
public:
  File() {}
  File(std::vector<uint8_t>* slot, bool write)
      : slot_(slot), write_(write) {}
  operator bool() const { return slot_ != nullptr; }
  size_t write(const uint8_t* p, size_t n) {
    if (!slot_) return 0;
    if (write_ && pos_ == 0) slot_->clear();
    size_t end = pos_ + n;
    if (slot_->size() < end) slot_->resize(end);
    memcpy(slot_->data() + pos_, p, n);
    pos_ += n;
    return n;
  }
  size_t read(uint8_t* p, size_t n) {
    if (!slot_) return 0;
    size_t avail = slot_->size() > pos_ ? slot_->size() - pos_ : 0;
    size_t take = n < avail ? n : avail;
    memcpy(p, slot_->data() + pos_, take);
    pos_ += take;
    return take;
  }
  template <typename... A> size_t printf(const char* f, A... a) {
    char buf[1024];
    int n = snprintf(buf, sizeof(buf), f, a...);
    if (n < 0) return 0;
    return write((const uint8_t*)buf, (size_t)n);
  }
  void close() { slot_ = nullptr; }
  bool available() { return slot_ && pos_ < slot_->size(); }
  String readStringUntil(char term) {
    std::string out;
    while (slot_ && pos_ < slot_->size()) {
      char c = (char)(*slot_)[pos_++];
      if (c == term) break;
      out += c;
    }
    return String(out.c_str());
  }
private:
  std::vector<uint8_t>* slot_ = nullptr;
  bool write_ = false;
  size_t pos_ = 0;
};

class LittleFSClass {
public:
  bool begin(bool = false) { return true; }
  bool exists(const char* p) { return files_.count(p) != 0; }
  bool remove(const char* p) { return files_.erase(p) > 0; }
  File open(const char* p, const char* mode) {
    auto& slot = files_[p];
    if (mode[0] == 'r') {
      if (files_.find(p) == files_.end() || slot.empty()) return File();
      return File(&slot, false);
    }
    slot.clear();
    return File(&slot, true);
  }
private:
  std::map<std::string, std::vector<uint8_t>> files_;
};
extern LittleFSClass LittleFS;
