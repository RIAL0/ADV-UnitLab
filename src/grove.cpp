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

Found scan(bool includeUart) {
  Found f;
  if (i2cBegin(100000)) {
    for (uint8_t a = 8; a < 120; a++) {
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
  if (f.i2cCount == 0) {
    // Nothing on I2C: the port may hold a UART or IR unit instead. A UART transmit line also
    // idles high, so look for NMEA first and only then for the IR receiver's idle-high output.
    if (includeUart) {
      f.gps = nmeaProbe(f.gpsRx, f.gpsTx, f.gpsBaud, 1200);
      release();
    }
    if (!f.gps) f.irRx = irRxPin();
  }
  return f;
}

}  // namespace grove
