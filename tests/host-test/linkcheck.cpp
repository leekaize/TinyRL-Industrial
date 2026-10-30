// Link check: pulls in main.cpp's setup()/loop() once + exercises the
// console command paths (s / m / l / f) end-to-end.
#include <Arduino.h>
extern void setup();
extern void loop();
extern HardwareSerial Serial;
static void ticks(int n) { for (int i = 0; i < n; ++i) loop(); }
int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  setup();
  ticks(3);
  printf("--- console: s (start learning) ---\n");
  Serial.feed("s\n");
  ticks(2);
  printf("--- console: m (status) ---\n");
  Serial.feed("m\n");
  ticks(2);
  printf("--- console: l (library) ---\n");
  Serial.feed("l\n");
  ticks(2);
  printf("--- console: f belt_slip (no open episode -> graceful) ---\n");
  Serial.feed("f belt_slip\n");
  ticks(2);
  printf("--- console: ? (usage) ---\n");
  Serial.feed("?\n");
  ticks(2);
  printf("=== LINK + SETUP/LOOP + CONSOLE OK ===\n");
  return 0;
}
