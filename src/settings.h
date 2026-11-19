// Persistent settings (stored in the ESP32's NVS flash).
#pragma once
#include <Arduino.h>

#define UL_VERSION "0.1.1"

struct Config {
  uint8_t brightness = 140;   // backlight 16..255
  uint8_t dimIdx = 4;         // index into DIM_SEC: dim after 2 minutes
  uint8_t offIdx = 0;         // index into OFF_SEC: never switch the screen off
  uint8_t volume = 160;       // speaker volume 0..255 (alarm beeps, key clicks)
  int16_t tempOffsetC10 = 0;  // ENV-Pro temperature offset in 0.1 degC steps (self-heating correction)
  uint16_t seaLevelHpa10 = 10132;  // reference sea-level pressure for altitude, 0.1 hPa
};

// ENV-Pro alarm thresholds. A value counts only while its "on" flag is set.
struct EnvAlarms {
  bool tHiOn = true;   float tHi = 35.0f;    // degC
  bool tLoOn = false;  float tLo = 5.0f;     // degC
  bool rhHiOn = true;  float rhHi = 80.0f;   // %RH
  bool rhLoOn = false; float rhLo = 20.0f;   // %RH
  bool iaqOn = true;   float iaq = 200.0f;   // BSEC IAQ index (BSEC2 build only)
  bool co2On = true;   float co2 = 1500.0f;  // eCO2 ppm (BSEC2 build only)
  bool gasOn = false;  float gasDrop = 50.0f; // % drop of gas resistance below the session baseline (both builds)
  bool sound = true;
};

extern Config cfg;
extern EnvAlarms envAlarms;
void cfgLoad();
void cfgSave();
void alarmsLoad();
void alarmsSave();
uint32_t cfgDimMs();          // 0 = never
uint32_t cfgOffMs();          // 0 = never
