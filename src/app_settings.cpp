// Settings: screen, idle behaviour, speaker volume, ENV-Pro corrections, about.
#include "apps.h"
#include "settings.h"
#include "ui.h"
#include <Preferences.h>

Config cfg;
EnvAlarms envAlarms;
static Preferences s_prefs;

static const uint16_t DIM_SEC[] = {0, 15, 30, 60, 120, 300};
static const char* const DIM_TXT[] = {"never", "15 s", "30 s", "1 min", "2 min", "5 min"};
static const uint16_t OFF_SEC[] = {0, 60, 120, 300, 600, 1800};
static const char* const OFF_TXT[] = {"never", "1 min", "2 min", "5 min", "10 min", "30 min"};

void cfgLoad() {
  s_prefs.begin("ulcfg", false);
  Config d;
  cfg.brightness = s_prefs.getUChar("bright", d.brightness);
  cfg.dimIdx = min((int)s_prefs.getUChar("dim", d.dimIdx), 5);
  cfg.offIdx = min((int)s_prefs.getUChar("off", d.offIdx), 5);
  cfg.volume = s_prefs.getUChar("vol", d.volume);
  cfg.tempOffsetC10 = s_prefs.getShort("toff", d.tempOffsetC10);
  cfg.seaLevelHpa10 = s_prefs.getUShort("slp", d.seaLevelHpa10);
  if (cfg.brightness < 16) cfg.brightness = 16;
  if (cfg.seaLevelHpa10 < 8700 || cfg.seaLevelHpa10 > 10900) cfg.seaLevelHpa10 = d.seaLevelHpa10;
  alarmsLoad();
}

void cfgSave() {
  s_prefs.putUChar("bright", cfg.brightness);
  s_prefs.putUChar("dim", cfg.dimIdx);
  s_prefs.putUChar("off", cfg.offIdx);
  s_prefs.putUChar("vol", cfg.volume);
  s_prefs.putShort("toff", cfg.tempOffsetC10);
  s_prefs.putUShort("slp", cfg.seaLevelHpa10);
}

void alarmsLoad() {
  EnvAlarms d;
  if (s_prefs.getBytesLength("alarms") == sizeof(EnvAlarms)) s_prefs.getBytes("alarms", &envAlarms, sizeof(EnvAlarms));
  else envAlarms = d;
}

void alarmsSave() { s_prefs.putBytes("alarms", &envAlarms, sizeof(EnvAlarms)); }

uint32_t cfgDimMs() { return DIM_SEC[cfg.dimIdx] * 1000UL; }
uint32_t cfgOffMs() { return OFF_SEC[cfg.offIdx] * 1000UL; }

void appSettings() {
  int sel = 0;
  const int N = 7;
  bool dirty = true;
  while (true) {
    ui::Key k = ui::poll();
    if (k.back) break;
    if (k.up) { sel = (sel + N - 1) % N; dirty = true; }
    if (k.down) { sel = (sel + 1) % N; dirty = true; }
    int step = k.right ? 1 : (k.left ? -1 : 0);
    if (step) {
      dirty = true;
      switch (sel) {
        case 0: cfg.brightness = constrain(cfg.brightness + step * 16, 16, 255);
                M5Cardputer.Display.setBrightness(cfg.brightness); break;
        case 1: cfg.dimIdx = (cfg.dimIdx + 6 + step) % 6; break;
        case 2: cfg.offIdx = (cfg.offIdx + 6 + step) % 6; break;
        case 3: cfg.volume = constrain(cfg.volume + step * 16, 0, 255);
                M5Cardputer.Speaker.setVolume(cfg.volume);
                M5Cardputer.Speaker.tone(2000, 60); break;
        case 4: cfg.tempOffsetC10 = constrain(cfg.tempOffsetC10 + step, -100, 100); break;
        case 5: cfg.seaLevelHpa10 = constrain(cfg.seaLevelHpa10 + step, 8700, 10900); break;
        default: break;
      }
      cfgSave();
    }
    if (k.enter && sel == 6) {
      ui::pager("About UnitLab", [](std::vector<String>& l) {
        l.push_back("#ADV-UnitLab");
        l.push_back("Version\t" UL_VERSION);
#ifdef UL_BSEC2
        l.push_back("ENV build\tBSEC2 (IAQ, eCO2, bVOC)");
#else
        l.push_back("ENV build\tBME68x raw (public image)");
#endif
        l.push_back("Source\tgithub.com/RIAL0/ADV-UnitLab");
        l.push_back("#Phase 1 sections");
        l.push_back("ENV-Pro U169, GPS U032 v1.1, IR U002");
        l.push_back("#Coming next");
        l.push_back("Phase 2: RFID2 U031-B, NFC U216 + Cap U219 NFC");
        l.push_back("Phase 3: Cap CC1101 U219 Sub-GHz, Hydra-RF nRF24");
        l.push_back("Phase 4: Gateway H2 U195 Zigbee/Thread/Matter");
        l.push_back("#Files");
        l.push_back("All captures and logs go to /UnitLab/ on the SD card.");
      });
      dirty = true;
    }
    if (!dirty) { delay(15); continue; }
    dirty = false;
    ui::clear();
    ui::header("Settings");
    char v[7][28];
    snprintf(v[0], 28, "%d%%", cfg.brightness * 100 / 255);
    snprintf(v[1], 28, "%s", DIM_TXT[cfg.dimIdx]);
    snprintf(v[2], 28, "%s", OFF_TXT[cfg.offIdx]);
    snprintf(v[3], 28, "%d%%", cfg.volume * 100 / 255);
    snprintf(v[4], 28, "%+.1f C", cfg.tempOffsetC10 / 10.0f);
    snprintf(v[5], 28, "%.1f hPa", cfg.seaLevelHpa10 / 10.0f);
    snprintf(v[6], 28, "ENTER");
    static const char* const L[] = {"Brightness", "Dim after", "Screen off", "Volume", "ENV temp offset",
                                    "Sea-level press.", "About"};
    for (int i = 0; i < N; i++) {
      int y = TOP + 2 + i * 14;
      bool on = i == sel;
      if (on) cv.fillRect(0, y - 3, SCR_W, 13, C_SEL);
      cv.setTextColor(on ? C_FG : C_DIM, on ? C_SEL : C_BG);
      cv.setCursor(6, y); cv.print(L[i]);
      cv.setTextColor(on ? C_YEL : C_FG, on ? C_SEL : C_BG);
      cv.setCursor(130, y); cv.print(v[i]);
    }
    ui::footer(";/. select   ,// change   ` back");
    ui::push();
  }
}
