// ADV-UnitLab for M5Stack Cardputer ADV - explorer for M5 Units and Caps.
// Keys everywhere:  ; up   . down   , left   / right   ENTER select   ` or Backspace or G0 = back
#include "apps.h"
#include "grove.h"
#include "settings.h"
#include "ui.h"

#include "menu_draw.h"

// Same order as MENU_ITEMS in menu_draw.h
static void (*const RUN[])() = {appEnv, appGps, appIr, appNfc, appDetect, appSettings};
static const int NITEMS = MENU_COUNT;
static_assert(sizeof(RUN) / sizeof(RUN[0]) == MENU_COUNT, "RUN[] and MENU_ITEMS[] must match");

void setup() {
  auto m5cfg = M5.config();
  m5cfg.internal_imu = false;
  m5cfg.internal_mic = false;
  m5cfg.internal_spk = true;
  M5Cardputer.begin(m5cfg, true);
  M5Cardputer.Display.setRotation(1);
  cfgLoad();
  M5Cardputer.Display.setBrightness(cfg.brightness);
  M5Cardputer.Speaker.setVolume(cfg.volume);
  ui::begin();
  ui::sdMount();
  analogSetPinAttenuation(10, ADC_11db);
  // Board detection can leave the expansion-header pins driven as outputs. Park them as inputs
  // so a fitted Cap is never driven against until a section claims it.
  for (int pin : {3, 4, 5, 6, 13, 15}) pinMode(pin, INPUT);
  grove::release();
  pinMode(0, INPUT_PULLUP);

  ui::clear();
  cv.setTextColor(C_ACC); cv.setTextSize(3); cv.setCursor(48, 36); cv.print("UnitLab");
  cv.setTextSize(1); cv.setTextColor(C_DIM); cv.setCursor(36, 72); cv.print("M5 Units + Caps explorer v" UL_VERSION);
  cv.setCursor(40, 90);
  bool adv = M5.getBoard() == m5::board_t::board_M5CardputerADV;
  cv.setTextColor(adv ? C_OK : C_WARN);
  cv.print(adv ? "   Cardputer ADV detected" : "Warning: ADV not detected");
  ui::push();
  delay(900);
}

void loop() {
  static int sel = 0, top = 0;
  static bool dirty = true;
  static uint32_t lastDraw = 0;
  ui::Key k = ui::poll();
  if (k.up) { sel = (sel + NITEMS - 1) % NITEMS; dirty = true; }
  if (k.down) { sel = (sel + 1) % NITEMS; dirty = true; }
  if (k.left) { sel = max(0, sel - MENU_ROWS); dirty = true; }
  if (k.right) { sel = min(NITEMS - 1, sel + MENU_ROWS); dirty = true; }
  if (k.ch >= '1' && k.ch <= '9' && k.ch - '1' < NITEMS) { sel = k.ch - '1'; dirty = true; }
  if (k.enter) {
    RUN[sel]();
    grove::release();          // whatever the section did, hand the port back clean
    dirty = true;
  }
  if (millis() - lastDraw > 5000) dirty = true;      // refresh battery in the header
  if (!dirty) { delay(15); return; }
  dirty = false;
  lastDraw = millis();
  top = sel - 1;
  if (top > NITEMS - MENU_ROWS) top = NITEMS - MENU_ROWS;
  if (top < 0) top = 0;
  ui::clear();
  ui::header("UnitLab");
  drawMenuBody(cv, sel, top);
  ui::footer(";/. move   ,// page   ENTER open");
  ui::push();
}
