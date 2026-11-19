#include "nfc_store.h"
#include "ui.h"
#include <SD.h>
#include <algorithm>

namespace nfcstore {

static const char* NFC_DIR = "/UnitLab/nfc";

static String hexBytes(const uint8_t* d, size_t n) {
  String s;
  char b[4];
  for (size_t i = 0; i < n; i++) {
    snprintf(b, sizeof(b), "%02X", d[i]);
    if (i) s += ' ';
    s += b;
  }
  return s;
}

String Card::uidString() const { return hexBytes(uid, uidLen); }

// Parse "04 A2 2C .." into out (up to maxOut bytes). Returns the count.
static int parseHex(const String& v, uint8_t* out, int maxOut) {
  int n = 0;
  const char* p = v.c_str();
  while (*p && n < maxOut) {
    while (*p == ' ') p++;
    if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1])) break;
    char h[3] = {p[0], p[1], 0};
    out[n++] = (uint8_t)strtoul(h, nullptr, 16);
    p += 2;
  }
  return n;
}

String save(const Card& c, const String& name) {
  if (!ui::ensureDir("nfc")) { ui::toast("No SD card", C_BAD); return ""; }
  String path = String(NFC_DIR) + "/" + name + ".nfc";
  // Avoid clobbering an existing file: append _NNNN if needed
  if (SD.exists(path)) path = ui::nextFile("nfc", name.c_str(), "nfc");
  File f = SD.open(path, FILE_WRITE);
  if (!f) { ui::toast("SD write failed", C_BAD); return ""; }

  f.print("Filetype: Flipper NFC device\n");
  f.print("Version: 4\n");
  f.print("# Written by ADV-UnitLab\n");
  f.printf("Device type: %s\n", c.kind == Kind::NTAG_UL ? "NTAG/Ultralight" : "ISO14443-3A");
  f.print("# UID is common for all formats\n");
  f.printf("UID: %s\n", c.uidString().c_str());
  // ATQA is stored low byte first in Flipper files
  f.printf("ATQA: %02X %02X\n", (unsigned)(c.atqa & 0xFF), (unsigned)((c.atqa >> 8) & 0xFF));
  f.printf("SAK: %02X\n", (unsigned)c.sak);

  if (c.kind == Kind::NTAG_UL) {
    f.print("Data format version: 2\n");
    f.printf("NTAG/Ultralight type: %s\n", c.typeStr.c_str());
    uint8_t zsig[32] = {0};
    f.printf("Signature: %s\n", hexBytes(zsig, 32).c_str());
    uint8_t ver[8] = {0};
    if (c.hasVer) memcpy(ver, c.ver, 8);
    f.printf("Mifare version: %s\n", hexBytes(ver, 8).c_str());
    for (int i = 0; i < 3; i++) { f.printf("Counter %d: 0\n", i); f.printf("Tearing %d: 00\n", i); }
    f.printf("Pages total: %u\n", (unsigned)c.pagesTotal);
    f.printf("Pages read: %u\n", (unsigned)c.pagesRead);
    uint16_t pages = c.mem.size() / 4;
    for (uint16_t p = 0; p < pages; p++)
      f.printf("Page %u: %s\n", (unsigned)p, hexBytes(&c.mem[p * 4], 4).c_str());
    f.print("Failed authentication attempts: 0\n");
  }
  f.close();
  return path;
}

bool load(const String& path, Card& c) {
  File f = SD.open(path);
  if (!f) return false;
  c = Card();
  bool isNfc = false;
  std::vector<std::pair<uint16_t, std::array<uint8_t, 4>>> pages;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.isEmpty() || line.startsWith("#")) continue;
    int colon = line.indexOf(':');
    if (colon < 0) continue;
    String key = line.substring(0, colon), val = line.substring(colon + 1);
    val.trim();
    if (key == "Filetype") isNfc = val.indexOf("Flipper NFC") >= 0;
    else if (key == "Device type") c.kind = (val.indexOf("NTAG") >= 0 || val.indexOf("Ultralight") >= 0)
                                              ? Kind::NTAG_UL : Kind::ISO14443_3A;
    else if (key == "UID") c.uidLen = parseHex(val, c.uid, 10);
    else if (key == "ATQA") { uint8_t a[2] = {0}; if (parseHex(val, a, 2) == 2) c.atqa = a[0] | (a[1] << 8); }
    else if (key == "SAK") { uint8_t s = 0; parseHex(val, &s, 1); c.sak = s; }
    else if (key == "NTAG/Ultralight type") c.typeStr = val;
    else if (key == "Mifare version") { c.hasVer = parseHex(val, c.ver, 8) == 8; }
    else if (key == "Pages total") c.pagesTotal = val.toInt();
    else if (key == "Pages read") c.pagesRead = val.toInt();
    else if (key.startsWith("Page ")) {
      uint16_t idx = key.substring(5).toInt();
      std::array<uint8_t, 4> pg{};
      if (parseHex(val, pg.data(), 4) == 4) pages.push_back({idx, pg});
    }
  }
  f.close();
  if (!isNfc) return false;
  if (!pages.empty()) {
    uint16_t maxIdx = 0;
    for (auto& pr : pages) maxIdx = max(maxIdx, pr.first);
    c.mem.assign((maxIdx + 1) * 4, 0);
    for (auto& pr : pages) memcpy(&c.mem[pr.first * 4], pr.second.data(), 4);
  }
  return c.uidLen == 4 || c.uidLen == 7 || c.uidLen == 10;
}

std::vector<String> list() {
  std::vector<String> names;
  if (!ui::sdMount()) return names;
  File dir = SD.open(NFC_DIR);
  if (!dir) return names;
  for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
    String n = e.name();
    n = n.substring(n.lastIndexOf('/') + 1);
    if (!e.isDirectory() && n.endsWith(".nfc")) names.push_back(n.substring(0, n.length() - 4));
    e.close();
  }
  dir.close();
  std::sort(names.begin(), names.end());
  return names;
}

}  // namespace nfcstore
