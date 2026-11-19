# Roadmap

Each phase is released and tested on hardware before the next starts.

## Phase 1: core, ENV-Pro, GPS, Infrared (v0.1)

Done. See CHANGELOG.

## Phase 2: 13.56 MHz read + emulate (v0.2) — DONE

Read NFC-A on U216 / U219 Cap / RFID2; NTAG/Ultralight full dump to Flipper `.nfc`; UID-level save for other tags; NTAG/Ultralight emulation on the two ST25R3916 devices. Toolchain confirmed on `espressif32@6.12.0` — no move to Arduino-ESP32 3.x was needed.

## Phase 2.5: MIFARE Classic + magic cards

- **MIFARE Classic full dump**: per-sector key auth (default keys + a `/UnitLab/nfc/keys.txt` dictionary), save all blocks to `.nfc`.
- **Magic cards (Gen1a / CUID)**: write a saved dump to a UID-changeable card via the ST25R3916 raw transceive (backdoor commands). No library support, so this is built and tested against real magic cards.
- **125 kHz is out of scope** for all UnitLab hardware (13.56 MHz only). Indala / HID Prox / EM4100 stay on the Flipper's LF antenna.

## Phase 2 reference (original plan)

| Device | Bus | Driver | Read/save | Emulate |
|---|---|---|---|---|
| U031-B RFID2 (WS1850S, RC522 clone) | Grove I2C 0x28 | MFRC522 over I2C | UID, ATQA/SAK, MIFARE Classic sectors (default keys + key file), Ultralight/NTAG pages | **No** (the chip cannot act as a card). Write the dump to a Gen1a/CUID magic card instead, or emulate it on an ST25R3916 device |
| U216 NFC (ST25R3916) | Grove I2C 0x50 | `m5stack/M5Unit-NFC` | NFC-A/B/F/V: UID, type, memory dumps, NDEF | NFC-A (UID, NTAG/Ultralight, NDEF) and NFC-F. MIFARE Classic (Crypto1) is not expected to work over I2C |
| Cap CC1101 U219, NFC half (ST25R3916) | Cap SPI, CS G6, IRQ G4 | same library, SPI transport | as U216 | as U216 |

- One shared card store: `/UnitLab/nfc/*.nfc` in Flipper format, so a card read on RFID2 can be emulated on U216/U219 or a Flipper.
- **Toolchain check needed first**: M5Unit-NFC 0.1.x builds on M5UnitUnified. Confirm it compiles on `espressif32@6.12.0` (Arduino-ESP32 2.0.17); if it needs Arduino-ESP32 3.x, move the whole project to pioarduino and retest Phase 1.

## Phase 3: RF

| Screen | Hardware | Pins (Cardputer ADV) |
|---|---|---|
| Sub-GHz (CC1101) | Cap CC1101 U219 only | CS G5 · GDO0 G15 · SPI SCK G40 / MOSI G14 / MISO G39 · band switch RF_SW0 G13 + RF_SW1 = CC1101 GDO2 (00 = 315, 01 = 433, 11 = 868/915) |
| 2.4 GHz (nRF24) | Hydra-RF 424 (nRF24 half only; its CC1101 is not used) | **To confirm on the board before any TX**: third-party docs list nRF24 CS G6 / CE G4. Check against PINGEQUA's `brucePins.conf` |

- The SPI bus is shared with the SD card (CS G12). Every screen holds the unused chip selects high.
- Cap detection: read the CC1101 PARTNUM/VERSION registers at CS G5 (expect 0x00/0x14); nRF24 detection by writing and reading back RF_SETUP at CS G6. A Cap U219 also answers on G6 (its ST25R3916), so detection checks the CC1101 first.
- Sub-GHz: RSSI sweep (real dBm), frequency finder, raw OOK/ASK capture and replay, Flipper `.sub` files. Re-set the band switch after every frequency change.
- nRF24: 126-channel activity heatmap built from the RPD bit (above about −64 dBm, so activity rather than dBm) with a Wi-Fi channel overlay, plus a list of nRF24 device addresses heard.
- Never transmit without the antenna fitted.

## Phase 4: Gateway H2 U195 (802.15.4)

- Second build target in this repo: `h2-sniffer/` (ESP-IDF, `esp32h2`), flashed once over the unit's own USB-C (hold Boot while plugging in). Reversible: reflash M5's NCP image to go back.
- Sniffer uses `esp_ieee802154` in promiscuous mode, hops channels 11–26, and streams frames to the Cardputer over the Grove UART (framed: `0x7E len ch rssi lqi payload crc`), at 921600 baud if the cable allows, else 460800.
- Cardputer side lists per channel: PAN IDs, Zigbee beacons (permit-join, router/end-device capacity, extended PAN ID), Thread MLE discovery (network name, extended PAN ID), device short/extended addresses with OUI vendor, RSSI/LQI, frame counts.
- Matter: Thread devices appear above; devices waiting to be commissioned are found with the Cardputer's BLE (service 0xFFF6); Matter-over-Wi-Fi via mDNS `_matter._tcp` / `_matterc._udp`.
- Payloads are encrypted with the network key, so device names and commands are not visible unless you enter your own network's key.
