// Phase 2: 13.56 MHz NFC / RFID. One section for three devices that share M5Stack's library:
//   U216 Unit NFC   (ST25R3916, Grove I2C 0x50)   read + emulate
//   U219 Cap NFC    (ST25R3916, EXT SPI CS G6)     read + emulate   (shares the SD SPI bus)
//   U031-B RFID2    (WS1850S,   Grove I2C 0x28)    read only (no card emulation on this chip)
//
// Reads NFC-A tags. NTAG / Ultralight are fully dumped and saved as Flipper ".nfc"; anything else
// detectable is saved UID-level (ISO14443-3A). NTAG / Ultralight can be emulated on the two
// ST25R3916 devices. MIFARE Classic full dump and magic-card writing come in a later phase.
#include "apps.h"
#include "grove.h"
#include "nfc_store.h"
#include "ui.h"

#include <M5UnitUnified.h>
#include <M5UnitUnifiedNFC.h>
#include <M5UnitUnifiedRFID.h>
#include <wiring/m5_unit_unified_wiring.hpp>
#include <new>

using m5::nfc::a::PICC;
using Atype = m5::nfc::a::Type;

// ------------------------------------------------------------------ devices
enum class Dev { None, U216, RFID2, Cap };

static m5::unit::UnitUnified s_units;
static m5::unit::UnitNFC s_unitNfc{};          // ST25R3916 over I2C (U216)
static m5::unit::CapCC1101NFC s_cap{};         // ST25R3916 over SPI (U219 Cap), CS G6 / IRQ G4
static m5::unit::UnitRFID2 s_rfid2{};          // WS1850S over I2C (U031-B)

static m5::nfc::NFCLayerA s_nfcU216{s_unitNfc};
static m5::nfc::NFCLayerA s_nfcCap{s_cap};
static m5::nfc::NFCLayerA s_nfcRfid{s_rfid2};

static Dev s_dev = Dev::None;                  // initialized device (persists until reboot)
static m5::nfc::NFCLayerA* s_nfc = nullptr;

static const char* devName(Dev d) {
  switch (d) { case Dev::U216: return "U216 Unit NFC"; case Dev::RFID2: return "U031-B RFID2";
               case Dev::Cap: return "U219 Cap NFC"; default: return "none"; }
}
static bool devEmulates(Dev d) { return d == Dev::U216 || d == Dev::Cap; }

// Bring up one device. One per boot: switching needs a restart (one Grove port; the Cap is on SPI).
static bool devBegin(Dev d) {
  bool ok = false;
  if (d == Dev::U216) {
    grove::release();
    ok = m5::unit::wiring::i2cWire(s_units, s_unitNfc, Wire, GROVE_YEL, GROVE_WHT, 400000) && s_units.begin();
    s_nfc = &s_nfcU216;
  } else if (d == Dev::RFID2) {
    grove::release();
    ok = m5::unit::wiring::i2cWire(s_units, s_rfid2, Wire, GROVE_YEL, GROVE_WHT, 400000) && s_units.begin();
    s_nfc = &s_nfcRfid;
  } else if (d == Dev::Cap) {
    ui::sdMount();                              // ensure the shared SD/SPI bus is up first
    ok = m5::unit::wiring::addSPI(s_units, s_cap, 10000000, 1) && s_units.begin();  // SPI mode 1
    s_nfc = &s_nfcCap;
  }
  if (ok) s_dev = d;
  return ok;
}

// ------------------------------------------------------------------ type mapping
struct NtagInfo { const char* flipper; uint8_t ver[8]; bool hasVer; bool isNtagUl; };

static NtagInfo ntagInfo(Atype t) {
  switch (t) {
    case Atype::NTAG_203:  return {"NTAG203", {}, false, true};
    case Atype::NTAG_210:  return {"NTAG210", {}, false, true};
    case Atype::NTAG_210u: return {"NTAG210u", {}, false, true};
    case Atype::NTAG_212:  return {"NTAG212", {}, false, true};
    case Atype::NTAG_213:  return {"NTAG213", {0x00,0x04,0x04,0x02,0x01,0x00,0x0F,0x03}, true, true};
    case Atype::NTAG_215:  return {"NTAG215", {0x00,0x04,0x04,0x02,0x01,0x00,0x11,0x03}, true, true};
    case Atype::NTAG_216:  return {"NTAG216", {0x00,0x04,0x04,0x02,0x01,0x00,0x13,0x03}, true, true};
    case Atype::MIFARE_Ultralight:       return {"Mifare Ultralight", {}, false, true};
    case Atype::MIFARE_Ultralight_EV1_1: return {"Mifare Ultralight 11", {0x00,0x04,0x03,0x01,0x01,0x00,0x0B,0x03}, true, true};
    case Atype::MIFARE_Ultralight_EV1_2: return {"Mifare Ultralight 21", {0x00,0x04,0x03,0x01,0x01,0x00,0x0E,0x03}, true, true};
    case Atype::MIFARE_UltralightC:      return {"Mifare Ultralight C", {}, false, true};
    case Atype::MIFARE_Ultralight_Nano:  return {"Mifare Ultralight Nano", {}, false, true};
    default: return {"Unknown", {}, false, false};
  }
}

// Reverse: Flipper type string -> emulation type (for emulating a loaded file)
static Atype typeFromFlipper(const String& s) {
  if (s == "NTAG213") return Atype::NTAG_213;
  if (s == "NTAG215") return Atype::NTAG_215;
  if (s == "NTAG216") return Atype::NTAG_216;
  if (s == "NTAG210") return Atype::NTAG_210;
  if (s == "NTAG212") return Atype::NTAG_212;
  if (s == "Mifare Ultralight") return Atype::MIFARE_Ultralight;
  if (s == "Mifare Ultralight 11") return Atype::MIFARE_Ultralight_EV1_1;
  if (s == "Mifare Ultralight 21") return Atype::MIFARE_Ultralight_EV1_2;
  if (s == "Mifare Ultralight C") return Atype::MIFARE_UltralightC;
  return Atype::Unknown;
}

static uint8_t bcc(const uint8_t* p, int n, uint8_t init = 0) {
  uint8_t v = init;
  for (int i = 0; i < n; i++) v ^= p[i];
  return v;
}
// Pages 0-2 of an NTAG/UL hold UID bytes 0-2, BCC0, UID bytes 3-6, BCC1 (7-byte UID)
static void embedUid(std::vector<uint8_t>& mem, const uint8_t* uid, uint8_t len) {
  if (len != 7 || mem.size() < 9) return;
  memcpy(&mem[0], uid, 3);
  mem[3] = bcc(uid, 3, 0x88);
  memcpy(&mem[4], uid + 3, 4);
  mem[8] = bcc(uid + 3, 4);
}

// ------------------------------------------------------------------ current card
static nfcstore::Card s_card;
static bool s_have = false;
static String s_info;          // one-line status under the card data

// Read a detected PICC: identify, then for NTAG/UL dump every page into s_card.
static bool readCard() {
  PICC picc{};
  if (!s_nfc->detect(picc)) return false;
  if (!s_nfc->identify(picc) || !s_nfc->reactivate(picc)) { s_info = "detected, could not read"; return false; }
  M5Cardputer.Speaker.tone(3000, 20);

  s_card = nfcstore::Card();
  s_card.uidLen = picc.size;
  memcpy(s_card.uid, picc.uid, picc.size);
  s_card.atqa = picc.atqa;
  s_card.sak = picc.sak;

  NtagInfo ni = ntagInfo(picc.type);
  if (ni.isNtagUl) {
    s_card.kind = nfcstore::Kind::NTAG_UL;
    s_card.typeStr = ni.flipper;
    s_card.hasVer = ni.hasVer;
    if (ni.hasVer) memcpy(s_card.ver, ni.ver, 8);
    uint16_t pages = picc.blocks;
    if (pages == 0 || pages > 231) pages = 231;
    s_card.pagesTotal = pages;
    s_card.mem.assign((size_t)pages * 4, 0);
    uint16_t read = 0;
    for (uint16_t p = 0; p < pages; p++) {
      uint8_t rx[4];
      if (s_nfc->read4(rx, (uint8_t)p)) { memcpy(&s_card.mem[p * 4], rx, 4); read++; }
    }
    s_card.pagesRead = read;
    s_info = String(ni.flipper) + ": " + read + "/" + pages + " pages";
  } else {
    s_card.kind = nfcstore::Kind::ISO14443_3A;
    s_card.typeStr = picc.typeAsString().c_str();
    s_info = s_card.typeStr + " (UID only)";
  }
  s_nfc->deactivate();
  s_have = true;
  return true;
}

// ------------------------------------------------------------------ emulate
static void emulate(const nfcstore::Card& c, Atype type) {
  if (!devEmulates(s_dev)) { ui::toast("Emulation needs U216 or the Cap", C_WARN, 1800); return; }
  if (c.kind != nfcstore::Kind::NTAG_UL || type == Atype::Unknown) {
    ui::toast("Only NTAG / Ultralight can be emulated", C_WARN, 1800);
    return;
  }
  // Configure the chip for NFC-A card emulation and (re)begin it
  if (s_dev == Dev::U216) { auto cf = s_unitNfc.config(); cf.emulation = true; cf.mode = m5::nfc::NFC::A; s_unitNfc.config(cf); }
  else { auto cf = s_cap.config(); cf.emulation = true; cf.mode = m5::nfc::NFC::A; s_cap.config(cf); }
  if (!s_units.begin()) { ui::toast("Emulation init failed", C_BAD, 1800); return; }

  PICC picc{};
  if (!picc.emulate(type, c.uid, c.uidLen)) { ui::toast("Bad UID for emulation", C_BAD); return; }

  std::vector<uint8_t> mem = c.mem;            // local copy; embed UID so readers see it
  size_t need = (size_t)picc.blocks * 4;
  if (mem.size() < need) mem.resize(need, 0);
  embedUid(mem, c.uid, c.uidLen);

  m5::nfc::EmulationLayerA* emu = nullptr;
  if (s_dev == Dev::U216) emu = new (std::nothrow) m5::nfc::EmulationLayerA(s_unitNfc);
  else emu = new (std::nothrow) m5::nfc::EmulationLayerA(s_cap);
  if (!emu || !emu->begin(picc, mem.data(), mem.size())) {
    ui::toast("Emulation begin failed", C_BAD, 1800);
    delete emu;
    // restore reader mode
    if (s_dev == Dev::U216) { auto cf = s_unitNfc.config(); cf.emulation = false; s_unitNfc.config(cf); }
    else { auto cf = s_cap.config(); cf.emulation = false; s_cap.config(cf); }
    s_units.begin();
    return;
  }

  using State = m5::nfc::EmulationLayerA::State;
  bool dirty = true;
  State last = State::None;
  uint32_t reads = 0;
  while (true) {
    s_units.update();
    emu->update();
    State st = emu->state();
    if (st != last) { last = st; reads += (st == State::Active); dirty = true; }
    ui::Key k = ui::poll();
    if (k.back) break;
    if (k.any) dirty = true;
    if (!dirty) { delay(5); continue; }
    dirty = false;
    ui::stayAwake();
    ui::clear();
    ui::header("NFC Emulate");
    int y = TOP + 2;
    cv.setTextColor(C_ACC, C_BG); cv.setTextSize(2); cv.setCursor(6, y); cv.print(c.typeStr); cv.setTextSize(1);
    y += 20;
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(6, y); cv.print("UID");
    cv.setTextColor(C_FG, C_BG); cv.setCursor(40, y); cv.print(c.uidString());
    y += 12;
    const char* sTxt = st == State::Active ? "reader talking" : st == State::Halt ? "halted" :
                       st == State::Ready ? "selected" : st == State::Idle ? "field present" :
                       st == State::Off ? "waiting for a reader" : "-";
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(6, y); cv.print("State");
    cv.setTextColor(st == State::Active ? C_OK : C_FG, C_BG); cv.setCursor(40, y); cv.print(sTxt);
    y += 12;
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(6, y); cv.printf("Selected %lu time(s)", (unsigned long)reads);
    y += 16;
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(6, y); cv.print("Hold near a reader or phone.");
    cv.setCursor(6, y + 10); cv.printf("On %s via the ST25R3916.", devName(s_dev));
    ui::footer("` back to reader mode");
    ui::push();
  }
  emu->end();
  delete emu;
  // back to reader mode
  if (s_dev == Dev::U216) { auto cf = s_unitNfc.config(); cf.emulation = false; cf.mode = m5::nfc::NFC::A; s_unitNfc.config(cf); }
  else { auto cf = s_cap.config(); cf.emulation = false; cf.mode = m5::nfc::NFC::A; s_cap.config(cf); }
  s_units.begin();
}

// ------------------------------------------------------------------ screens
static void saveCurrent() {
  if (!s_have) { ui::toast("Read a card first", C_WARN); return; }
  String name = s_card.kind == nfcstore::Kind::NTAG_UL ? s_card.typeStr : String("card");
  name.replace(" ", "_");
  if (!ui::input("Card name", name, 24) || name.isEmpty()) return;
  name.replace(" ", "_");
  String path = nfcstore::save(s_card, name);
  if (path.length()) ui::toast("Saved " + path.substring(path.lastIndexOf('/') + 1));
}

static void screenRead() {
  s_have = false;
  s_info = "Hold a tag to the antenna";
  bool dirty = true;
  uint32_t lastPoll = 0;
  while (true) {
    s_units.update();
    if (millis() - lastPoll > 200) {
      lastPoll = millis();
      if (readCard()) dirty = true;
    }
    ui::Key k = ui::poll();
    if (k.back) break;
    if (k.any) dirty = true;
    if (k.lc == 's') { saveCurrent(); dirty = true; }
    if (k.lc == 'e' && s_have) { emulate(s_card, typeFromFlipper(s_card.typeStr)); dirty = true; }
    if (!dirty) { delay(10); continue; }
    dirty = false;
    ui::stayAwake();
    ui::clear();
    char hdr[28];
    snprintf(hdr, sizeof(hdr), "NFC Read: %s", s_dev == Dev::U216 ? "U216" : s_dev == Dev::Cap ? "Cap" : "RFID2");
    ui::header(hdr);
    int y = TOP + 2;
    if (!s_have) {
      cv.setTextColor(C_DIM, C_BG); cv.setCursor(6, y + 24); cv.print(s_info);
      cv.setCursor(6, y + 40); cv.print(devEmulates(s_dev) ? "S save   E emulate" : "S save");
    } else {
      cv.setTextColor(C_ACC, C_BG); cv.setTextSize(2); cv.setCursor(6, y); cv.print(s_card.typeStr); cv.setTextSize(1);
      y += 20;
      cv.setTextColor(C_DIM, C_BG); cv.setCursor(6, y); cv.print("UID");
      cv.setTextColor(C_FG, C_BG); cv.setCursor(40, y); cv.print(s_card.uidString());
      y += 11;
      cv.setTextColor(C_DIM, C_BG); cv.setCursor(6, y); cv.printf("ATQA %04X   SAK %02X", s_card.atqa, s_card.sak);
      y += 11;
      cv.setTextColor(C_FG, C_BG); cv.setCursor(6, y); cv.print(s_info);
      y += 14;
      cv.setTextColor(C_DIM, C_BG); cv.setCursor(6, y);
      cv.print(devEmulates(s_dev) && s_card.kind == nfcstore::Kind::NTAG_UL ? "S save   E emulate   re-tap to re-read"
                                                                           : "S save   re-tap to re-read");
    }
    ui::footer("S save  E emulate  ` back");
    ui::push();
  }
}

static void screenSaved() {
  std::vector<String> names = nfcstore::list();
  if (names.empty()) { ui::toast("No .nfc files in /UnitLab/nfc", C_WARN, 1500); return; }
  std::vector<const char*> items;
  for (auto& n : names) items.push_back(n.c_str());
  int i = ui::popup("Saved cards", items.data(), items.size());
  if (i < 0) return;
  nfcstore::Card c;
  String path = "/UnitLab/nfc/" + names[i] + ".nfc";
  if (!nfcstore::load(path, c)) { ui::toast("Could not read file", C_BAD); return; }

  bool dirty = true;
  while (true) {
    ui::Key k = ui::poll();
    if (k.back) break;
    if (k.any) dirty = true;
    if (k.lc == 'e') { emulate(c, typeFromFlipper(c.typeStr)); dirty = true; }
    if (!dirty) { delay(10); continue; }
    dirty = false;
    ui::clear();
    ui::header(names[i].c_str());
    int y = TOP + 2;
    cv.setTextColor(C_ACC, C_BG); cv.setCursor(6, y); cv.print(c.typeStr.length() ? c.typeStr : String("ISO14443-3A"));
    y += 12;
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(6, y); cv.print("UID");
    cv.setTextColor(C_FG, C_BG); cv.setCursor(40, y); cv.print(c.uidString());
    y += 11;
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(6, y); cv.printf("ATQA %04X  SAK %02X", c.atqa, c.sak);
    y += 11;
    if (c.kind == nfcstore::Kind::NTAG_UL)
      { cv.setCursor(6, y); cv.printf("%u pages stored", (unsigned)(c.mem.size() / 4)); y += 11; }
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(6, y + 6);
    cv.print(c.kind == nfcstore::Kind::NTAG_UL ? "E emulate (U216 or Cap)" : "UID-only file: cannot emulate");
    ui::footer("E emulate   ` back");
    ui::push();
  }
}

// ------------------------------------------------------------------ device picker
static bool pickDevice() {
  // Pre-detect on the Grove port so the likely device is the default
  Dev suggest = Dev::None;
  if (grove::i2cSafe() && grove::i2cBegin(400000)) {
    if (grove::i2cPresent(0x50)) suggest = Dev::U216;
    else if (grove::i2cPresent(0x28)) suggest = Dev::RFID2;
    grove::release();
  }
  const char* items[] = {"U216 Unit NFC (Grove)", "U031-B RFID2 (Grove)", "U219 Cap NFC (SPI)"};
  // popup() returns an index; default the cursor is 0, so order so the suggestion is visible in text
  ui::toast(suggest == Dev::U216 ? "Found U216 on Grove" : suggest == Dev::RFID2 ? "Found RFID2 on Grove"
                                                                                 : "Nothing on Grove: Cap?",
            suggest == Dev::None ? C_WARN : C_OK, 1200);
  int i = ui::popup("Choose device", items, 3);
  if (i < 0) return false;
  Dev chosen = i == 0 ? Dev::U216 : i == 1 ? Dev::RFID2 : Dev::Cap;
  ui::clear(); ui::header("NFC / RFID");
  cv.setTextColor(C_FG, C_BG); cv.setCursor(6, TOP + 30); cv.printf("Starting %s...", devName(chosen));
  ui::push();
  if (!devBegin(chosen)) { ui::toast(String("Could not start ") + devName(chosen), C_BAD, 2200); return false; }
  return true;
}

void appNfc() {
  if (s_dev == Dev::None) {
    if (!pickDevice()) return;
  }
  while (true) {
    char line[32];
    snprintf(line, sizeof(line), "Device: %s", devName(s_dev));
    const char* items[] = {"Read a tag", "Saved cards", line};
    int i = ui::popup("NFC / RFID", items, 3);
    if (i < 0) break;
    if (i == 0) screenRead();
    else if (i == 1) screenSaved();
    else ui::toast("Switch device: restart UnitLab", C_WARN, 1800);
  }
}
