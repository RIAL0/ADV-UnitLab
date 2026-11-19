// U002 IR unit (receiver + LED on Grove) and the Cardputer's own IR LED (GPIO44).
// Capture decodes with Arduino-IRremote and always keeps the raw timings as well. Signals are saved
// in Flipper Zero ".ir" format (parsed where Flipper has the protocol, raw otherwise), so files move
// freely between the Cardputer, a Flipper and Bruce.
#include "apps.h"
#include "grove.h"
#include "settings.h"
#include "ui.h"
#include <algorithm>

#define RAW_BUFFER_LENGTH 750        // air-conditioner remotes need long frames
#define USE_16_BIT_TIMING_BUFFER     // keep raw timings above 12.75 ms exact
#define SEND_PWM_BY_TIMER
#include <IRremote.hpp>

static const int IR_LED_BUILTIN = 44;
static const char* IR_DIR = "/UnitLab/ir";

// ------------------------------------------------------------------ signal model
struct IrSignal {
  String name;
  bool parsed = false;
  String proto;                      // Flipper protocol name when parsed
  uint32_t addr = 0, cmd = 0;
  uint32_t freq = 38000;
  float duty = 0.33f;
  std::vector<uint16_t> raw;         // mark, space, mark, ... in microseconds
};

// IRremote decode -> Flipper protocol. Returns false when Flipper has no matching parsed form.
static bool toFlipper(const IRData& d, String& proto, uint32_t& addr, uint32_t& cmd) {
  switch (d.protocol) {
    case NEC:
    case NEC2:
      if (d.address <= 0xFF && d.command <= 0xFF) { proto = "NEC"; addr = d.address; cmd = d.command; return true; }
      if (d.command <= 0xFF) { proto = "NECext"; addr = d.address; cmd = d.command | ((uint32_t)(~d.command & 0xFF) << 8); return true; }
      return false;
    case SAMSUNG:
      if ((d.address >> 8) == (d.address & 0xFF) && d.command <= 0xFF) {
        proto = "Samsung32"; addr = d.address & 0xFF; cmd = d.command; return true;
      }
      return false;
    case SONY:
      proto = d.numberOfBits == 20 ? "SIRC20" : d.numberOfBits == 15 ? "SIRC15" : "SIRC";
      addr = d.address; cmd = d.command; return true;
    case RC5:
      if (d.command < 64) { proto = "RC5"; addr = d.address; cmd = d.command; return true; }
      return false;
    case RC6:
      proto = "RC6"; addr = d.address; cmd = d.command; return true;
    default:
      return false;
  }
}

static String leBytes(uint32_t v) {
  char b[16];
  snprintf(b, sizeof(b), "%02X %02X %02X %02X", (unsigned)(v & 0xFF), (unsigned)((v >> 8) & 0xFF),
           (unsigned)((v >> 16) & 0xFF), (unsigned)((v >> 24) & 0xFF));
  return b;
}
// "07 BF 00 00" -> 0x0000BF07 (Flipper stores multi-byte fields little-endian)
static uint32_t parseLeBytes(const String& s) {
  uint32_t v = 0;
  int shift = 0;
  const char* p = s.c_str();
  while (*p && shift < 32) {
    char* end;
    unsigned long b = strtoul(p, &end, 16);
    if (end == p) break;
    v |= (uint32_t)(b & 0xFF) << shift;
    shift += 8;
    p = end;
  }
  return v;
}

// ------------------------------------------------------------------ Flipper .ir files
static bool appendSignal(const String& path, const IrSignal& s) {
  bool fresh = !SD.exists(path);
  File f = SD.open(path, FILE_APPEND);
  if (!f) return false;
  if (fresh) f.print("Filetype: IR signals file\nVersion: 1\n");
  f.print("#\n");
  f.printf("name: %s\n", s.name.c_str());
  if (s.parsed) {
    f.printf("type: parsed\nprotocol: %s\naddress: %s\ncommand: %s\n", s.proto.c_str(), leBytes(s.addr).c_str(),
             leBytes(s.cmd).c_str());
  } else {
    f.printf("type: raw\nfrequency: %lu\nduty_cycle: %.6f\ndata:", (unsigned long)s.freq, s.duty);
    for (uint16_t t : s.raw) f.printf(" %u", t);
    f.print("\n");
  }
  f.close();
  return true;
}

static bool loadFile(const String& path, std::vector<IrSignal>& out) {
  File f = SD.open(path);
  if (!f) return false;
  out.clear();
  IrSignal cur;
  bool have = false;
  auto flush = [&]() { if (have && cur.name.length()) out.push_back(cur); cur = IrSignal(); have = false; };
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.startsWith("#")) { flush(); continue; }
    int colon = line.indexOf(':');
    if (colon < 0) continue;
    String key = line.substring(0, colon), val = line.substring(colon + 1);
    val.trim();
    if (key == "name") { flush(); cur.name = val; have = true; }
    else if (key == "type") cur.parsed = val == "parsed";
    else if (key == "protocol") cur.proto = val;
    else if (key == "address") cur.addr = parseLeBytes(val);
    else if (key == "command") cur.cmd = parseLeBytes(val);
    else if (key == "frequency") cur.freq = val.toInt();
    else if (key == "duty_cycle") cur.duty = val.toFloat();
    else if (key == "data") {
      // Long raw signals may continue on further "data:" lines
      const char* p = val.c_str();
      while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        char* end;
        long v = strtol(p, &end, 10);
        if (end == p) break;
        if (v > 0 && v < 65536 && cur.raw.size() < 2048) cur.raw.push_back((uint16_t)v);
        p = end;
      }
    }
  }
  flush();
  f.close();
  return true;
}

// ------------------------------------------------------------------ transmit
static int s_txPin = IR_LED_BUILTIN;
static int s_rxPin = -1;
static bool s_rxOn = false;

static void rxStart() { if (s_rxPin >= 0) { IrReceiver.begin(s_rxPin, DISABLE_LED_FEEDBACK); s_rxOn = true; } }
static void rxStop() { if (s_rxOn) { IrReceiver.stop(); s_rxOn = false; } }

static String sendSignal(const IrSignal& s) {
  bool wasRx = s_rxOn;
  rxStop();                                    // do not capture our own transmission
  IrSender.setSendPin(s_txPin);
  String err;
  if (!s.parsed) {
    if (s.raw.empty()) err = "empty raw signal";
    else IrSender.sendRaw(s.raw.data(), s.raw.size(), (uint8_t)constrain((int)(s.freq / 1000), 30, 60));
  } else if (s.proto == "NEC") {
    IrSender.sendNEC(s.addr & 0xFF, s.cmd & 0xFF, 0);
  } else if (s.proto == "NECext") {
    uint16_t c = s.cmd & 0xFFFF;
    bool inverted = ((c >> 8) & 0xFF) == (uint8_t)~(c & 0xFF);
    IrSender.sendNEC(s.addr & 0xFFFF, inverted ? (c & 0xFF) : c, 0);
  } else if (s.proto == "Samsung32") {
    IrSender.sendSamsung(s.addr & 0xFF, s.cmd & 0xFF, 0);
  } else if (s.proto == "SIRC" || s.proto == "SIRC15" || s.proto == "SIRC20") {
    uint8_t bits = s.proto == "SIRC20" ? 20 : s.proto == "SIRC15" ? 15 : 12;
    IrSender.sendSony(s.addr, s.cmd & 0x7F, 2, bits);          // Sony receivers expect 3 frames
  } else if (s.proto == "RC5" || s.proto == "RC5X") {
    IrSender.sendRC5(s.addr & 0x1F, s.cmd & 0x7F, 0, true);
  } else if (s.proto == "RC6") {
    IrSender.sendRC6(s.addr & 0xFF, s.cmd & 0xFF, 0, true);
  } else {
    err = "protocol " + s.proto + " not supported yet";
  }
  if (wasRx) { delay(40); rxStart(); }
  return err;
}

// ------------------------------------------------------------------ capture
struct Capture { IrSignal sig; String decoded; uint8_t bits = 0; uint32_t at = 0; };
static std::vector<Capture> s_caps;
static String s_saveFile;
static uint32_t s_repeats = 0, s_overflows = 0;

static bool grab(Capture& c) {
  if (!IrReceiver.decode()) return false;
  IRData& d = IrReceiver.decodedIRData;
  if (d.flags & IRDATA_FLAGS_IS_REPEAT) { s_repeats++; IrReceiver.resume(); return false; }
  if (d.flags & IRDATA_FLAGS_WAS_OVERFLOW) { s_overflows++; IrReceiver.resume(); return false; }
  // Raw timings: rawbuf[1..rawlen-1] in 50 us ticks; IRremote's receiver lengthens marks slightly
  auto& p = IrReceiver.irparams;
  c.sig.raw.clear();
  for (unsigned i = 1; i < (unsigned)p.rawlen; i++) {
    int us = (int)p.rawbuf[i] * MICROS_PER_TICK;
    us += (i & 1) ? -MARK_EXCESS_MICROS : MARK_EXCESS_MICROS;
    c.sig.raw.push_back((uint16_t)constrain(us, 1, 65535));
  }
  c.bits = d.numberOfBits;
  c.at = millis();
  char b[64];
  snprintf(b, sizeof(b), "%s A:0x%X C:0x%X %ub", getProtocolString(d.protocol), d.address, d.command, d.numberOfBits);
  c.decoded = b;
  String proto;
  uint32_t a = 0, cm = 0;
  if (toFlipper(d, proto, a, cm)) { c.sig.parsed = true; c.sig.proto = proto; c.sig.addr = a; c.sig.cmd = cm; }
  else c.sig.parsed = false;
  c.sig.freq = 38000;
  IrReceiver.resume();
  return true;
}

static void saveCapture(Capture& c) {
  if (!ui::sdMount() || !ui::ensureDir("ir")) { ui::toast("No SD card", C_BAD); return; }
  if (s_saveFile.isEmpty()) s_saveFile = ui::nextFile("ir", "remote", "ir");
  String name = "Button_" + String(s_caps.size());
  if (!ui::input("Button name", name, 24) || name.isEmpty()) return;
  name.replace(" ", "_");
  c.sig.name = name;
  if (appendSignal(s_saveFile, c.sig)) ui::toast("Saved to " + s_saveFile.substring(s_saveFile.lastIndexOf('/') + 1));
  else ui::toast("SD write failed", C_BAD);
}

static void screenCapture() {
  if (s_rxPin < 0) { ui::toast("No IR receiver found on Grove", C_BAD, 1800); return; }
  rxStart();
  int sel = -1;     // -1 = follow the newest capture
  bool dirty = true;
  bool showRaw = false;
  while (true) {
    Capture c;
    if (grab(c)) {
      s_caps.push_back(c);
      if (s_caps.size() > 30) s_caps.erase(s_caps.begin());
      sel = -1;
      M5Cardputer.Speaker.tone(3500, 30);
      dirty = true;
    }
    ui::Key k = ui::poll();
    if (k.back) break;
    if (k.any) dirty = true;
    int n = s_caps.size();
    int cur = sel < 0 ? n - 1 : sel;
    if (k.up && n) sel = max(0, cur - 1);
    if (k.down && n) sel = cur + 1 >= n ? -1 : cur + 1;
    if (k.lc == 'r') showRaw = !showRaw;
    if (k.lc == 'n') { s_saveFile = ""; ui::toast("Next save starts a new file"); }
    if (n && cur >= 0) {
      if (k.enter) {
        String e = sendSignal(s_caps[cur].sig);
        ui::toast(e.isEmpty() ? "Sent" : e, e.isEmpty() ? C_OK : C_BAD, 600);
      }
      if (k.lc == 's') saveCapture(s_caps[cur]);
      if (k.lc == 'p' && s_caps[cur].sig.parsed) {
        s_caps[cur].sig.parsed = false;            // force raw for odd devices that need exact timing
        ui::toast("Will save as raw");
      }
    }
    if (!dirty) { delay(5); continue; }
    dirty = false;
    ui::clear();
    char hdr[32];
    snprintf(hdr, sizeof(hdr), "IR Capture  RX G%d", s_rxPin);
    ui::header(hdr);
    int y = TOP + 1;
    if (!n) {
      cv.setTextColor(C_FG, C_BG); cv.setCursor(6, y + 26); cv.print("Point a remote at the IR unit");
      cv.setCursor(6, y + 36); cv.print("and press a button.");
      cv.setTextColor(C_DIM, C_BG); cv.setCursor(6, y + 56);
      cv.printf("repeats %lu  overflows %lu", (unsigned long)s_repeats, (unsigned long)s_overflows);
    } else {
      const Capture& c = s_caps[cur];
      cv.setTextColor(C_YEL, C_BG); cv.setCursor(4, y); cv.printf("#%d of %d", cur + 1, n);
      cv.setTextColor(c.sig.parsed ? C_OK : C_WARN, C_BG); cv.setCursor(70, y);
      if (c.sig.parsed) cv.printf("Flipper: %s", c.sig.proto.c_str()); else cv.print("saves as RAW");
      y += 11;
      cv.setTextColor(C_FG, C_BG); cv.setCursor(4, y); cv.print(c.decoded);
      y += 10;
      if (c.sig.parsed) {
        cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, y);
        cv.printf("addr %s", leBytes(c.sig.addr).c_str()); y += 9;
        cv.setCursor(4, y); cv.printf("cmd  %s", leBytes(c.sig.cmd).c_str()); y += 10;
      }
      cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, y);
      uint32_t total = 0;
      for (uint16_t t : c.sig.raw) total += t;
      cv.printf("%u timings, %.1f ms", (unsigned)c.sig.raw.size(), total / 1000.0f);
      y += 11;
      if (showRaw) {
        String line;
        for (size_t i = 0; i < c.sig.raw.size() && y < 112; i++) {
          String t = String(c.sig.raw[i]) + " ";
          if (line.length() + t.length() > 39) { cv.setCursor(4, y); cv.print(line); line = ""; y += 9; }
          line += t;
        }
        if (y < 112 && line.length()) { cv.setCursor(4, y); cv.print(line); }
      } else {
        // waveform strip of the first ~60 ms
        int x = 4, base = y + 14;
        bool mark = true;
        cv.drawFastHLine(4, base, 232, C_SEL);
        for (size_t i = 0; i < c.sig.raw.size() && x < 236; i++) {
          int w = max(1, (int)(c.sig.raw[i] / 250));
          if (mark) cv.fillRect(x, base - 10, min(w, 236 - x), 10, C_ACC);
          x += w;
          mark = !mark;
        }
        y = base + 4;
      }
      if (!s_saveFile.isEmpty()) {
        cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, 113);
        cv.print("-> " + s_saveFile.substring(s_saveFile.lastIndexOf('/') + 1));
      }
    }
    ui::footer("ENTER send S save R raw ;/. hist");
    ui::push();
  }
  rxStop();
}

// ------------------------------------------------------------------ saved remotes
static String pickIrFile() {
  if (!ui::sdMount()) { ui::toast("No SD card", C_BAD); return ""; }
  std::vector<String> names;
  File dir = SD.open(IR_DIR);
  if (dir) {
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      String n = f.name();
      if (!f.isDirectory() && n.endsWith(".ir")) names.push_back(n.substring(n.lastIndexOf('/') + 1));
      f.close();
    }
    dir.close();
  }
  if (names.empty()) { ui::toast("No .ir files in /UnitLab/ir", C_WARN, 1500); return ""; }
  std::sort(names.begin(), names.end());
  std::vector<const char*> items;
  for (auto& n : names) items.push_back(n.c_str());
  int i = ui::popup("Remote file", items.data(), items.size());
  return i < 0 ? String("") : String(IR_DIR) + "/" + names[i];
}

static void screenRemotes() {
  String path = pickIrFile();
  if (path.isEmpty()) return;
  std::vector<IrSignal> sigs;
  if (!loadFile(path, sigs) || sigs.empty()) { ui::toast("No signals in file", C_BAD); return; }
  int sel = 0, top = 0;
  bool dirty = true;
  String title = path.substring(path.lastIndexOf('/') + 1);
  while (true) {
    ui::Key k = ui::poll();
    if (k.back) break;
    int n = sigs.size();
    if (k.up) { sel = (sel + n - 1) % n; dirty = true; }
    if (k.down) { sel = (sel + 1) % n; dirty = true; }
    if (k.enter || k.space) {
      String e = sendSignal(sigs[sel]);
      ui::toast(e.isEmpty() ? "Sent " + sigs[sel].name : e, e.isEmpty() ? C_OK : C_BAD, 500);
      dirty = true;
    }
    if (!dirty) { delay(10); continue; }
    dirty = false;
    const int rows = ROWS - 1;
    if (sel < top) top = sel;
    if (sel >= top + rows) top = sel - rows + 1;
    ui::clear();
    char pos[12];
    snprintf(pos, sizeof(pos), "%d/%d", sel + 1, n);
    ui::header(title.c_str(), pos);
    for (int r = 0; r < rows && top + r < n; r++) {
      const IrSignal& s = sigs[top + r];
      int y = TOP + r * LINE_H;
      bool on = top + r == sel;
      if (on) cv.fillRect(0, y - 1, SCR_W, LINE_H, C_SEL);
      cv.setTextColor(on ? C_FG : C_DIM, on ? C_SEL : C_BG);
      cv.setCursor(4, y); cv.print(s.name.substring(0, 20));
      cv.setCursor(130, y);
      if (s.parsed) cv.printf("%s %02X/%02X", s.proto.c_str(), (unsigned)(s.addr & 0xFF), (unsigned)(s.cmd & 0xFF));
      else cv.printf("raw %u", (unsigned)s.raw.size());
    }
    ui::footer(s_txPin == IR_LED_BUILTIN ? "ENTER send  TX: built-in G44" : "ENTER send  TX: IR unit LED");
    ui::push();
  }
}

// ------------------------------------------------------------------ manual sender (from SensorLab)
static void screenManual() {
  static const char* protos[] = {"NEC", "Samsung32", "SIRC", "RC5"};
  int proto = 0, field = 0;
  uint16_t addr = 0x00, cmd = 0x45;
  bool dirty = true;
  while (true) {
    ui::Key k = ui::poll();
    if (k.back) break;
    if (k.any) dirty = true;
    if (k.lc == 'p') proto = (proto + 1) % 4;
    if (k.left || k.right || k.tab) field ^= 1;
    if (k.up || k.down) {
      int step = k.up ? 1 : -1;
      if (field == 0) addr = (addr + step) & 0xFF; else cmd = (cmd + step) & 0xFF;
    }
    if (k.enter || k.space) {
      IrSignal s;
      s.parsed = true; s.proto = protos[proto]; s.addr = addr; s.cmd = cmd;
      String e = sendSignal(s);
      if (!e.isEmpty()) ui::toast(e, C_BAD);
    }
    if (!dirty) { delay(15); continue; }
    dirty = false;
    ui::clear();
    ui::header("IR Manual send");
    int y = TOP + 6;
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, y); cv.print("Protocol");
    cv.setTextColor(C_FG, C_BG); cv.setCursor(80, y); cv.print(protos[proto]);
    y += 14;
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, y); cv.print("Address");
    cv.setTextColor(field == 0 ? C_YEL : C_FG, field == 0 ? C_SEL : C_BG); cv.setCursor(80, y); cv.printf(" 0x%02X (%3d) ", addr, addr);
    y += 12;
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, y); cv.print("Command");
    cv.setTextColor(field == 1 ? C_YEL : C_FG, field == 1 ? C_SEL : C_BG); cv.setCursor(80, y); cv.printf(" 0x%02X (%3d) ", cmd, cmd);
    y += 16;
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, y);
    cv.printf("TX on G%d %s", s_txPin, s_txPin == IR_LED_BUILTIN ? "(built-in LED)" : "(IR unit LED)");
    ui::footer("ENTER send  P protocol  ,// ;/. edit");
    ui::push();
  }
}

// ------------------------------------------------------------------ section
void appIr() {
  s_rxPin = grove::irRxPin();
  IrSender.begin(DISABLE_LED_FEEDBACK);
  IrSender.setSendPin(s_txPin);
  while (true) {
    char txLine[32], rxLine[32];
    snprintf(txLine, sizeof(txLine), "TX: %s", s_txPin == IR_LED_BUILTIN ? "built-in G44" : "IR unit LED");
    snprintf(rxLine, sizeof(rxLine), "Capture %s", s_rxPin >= 0 ? "" : "(no unit)");
    const char* items[] = {rxLine, "Saved remotes", "Manual send", txLine, "Re-detect unit"};
    int i = ui::popup("Infrared", items, 5);
    if (i < 0) break;
    if (i == 0) screenCapture();
    if (i == 1) screenRemotes();
    if (i == 2) screenManual();
    if (i == 3) {
      if (s_rxPin < 0) ui::toast("IR unit not found: built-in only", C_WARN);
      else {
        int unitLed = s_rxPin == GROVE_YEL ? GROVE_WHT : GROVE_YEL;
        s_txPin = s_txPin == IR_LED_BUILTIN ? unitLed : IR_LED_BUILTIN;
      }
    }
    if (i == 4) {
      rxStop();
      s_rxPin = grove::irRxPin();
      ui::toast(s_rxPin >= 0 ? "Receiver on G" + String(s_rxPin) : String("No IR receiver found"),
                s_rxPin >= 0 ? C_OK : C_WARN);
      if (s_rxPin < 0) s_txPin = IR_LED_BUILTIN;
    }
  }
  rxStop();
  s_caps.clear();
  s_caps.shrink_to_fit();
  pinMode(IR_LED_BUILTIN, OUTPUT);
  digitalWrite(IR_LED_BUILTIN, LOW);
  grove::release();
}
