# Changelog

## 0.1

Phase 1: core, ENV-Pro, GPS, Infrared.

- **Core**: SensorLab-style menu, keyboard and screen helpers; Grove port manager that hands G1/G2 to one unit at a time; Detect screen (I2C scan with BME68x chip-id check, NMEA probe at 115200/9600/38400 in both wire orders, IR receiver detection).
- **ENV-Pro (U169)**: live view (temperature, humidity, dew point, absolute humidity, heat index, pressure with 3-hour trend, altitude, gas resistance), graphs of every channel over 10 min / 100 min / 10 h, full data list, alarms with hysteresis and non-blocking beeps, CSV logging. Public image uses Bosch's BME68x driver; the `cardputer-adv-bsec` build adds BSEC2 IAQ, static IAQ, eCO2 and bVOC with calibration saved to flash.
- **GPS (U032 v1.1)**: dashboard, sky plot by constellation, signal-strength bars, trip computer, full data list with every satellite, GPX logging, 1/5/10 Hz update rate and cold start (CASIC commands).
- **Infrared (U002 + built-in LED)**: capture with protocol decode plus raw timings, save to Flipper `.ir` files, browse and replay saved remotes, manual sender, choice of built-in or unit LED.
