# ADV-UnitLab

Firmware for the **M5Stack Cardputer ADV** that gets everything out of M5 Units and Caps: one section per device, live data, graphs, logs, and captures you can replay. Sister project of [SensorLab](https://github.com/RIAL0/SensorLab), with the same look and keys.

Install the `.bin` from [Releases](../../releases) with [M5Launcher](https://github.com/bmorcelli/Launcher) (SD → select → Install).

## Sections

| Section | Device | What you get |
|---|---|---|
| **ENV-Pro** | U169 (BME688, I2C 0x77) | Temperature, humidity, dew point, absolute humidity, heat index, pressure + 3 h trend, altitude, gas resistance. Graphs over 10 min / 100 min / 10 h. Alarms. CSV log. IAQ/eCO2/bVOC in the BSEC2 build |
| **GPS** | U032-V11 (ATGM336H, UART) | Position, speed, course, altitude, DOPs, UTC, sky plot per constellation, C/N0 bars, trip computer, GPX log, 1/5/10 Hz |
| **Infrared** | U002 + built-in IR LED | Capture + decode + raw timings, save as Flipper `.ir`, replay saved remotes, manual sender |
| **Detect** | Grove port | Says what is plugged in and how it is wired |

Coming next (see [docs/ROADMAP.md](docs/ROADMAP.md)): RFID2 U031-B, NFC U216 and Cap CC1101 U219 (NFC + Sub-GHz), Hydra-RF 424 nRF24, Gateway H2 U195 (Zigbee / Thread / Matter).

## Keys

`;` up · `.` down · `,` left · `/` right · `ENTER` select · `` ` `` / Backspace / G0 back · `TAB` next view · `1`–`5` jump to view

| Section | Extra keys |
|---|---|
| ENV-Pro | `L` start/stop CSV log · `M` mute alarm · Graph: `;`/`.` channel, `,`/`/` time span · Data: `S` save · Alarms: `,`/`/` value, `ENTER` on/off |
| GPS | `L` start/stop GPX log · `R` update rate · `C` cold start · `Z` reset trip · Data: `S` save |
| IR capture | `ENTER` replay · `S` save to .ir · `R` raw timings · `P` force raw · `N` new file · `;`/`.` history |

## Files on the SD card

```
/UnitLab/env/env_NNNN.csv      ENV-Pro log, one row every 3 s
/UnitLab/gps/track_NNNN.gpx    GPS track, one point per second
/UnitLab/ir/remote_NNNN.ir     Flipper Zero infrared format (copy .ir files here to replay them)
/UnitLab/notes/*.txt           "S save" snapshots of data screens
```

## Hardware notes

- **One Grove port.** One unit at a time. ENV-Pro, NFC and RFID2 can share a Grove hub (addresses 0x77, 0x50, 0x28). GPS and Gateway H2 need the port to themselves.
- The firmware finds the UART wire order and baud rate itself, so GPS works whichever way the cable maps RX/TX.
- **IR**: the unit's receiver is found by its idle-high output. Sending can use the Cardputer's own LED (G44) or the unit's LED.
- **ENV-Pro temperature** reads slightly high from self-heating. Set a correction in Settings → ENV temp offset (it also corrects relative humidity). Set your local sea-level pressure there for an accurate altitude.

## Building

```
pip install platformio
pio run -e cardputer-adv          # public image (raw BME68x)
pio run -e cardputer-adv-bsec     # private image with Bosch BSEC2 (IAQ, eCO2, bVOC)
```

Output: `.pio/build/<env>/firmware.bin`.

**Why two images:** Bosch's BSEC2 is a closed binary library whose [licence](https://www.bosch-sensortec.com/media/boschsensortec/downloads/software/bme688_development_software/2024_12/20241219_clickthrough_license_terms_bsec_bme680_bme688_bme690.pdf) restricts redistribution. Releases ship the raw-driver image; build the BSEC2 image yourself for your own device. BSEC2 calibration (IAQ accuracy 3) is saved to flash hourly and restored at start, so it survives power cycles. A new sensor needs a day or two of run-in.

Pinned toolchain: `espressif32@6.12.0` (Arduino-ESP32 2.0.17), M5Unified 0.2.29, M5GFX 0.2.36, M5Cardputer 1.1.1, IRremote 4.7.1, TinyGPSPlus 1.1.0, BME68x 1.3.40408, BSEC2 1.9.2610. These match SensorLab, which is proven on the hardware.

## Licence

MIT for this code. BSEC2 (private build only) is under Bosch's own terms.
