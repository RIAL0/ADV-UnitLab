// Flipper Zero ".nfc" file format (firmware format Version 4), read and write.
// Phase 2 supports NFC-A: NTAG/Ultralight (full page dump) and a UID-level ISO14443-3A fallback
// for anything the firmware can detect but not fully read (e.g. MIFARE Classic without keys).
// Files live in /UnitLab/nfc/ so they move freely between the Cardputer and a Flipper.
#pragma once
#include <Arduino.h>
#include <vector>

namespace nfcstore {

// Device-type line in the .nfc file.
enum class Kind { ISO14443_3A, NTAG_UL };

struct Card {
  Kind kind = Kind::ISO14443_3A;
  uint8_t uid[10] = {0};
  uint8_t uidLen = 0;
  uint16_t atqa = 0;            // stored low byte first on the wire, printed high byte first
  uint8_t sak = 0;
  String typeStr;              // Flipper "NTAG/Ultralight type", e.g. "NTAG215"
  uint8_t ver[8] = {0};        // GET_VERSION response (NTAG/UL)
  bool hasVer = false;
  uint16_t pagesTotal = 0;
  uint16_t pagesRead = 0;
  std::vector<uint8_t> mem;    // page memory, 4 bytes per page

  String uidString() const;    // "04 A2 2C ..."
};

// Write a Flipper-compatible .nfc file. Returns the full path, or "" on failure.
String save(const Card& c, const String& name);

// Read a .nfc file. Fills c; returns false if the file can't be parsed as NFC-A.
bool load(const String& path, Card& c);

// List *.nfc base names in /UnitLab/nfc (sorted).
std::vector<String> list();

}  // namespace nfcstore
