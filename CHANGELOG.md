# Changelog

## 0.1.1

Fixes from the first hardware test.

- **Detect no longer hangs with the IR unit** (or a UART unit) plugged in. New order: listen passively for UART traffic, then look for an IR receiver, and only then try I2C, never on a wire that is held low or carrying data. UART is only opened on wires already carrying data, so the IR LED is never driven by it. Every stage shows on screen and `` ` `` / G0 aborts. ENV-Pro refuses to start I2C when the port holds the IR or a UART unit.
- **IR**: Samsung remotes now save as Flipper `Samsung32` (parsed) instead of raw.
- **GPS**: antenna status from the receiver's `$GPTXT` shown on Dash and Data; unknown elevation/azimuth shown as `-`, and the sky plot says when no positions are known yet; text no longer runs off the Sky and Trip screens.
- **ENV-Pro**: `H` sets how often the gas heater runs (3 s / 30 s / off). Less heating gives truer temperature and humidity. Alarm lines no longer squash the graph scale. Fixed overlapping text in the Data list.

## 0.1

Phase 1: core, ENV-Pro, GPS, Infrared.

- **Core**: SensorLab-style menu, keyboard and screen helpers; Grove port manager that hands G1/G2 to one unit at a time; Detect screen (I2C scan with BME68x chip-id check, NMEA probe at 115200/9600/38400 in both wire orders, IR receiver detection).
- **ENV-Pro (U169)**: live view (temperature, humidity, dew point, absolute humidity, heat index, pressure with 3-hour trend, altitude, gas resistance), graphs of every channel over 10 min / 100 min / 10 h, full data list, alarms with hysteresis and non-blocking beeps, CSV logging. Public image uses Bosch's BME68x driver; the `cardputer-adv-bsec` build adds BSEC2 IAQ, static IAQ, eCO2 and bVOC with calibration saved to flash.
- **GPS (U032 v1.1)**: dashboard, sky plot by constellation, signal-strength bars, trip computer, full data list with every satellite, GPX logging, 1/5/10 Hz update rate and cold start (CASIC commands).
- **Infrared (U002 + built-in LED)**: capture with protocol decode plus raw timings, save to Flipper `.ir` files, browse and replay saved remotes, manual sender, choice of built-in or unit LED.
