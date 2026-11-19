// Grove port (HY2.0-4P) on the Cardputer ADV: G2 = yellow wire (SDA), G1 = white wire (SCL).
// The same two pins serve I2C units (ENV-Pro, NFC, RFID2), UART units (GPS, Gateway H2) and the
// IR unit (one pin per direction), so each section claims the port and releases it on exit.
#pragma once
#include <Arduino.h>
#include <Wire.h>

static const int GROVE_YEL = 2;   // yellow wire, I2C SDA
static const int GROVE_WHT = 1;   // white wire, I2C SCL

namespace grove {

void release();                         // stop I2C / UART on the port, park both pins as inputs
bool i2cBegin(uint32_t hz = 400000);    // Wire on SDA G2 / SCL G1
bool i2cPresent(uint8_t addr);          // ACK test on the Grove bus (call after i2cBegin)
uint8_t i2cRead8(uint8_t addr, uint8_t reg, bool* ok = nullptr);

// UART units. Listens for valid NMEA ($...*hh) with both wire orders and the given baud rates.
// Returns true and fills rx/tx/baud when sentences arrive. Leaves Serial1 running on success.
bool nmeaProbe(int& rx, int& tx, uint32_t& baud, uint32_t listenMs = 1200);
void uartBegin(uint32_t baud, int rx, int tx);

// IR unit U002: the receiver output idles high (it is actively driven), the LED input does not.
// Returns the receiver pin, or -1 when neither wire looks like an idle IR receiver.
int irRxPin();

struct Found {
  bool envPro = false;    // BME688 at 0x77 (chip id 0x61)
  bool bme680 = false;    // BME68x at 0x76 (Waveshare BME680 with ADDR bridged)
  bool rfid2 = false;     // 0x28
  bool nfc = false;       // 0x50
  bool cardkb = false;    // 0x5F
  bool gps = false;       // NMEA stream
  int gpsRx = -1, gpsTx = -1;
  uint32_t gpsBaud = 0;
  int irRx = -1;
  int i2cCount = 0;
  uint8_t i2cAddrs[16];
};
Found scan(bool includeUart = true);

bool nmeaChecksumOk(const char* line);  // "$GPGGA,...*hh" without CR/LF
String nmeaWithChecksum(const char* body);  // "PCAS02,1000" -> "$PCAS02,1000*2E\r\n"

}  // namespace grove
