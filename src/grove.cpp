#include "grove.h"
#include <M5Cardputer.h>

namespace grove {

static bool s_i2c = false, s_uart = false;

void release() {
  if (s_i2c) { Wire.end(); s_i2c = false; }
  if (s_uart) { Serial1.end(); s_uart = false; }
  pinMode(GROVE_YEL, INPUT);
  pinMode(GROVE_WHT, INPUT);
}

bool i2cBegin(uint32_t hz) {
  release();
  M5.Ex_I2C.release();                  // M5Unified's own handle on the Grove pins, if it took them
  s_i2c = Wire.begin(GROVE_YEL, GROVE_WHT, hz);
  Wire.setTimeOut(20);
  return s_i2c;
}

bool i2cPresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

uint8_t i2cRead8(uint8_t addr, uint8_t reg, bool* ok) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  bool good = Wire.endTransmission(false) == 0 && Wire.requestFrom((int)addr, 1) == 1;
  uint8_t v = good ? Wire.read() : 0;
  if (ok) *ok = good;
  return v;
}

void uartBegin(uint32_t baud, int rx, int tx) {
  if (s_i2c) { Wire.end(); s_i2c = false; }
  if (s_uart) Serial1.end();
  Serial1.setRxBufferSize(2048);
  Serial1.begin(baud, SERIAL_8N1, rx, tx);
  s_uart = true;
}

bool nmeaChecksumOk(const char* line) {
  if (line[0] != '$') return false;
  const char* star = strchr(line, '*');
  if (!star || !isxdigit((unsigned char)star[1]) || !isxdigit((unsigned char)star[2])) return false;
  uint8_t sum = 0;
  for (const char* p = line + 1; p < star; p++) sum ^= (uint8_t)*p;
  return sum == (uint8_t)strtol(star + 1, nullptr, 16);
}

String nmeaWithChecksum(const char* body) {
  uint8_t sum = 0;
  for (const char* p = body; *p; p++) sum ^= (uint8_t)*p;
  char tail[8];
  snprintf(tail, sizeof(tail), "*%02X\r\n", sum);
  return String("$") + body + tail;
}

// Counts checksum-valid sentences seen within listenMs.
static int listenNmea(uint32_t listenMs) {
  char line[100];
  int len = 0, good = 0;
  uint32_t until = millis() + listenMs;
  while (millis() < until) {
    while (Serial1.available()) {
      char c = Serial1.read();
      if (c == '$') len = 0;
      if (c == '\r' || c == '\n') {
        line[len] = 0;
        if (len > 6 && nmeaChecksumOk(line)) good++;
        len = 0;
      } else if (len < (int)sizeof(line) - 1) {
        line[len++] = c;
      }
    }
    if (good >= 2) return good;
    delay(5);
  }
  return good;
}

bool nmeaProbe(int& rx, int& tx, uint32_t& baud, uint32_t listenMs) {
  static const uint32_t BAUDS[] = {115200, 9600, 38400};
  static const int ORDER[][2] = {{GROVE_YEL, GROVE_WHT}, {GROVE_WHT, GROVE_YEL}};
  for (uint32_t b : BAUDS) {
    for (auto& o : ORDER) {
      uartBegin(b, o[0], o[1]);
      if (listenNmea(listenMs) >= 2) { rx = o[0]; tx = o[1]; baud = b; return true; }
    }
  }
  release();
  return false;
}

int irRxPin() {
  release();
  // A demodulating IR receiver drives its output high while idle. Pull both pins down weakly:
  // the receiver output stays high, a bare LED driver input or an empty port reads low.
  int pins[2] = {GROVE_YEL, GROVE_WHT};
  int highs[2] = {0, 0};
  for (int p = 0; p < 2; p++) pinMode(pins[p], INPUT_PULLDOWN);
  delay(5);
  for (int i = 0; i < 20; i++) {
    for (int p = 0; p < 2; p++) highs[p] += digitalRead(pins[p]);
    delay(2);
  }
  release();
  if (highs[0] >= 18 && highs[1] <= 2) return pins[0];
  if (highs[1] >= 18 && highs[0] <= 2) return pins[1];
  return -1;
}

bool linesIdleHigh() {
  release();
  pinMode(GROVE_YEL, INPUT_PULLUP);
  pinMode(GROVE_WHT, INPUT_PULLUP);
  delay(3);
  int hi = 0;
  for (int i = 0; i < 10; i++) { hi += digitalRead(GROVE_YEL) && digitalRead(GROVE_WHT); delayMicroseconds(200); }
  release();
  return hi >= 9;
}

// Counts level changes on both wires while listening passively (pull-ups on, nothing driven).
// A UART unit sending data toggles a line hundreds of times a second; I2C units and an idle
// IR receiver never do.
static int lineActivity(uint32_t ms) {
  pinMode(GROVE_YEL, INPUT_PULLUP);
  pinMode(GROVE_WHT, INPUT_PULLUP);
  delay(2);
  int a = digitalRead(GROVE_YEL), b = digitalRead(GROVE_WHT), edges = 0;
  uint32_t until = millis() + ms;
  while (millis() < until) {
    int na = digitalRead(GROVE_YEL), nb = digitalRead(GROVE_WHT);
    edges += (na != a) + (nb != b);
    a = na; b = nb;
  }
  release();
  return edges;
}

bool i2cSafe() { return lineActivity(400) <= 20 && irRxPin() < 0 && linesIdleHigh(); }

// Scan order matters, because each probe drives the wires in a way some units dislike:
//  1. listen passively for UART traffic (GPS talks at least once a second)
//  2. if quiet, look for an IR receiver with weak pull-downs (one wire high, the other low)
//  3. only then, with both wires idle high, try I2C
// I2C is never started on the IR unit: its LED input loads the SDA wire, and the ESP32 I2C
// driver could wait forever on that bus (the v0.1 freeze). UART is only opened on a wire that is
// already carrying data, so the IR LED is never driven by a UART idle level.
Found scan(bool includeUart, const std::function<bool(const char*)>& step) {
  Found f;
  auto go = [&](const char* t) { if (!step(t)) { f.aborted = true; release(); return false; } return true; };

  if (!go("Listening for UART traffic")) return f;
  bool busy = lineActivity(700) > 20;
  if (!busy) { if (!go("Listening for UART traffic")) return f; busy = lineActivity(700) > 20; }

  if (busy) {
    if (includeUart) {
      static const uint32_t BAUDS[] = {115200, 9600, 38400};
      static const int ORDER[][2] = {{GROVE_YEL, GROVE_WHT}, {GROVE_WHT, GROVE_YEL}};
      char msg[40];
      for (uint32_t b : BAUDS) {
        for (auto& o : ORDER) {
          snprintf(msg, sizeof(msg), "NMEA %lu baud, RX G%d", (unsigned long)b, o[0]);
          if (!go(msg)) return f;
          uartBegin(b, o[0], o[1]);
          if (listenNmea(1000) >= 2) { f.gps = true; f.gpsRx = o[0]; f.gpsTx = o[1]; f.gpsBaud = b; break; }
        }
        if (f.gps) break;
      }
    }
    f.uartBusy = !f.gps;
    release();
    return f;
  }

  if (!go("Looking for an IR receiver")) return f;
  f.irRx = irRxPin();
  if (f.irRx >= 0) return f;

  if (!go("Checking line levels")) return f;
  if (!linesIdleHigh()) { f.i2cSkipped = true; return f; }

  if (!go("I2C scan 0x08-0x77")) return f;
  if (i2cBegin(100000)) {
    Wire.setTimeOut(10);
    for (uint8_t a = 8; a < 120; a++) {
      if ((a & 15) == 0 && !go("I2C scan 0x08-0x77")) return f;
      if (!i2cPresent(a)) continue;
      if (f.i2cCount < 16) f.i2cAddrs[f.i2cCount] = a;
      f.i2cCount++;
      bool ok = false;
      if (a == 0x77 && i2cRead8(a, 0xD0, &ok) == 0x61 && ok) f.envPro = true;
      if (a == 0x76 && i2cRead8(a, 0xD0, &ok) == 0x61 && ok) f.bme680 = true;
      if (a == 0x28) f.rfid2 = true;
      if (a == 0x50) f.nfc = true;
      if (a == 0x5F) f.cardkb = true;
    }
  }
  release();
  if (f.i2cCount > 20) f.i2cCount = 0;   // every address "answering" = not a real I2C bus
  return f;
}

}  // namespace grove
