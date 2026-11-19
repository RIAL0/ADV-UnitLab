// Detect: identifies what is plugged into the Grove port, and how it is wired.
#include "apps.h"
#include "grove.h"
#include "ui.h"

static const char* nameFor(uint8_t a) {
  switch (a) {
    case 0x28: return "U031-B RFID2 (WS1850S)";
    case 0x50: return "U216 NFC (ST25R3916)";
    case 0x5F: return "CardKB";
    case 0x76: return "BME68x (Waveshare BME680?)";
    case 0x77: return "U169 ENV-Pro (BME688)?";
    default: return "unknown";
  }
}

void appDetect() {
  uint32_t t0 = millis();
  grove::Found f = grove::scan(true, [&](const char* stage) {
    ui::clear();
    ui::header("Detect");
    cv.setTextColor(C_FG, C_BG);
    cv.setCursor(6, TOP + 24); cv.print("Scanning the Grove port...");
    cv.setTextColor(C_YEL, C_BG);
    cv.setCursor(6, TOP + 40); cv.print(stage);
    cv.setTextColor(C_DIM, C_BG);
    cv.setCursor(6, TOP + 56); cv.printf("%lu s", (unsigned long)((millis() - t0) / 1000));
    ui::footer("` or G0 abort");
    ui::push();
    ui::Key k = ui::poll();
    return !k.back;
  });
  if (f.aborted) { ui::toast("Scan aborted", C_WARN); return; }
  ui::pager("Detect", [&](std::vector<String>& l) {
    char b[64];
    l.push_back("#Grove port  G2 yellow / G1 white");
    if (f.i2cCount) {
      for (int i = 0; i < f.i2cCount && i < 16; i++) {
        uint8_t a = f.i2cAddrs[i];
        snprintf(b, sizeof(b), "I2C 0x%02X\t%s", a, nameFor(a));
        l.push_back(b);
      }
      if (f.envPro) l.push_back("ENV-Pro\tconfirmed: chip id 0x61 at 0x77");
      if (f.bme680) l.push_back("BME68x\tchip id 0x61 at 0x76");
      if (f.rfid2) l.push_back("RFID2\tsupported in Phase 2");
      if (f.nfc) l.push_back("NFC\tsupported in Phase 2");
    } else if (f.gps) {
      snprintf(b, sizeof(b), "%lu baud, RX G%d / TX G%d", (unsigned long)f.gpsBaud, f.gpsRx, f.gpsTx);
      l.push_back("GPS / NMEA\tfound");
      l.push_back(String("UART\t") + b);
      l.push_back("Open\tGPS section");
    } else if (f.irRx >= 0) {
      snprintf(b, sizeof(b), "receiver G%d, LED G%d", f.irRx, f.irRx == GROVE_YEL ? GROVE_WHT : GROVE_YEL);
      l.push_back("IR unit\tidle receiver found");
      l.push_back(String("Wiring\t") + b);
      l.push_back("Open\tInfrared section");
    } else if (f.uartBusy) {
      l.push_back("UART data\tbut not NMEA");
      l.push_back("Likely\tGateway H2 or another UART unit");
    } else if (f.i2cSkipped) {
      l.push_back("Wire held low\tI2C not attempted");
      l.push_back("Check\tcable and unit; reseat both ends");
    } else {
      l.push_back("Nothing\tanswered on UART, IR or I2C");
      l.push_back("Check\tcable fully seated in both ends");
    }
    l.push_back("#Tips");
    l.push_back("One unit at a time on Grove. RFID2, NFC and ENV-Pro can share a Grove hub (no address clash).");
    l.push_back("GPS and Gateway H2 need the port to themselves.");
  });
}
