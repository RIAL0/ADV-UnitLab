// Phase 2 toolchain probe: does M5Unit-NFC + M5Unit-RFID build against UnitLab's pinned
// platform (espressif32@6.12.0) and M5Unified 0.2.29? Compile-only; never flashed.
#include <M5Cardputer.h>
#include <M5UnitUnified.h>
#include <M5UnitUnifiedNFC.h>
#include <M5UnitUnifiedRFID.h>
#include <wiring/m5_unit_unified_wiring.hpp>
#include <vector>

static m5::unit::UnitUnified Units;
static m5::unit::UnitNFC unitNfc{};            // U216, I2C 0x50
static m5::unit::CapCC1101NFC capNfc{};        // U219 NFC half, SPI
static m5::unit::UnitRFID2 rfid2{};            // U031-B, I2C 0x28
static m5::nfc::NFCLayerA nfcA{unitNfc};
static m5::nfc::NFCLayerA capA{capNfc};
static m5::nfc::NFCLayerA rfidA{rfid2};

void setup() {
  M5Cardputer.begin();
  bool ok = m5::unit::wiring::addSPI(Units, capNfc, 10000000, 1) && Units.begin();
  (void)ok;
}

void loop() {
  Units.update();
  std::vector<m5::nfc::a::PICC> piccs;
  if (nfcA.detect(piccs)) {
    for (auto&& p : piccs) { if (nfcA.identify(p)) Serial.println(p.uidAsString().c_str()); }
    nfcA.deactivate();
  }
}
