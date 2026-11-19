// Section entry points. Each section claims the Grove port / Cap bus it needs and releases it on exit.
#pragma once
#include <Arduino.h>

void appEnv();        // U169 ENV-Pro (BME688)            app_env.cpp
void appGps();        // U032-V11 GPS v1.1 (ATGM336H)     app_gps.cpp
void appIr();         // U002 IR unit + built-in IR LED   app_ir.cpp
void appNfc();        // U216 NFC / U219 Cap NFC / RFID2      app_nfc.cpp
void appDetect();     // what is plugged into Grove        app_detect.cpp
void appSettings();   //                                   app_settings.cpp
