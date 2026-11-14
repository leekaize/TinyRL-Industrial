// Stub implementations (host verification only).
#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <Wire.h>

unsigned long g_host_millis = 0;
EspStub ESP;
HardwareSerial Serial;
HardwareSerial Serial1(true);   // silent: RS485 frames don't go to stdout
WiFiClass WiFi;
LittleFSClass LittleFS;
SPIClass SPI;
TwoWire Wire;
