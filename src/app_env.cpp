// U169 ENV-Pro (Bosch BME688, I2C 0x77 on Grove).
//
// Two backends, chosen at build time:
//   public image   (pio run -e cardputer-adv)       Bosch BME68x driver: T, RH, P, gas resistance
//   private image  (pio run -e cardputer-adv-bsec)  Bosch BSEC2: adds IAQ, static IAQ, eCO2, bVOC
// Both sample every 3 s (BSEC2 "LP" rate). Views: 1 Live  2 Graph  3 Data  4 Alarms.
#include "apps.h"
#include "grove.h"
#include "settings.h"
#include "ui.h"
#include <Preferences.h>
#include <math.h>
#include <new>
#include <bme68xLibrary.h>
#ifdef UL_BSEC2
#include <bsec2.h>
#endif

static const uint8_t ENV_ADDR = 0x77;
static const uint32_t SAMPLE_MS = 3000;

// ------------------------------------------------------------------ readings
struct Reading {
  bool valid = false;
  float t = NAN, rh = NAN, p = NAN, gasK = NAN;         // degC, %RH, hPa, kOhm
  bool iaqValid = false;
  float iaq = NAN, siaq = NAN, eco2 = NAN, bvoc = NAN;  // BSEC2 only
  float gasPct = NAN, stab = NAN, runIn = NAN;
  uint8_t acc = 0;                                      // IAQ accuracy 0..3
  uint32_t n = 0;                                       // sample counter
};
static Reading R;
static volatile bool s_new = false;

// Saturation vapour pressure over water (Magnus, hPa)
static float esat(float t) { return 6.112f * expf(17.62f * t / (243.12f + t)); }
static float dewPoint(float t, float rh) {
  float g = logf(rh / 100.0f) + 17.62f * t / (243.12f + t);
  return 243.12f * g / (17.62f - g);
}
static float absHumidity(float t, float rh) { return 216.7f * (rh / 100.0f * esat(t)) / (273.15f + t); }
static float altitudeM(float p) { return 44330.0f * (1.0f - powf(p / (cfg.seaLevelHpa10 / 10.0f), 0.1903f)); }
static float heatIndex(float t, float rh) {
  float f = t * 1.8f + 32;
  if (f < 80 || rh < 40) return t;   // Rothfusz regression only holds in hot, humid air
  float hi = -42.379f + 2.04901523f * f + 10.14333127f * rh - 0.22475541f * f * rh - 0.00683783f * f * f -
             0.05481717f * rh * rh + 0.00122874f * f * f * rh + 0.00085282f * f * rh * rh - 0.00000199f * f * f * rh * rh;
  return (hi - 32) / 1.8f;
}

static const char* iaqText(float i) {
  if (i <= 50) return "Excellent";
  if (i <= 100) return "Good";
  if (i <= 150) return "Lightly polluted";
  if (i <= 200) return "Moderately poll.";
  if (i <= 250) return "Heavily polluted";
  if (i <= 350) return "Severely poll.";
  return "Extreme";
}
static uint16_t iaqColor(float i) {
  if (i <= 50) return C_OK;
  if (i <= 100) return 0x9FE0;
  if (i <= 150) return C_YEL;
  if (i <= 200) return C_WARN;
  if (i <= 300) return C_BAD;
  return C_MAG;
}
static const char* accText(uint8_t a) {
  static const char* const T[] = {"0 stabilising", "1 learning", "2 calibrating", "3 calibrated"};
  return T[a > 3 ? 3 : a];
}

// ------------------------------------------------------------------ sensor backends
#ifdef UL_BSEC2
static Bsec2 s_bsec;
static Preferences s_bsecPrefs;
static uint32_t s_stateSavedAt = 0;
static const uint8_t BSEC_CFG[] = {
#include "config/bme688/bme688_sel_33v_3s_4d/bsec_selectivity.txt"
};

static void onBsecData(const bme68xData data, const bsecOutputs outputs, const Bsec2 bsec) {
  (void)data; (void)bsec;
  if (!outputs.nOutputs) return;
  float off = cfg.tempOffsetC10 / 10.0f;
  float rawT = NAN, rawRh = NAN;
  for (uint8_t i = 0; i < outputs.nOutputs; i++) {
    const bsecData& o = outputs.output[i];
    switch (o.sensor_id) {
      case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE: R.t = o.signal; break;
      case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY: R.rh = o.signal; break;
      case BSEC_OUTPUT_RAW_TEMPERATURE: rawT = o.signal + off; break;
      case BSEC_OUTPUT_RAW_HUMIDITY: rawRh = o.signal; break;
      case BSEC_OUTPUT_RAW_PRESSURE: R.p = o.signal > 2000 ? o.signal / 100.0f : o.signal; break;
      case BSEC_OUTPUT_RAW_GAS: R.gasK = o.signal / 1000.0f; break;
      case BSEC_OUTPUT_IAQ: R.iaq = o.signal; R.acc = o.accuracy; R.iaqValid = true; break;
      case BSEC_OUTPUT_STATIC_IAQ: R.siaq = o.signal; break;
      case BSEC_OUTPUT_CO2_EQUIVALENT: R.eco2 = o.signal; break;
      case BSEC_OUTPUT_BREATH_VOC_EQUIVALENT: R.bvoc = o.signal; break;
      case BSEC_OUTPUT_GAS_PERCENTAGE: R.gasPct = o.signal; break;
      case BSEC_OUTPUT_STABILIZATION_STATUS: R.stab = o.signal; break;
      case BSEC_OUTPUT_RUN_IN_STATUS: R.runIn = o.signal; break;
      default: break;
    }
  }
  if (isnan(R.t)) R.t = rawT;
  if (isnan(R.rh)) R.rh = rawRh;
  R.valid = !isnan(R.t) && !isnan(R.rh) && !isnan(R.p);
  R.n++;
  s_new = true;
}

static void bsecSaveState() {
  uint8_t st[BSEC_MAX_STATE_BLOB_SIZE];
  if (s_bsec.getState(st)) s_bsecPrefs.putBytes("state", st, sizeof(st));
  s_stateSavedAt = millis();
}

static String s_err;
static bool sensorBegin() {
  s_bsecPrefs.begin("ulbsec", false);
  if (!s_bsec.begin(ENV_ADDR, Wire)) { s_err = "BSEC begin failed: " + String((int)s_bsec.status); return false; }
  bool cfgOk = s_bsec.setConfig(BSEC_CFG);   // BME688, 3.3 V supply, 3 s sample rate
  if (cfgOk && s_bsecPrefs.getBytesLength("state") == BSEC_MAX_STATE_BLOB_SIZE) {
    uint8_t st[BSEC_MAX_STATE_BLOB_SIZE];
    s_bsecPrefs.getBytes("state", st, sizeof(st));
    s_bsec.setState(st);                      // keeps IAQ calibration across power cycles
  }
  s_bsec.setTemperatureOffset(-cfg.tempOffsetC10 / 10.0f);   // BSEC subtracts the offset
  bsecSensor list[] = {BSEC_OUTPUT_IAQ, BSEC_OUTPUT_STATIC_IAQ, BSEC_OUTPUT_CO2_EQUIVALENT,
                       BSEC_OUTPUT_BREATH_VOC_EQUIVALENT, BSEC_OUTPUT_RAW_TEMPERATURE, BSEC_OUTPUT_RAW_PRESSURE,
                       BSEC_OUTPUT_RAW_HUMIDITY, BSEC_OUTPUT_RAW_GAS, BSEC_OUTPUT_STABILIZATION_STATUS,
                       BSEC_OUTPUT_RUN_IN_STATUS, BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE,
                       BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY, BSEC_OUTPUT_GAS_PERCENTAGE};
  if (!s_bsec.updateSubscription(list, ARRAY_LEN(list), BSEC_SAMPLE_RATE_LP)) {
    s_err = "BSEC subscribe failed: " + String((int)s_bsec.status);
    return false;
  }
  s_bsec.attachCallback(onBsecData);
  s_stateSavedAt = millis();
  return true;
}

static void sensorRun() {
  s_bsec.run();
  // Save the calibration hourly once BSEC reports full accuracy
  if (R.acc >= 3 && millis() - s_stateSavedAt > 3600000UL) bsecSaveState();
}

static void sensorEnd() {
  if (R.acc >= 3) bsecSaveState();
  s_bsecPrefs.end();
}
static const char* backendName() { return "BSEC2"; }
static void gasCycle() {}                          // BSEC2 runs its own heater schedule
static const char* gasText() { return "BSEC2 managed"; }

#else  // ------------------------------------------------ public build: raw BME68x driver
static Bme68x s_bme;
static uint32_t s_trigAt = 0, s_nextAt = 0;
static bool s_waiting = false, s_gasThis = false;
static String s_err;
// The gas measurement heats the hot plate to 320 C, which also warms the temperature sensor.
// Measuring gas less often (or never) gives truer temperature and humidity.
static int s_gasEvery = 0;                         // index into GAS_EVERY
static const uint8_t GAS_EVERY[] = {1, 10, 0};     // every sample (3 s), every 30 s, off
static const char* const GAS_TXT[] = {"3 s", "30 s", "off"};
static uint32_t s_sampleN = 0;
static float s_lastGas = NAN;

static bool sensorBegin() {
  s_bme.begin(ENV_ADDR, Wire);
  if (s_bme.checkStatus() == BME68X_ERROR) { s_err = "BME68x: " + s_bme.statusString(); return false; }
  s_bme.setTPH(BME68X_OS_2X, BME68X_OS_16X, BME68X_OS_1X);
  s_bme.setFilter(BME68X_FILTER_SIZE_3);
  s_bme.setHeaterProf(320, 150);              // 320 degC for 150 ms, Bosch's forced-mode default
  s_nextAt = millis();
  return true;
}

static void sensorRun() {
  uint32_t now = millis();
  if (!s_waiting && (int32_t)(now - s_nextAt) >= 0) {
    uint8_t every = GAS_EVERY[s_gasEvery];
    s_gasThis = every && (s_sampleN % every == 0);
    s_sampleN++;
    // A 1 ms heater pulse is the library's nearest thing to "heater off"; its gas result is
    // flagged unstable and ignored.
    if (s_gasThis) s_bme.setHeaterProf(320, 150); else s_bme.setHeaterProf(150, 1);
    s_bme.setOpMode(BME68X_FORCED_MODE);
    s_trigAt = now;
    s_nextAt += SAMPLE_MS;
    if ((int32_t)(now - s_nextAt) > 0) s_nextAt = now + SAMPLE_MS;   // caught up after a stall
    s_waiting = true;
  }
  // TPH conversion plus the 150 ms heater phase finish well inside 250 ms
  if (s_waiting && now - s_trigAt >= 250) {
    s_waiting = false;
    if (!s_bme.fetchData()) return;
    bme68xData d;
    s_bme.getData(d);
    float off = cfg.tempOffsetC10 / 10.0f;
    float t = d.temperature + off;
    // Shifting temperature changes relative humidity at the same absolute moisture
    float rh = d.humidity * esat(d.temperature) / esat(t);
    R.t = t;
    R.rh = constrain(rh, 0.0f, 100.0f);
    R.p = d.pressure / 100.0f;
    bool gasOk = (d.status & BME68X_GASM_VALID_MSK) && (d.status & BME68X_HEAT_STAB_MSK);
    if (s_gasThis && gasOk) s_lastGas = d.gas_resistance / 1000.0f;
    if (!GAS_EVERY[s_gasEvery]) s_lastGas = NAN;
    R.gasK = s_lastGas;
    R.valid = true;
    R.n++;
    s_new = true;
  }
}
static void sensorEnd() { s_bme.setOpMode(BME68X_SLEEP_MODE); }
static const char* backendName() { return "BME68x raw"; }
static void gasCycle() { s_gasEvery = (s_gasEvery + 1) % 3; s_sampleN = 0; s_lastGas = NAN; }
static const char* gasText() { return GAS_TXT[s_gasEvery]; }
#endif

// ------------------------------------------------------------------ history (graph data)
enum Ch { CH_T, CH_RH, CH_P, CH_GAS, CH_IAQ, CH_CO2, CH_N };
static const char* const CH_NAME[CH_N] = {"Temperature C", "Humidity %RH", "Pressure hPa", "Gas kOhm", "IAQ", "eCO2 ppm"};
static const int HIST = 200;
static const uint16_t TIER_DIV[3] = {1, 10, 60};             // 3 s, 30 s, 3 min per point
static const char* const TIER_TXT[3] = {"10 min", "100 min", "10 h"};
struct Tier { float v[CH_N][HIST]; int head = 0, count = 0; float acc[CH_N]; int accN[CH_N]; int sub = 0; };
static Tier* s_tiers = nullptr;                              // heap: ~14 KB only while ENV is open

static float chValue(int c) {
  switch (c) {
    case CH_T: return R.t;
    case CH_RH: return R.rh;
    case CH_P: return R.p;
    case CH_GAS: return R.gasK;
    case CH_IAQ: return R.iaqValid ? R.iaq : NAN;
    case CH_CO2: return R.iaqValid ? R.eco2 : NAN;
  }
  return NAN;
}

static void histPush() {
  for (int t = 0; t < 3; t++) {
    Tier& T = s_tiers[t];
    for (int c = 0; c < CH_N; c++) {
      float v = chValue(c);
      if (!isnan(v)) { T.acc[c] += v; T.accN[c]++; }
    }
    if (++T.sub < TIER_DIV[t]) continue;
    T.sub = 0;
    for (int c = 0; c < CH_N; c++) {
      T.v[c][T.head] = T.accN[c] ? T.acc[c] / T.accN[c] : NAN;
      T.acc[c] = 0; T.accN[c] = 0;
    }
    T.head = (T.head + 1) % HIST;
    if (T.count < HIST) T.count++;
  }
}

// Pressure tendency over 3 h (synoptic convention), one sample every 10 minutes
static float s_pHist[19];
static int s_pCount = 0;
static uint32_t s_pAt = 0;
static void pressurePush() {
  if (isnan(R.p) || (s_pCount && millis() - s_pAt < 600000UL)) return;
  s_pAt = millis();
  if (s_pCount < 19) s_pHist[s_pCount++] = R.p;
  else { memmove(s_pHist, s_pHist + 1, sizeof(float) * 18); s_pHist[18] = R.p; }
}
static String pressureTrend() {
  if (s_pCount < 19) {
    char b[32];
    snprintf(b, sizeof(b), "trend in %d min", (19 - s_pCount) * 10);
    return b;
  }
  float d = s_pHist[18] - s_pHist[0];
  float a = fabsf(d);
  const char* w = a < 0.1f ? "steady" : a < 1.6f ? "slowly" : a < 3.6f ? "" : a < 6.0f ? "quickly" : "very fast";
  char b[40];
  if (a < 0.1f) snprintf(b, sizeof(b), "steady (%+.1f/3h)", d);
  else snprintf(b, sizeof(b), "%s %s %+.1f/3h", d > 0 ? "rising" : "falling", w, d);
  return b;
}

// ------------------------------------------------------------------ gas baseline (both builds)
static float s_gasBase = NAN;
static uint32_t s_openedAt = 0;
static float gasDropPct() {
  if (isnan(R.gasK) || isnan(s_gasBase) || s_gasBase <= 0) return NAN;
  return max(0.0f, (1.0f - R.gasK / s_gasBase) * 100.0f);
}
static void gasBaselineUpdate() {
  // The heater needs a few minutes before the resistance means anything. After that, the highest
  // resistance seen (cleanest air) is the baseline, decaying slowly so it follows sensor drift.
  if (isnan(R.gasK) || millis() - s_openedAt < 300000UL) return;
  if (isnan(s_gasBase) || R.gasK > s_gasBase) s_gasBase = R.gasK;
  else s_gasBase = s_gasBase * 0.99995f + R.gasK * 0.00005f;
}

// ------------------------------------------------------------------ alarms
struct AlarmState { bool tHi, tLo, rhHi, rhLo, iaq, co2, gas; };
static AlarmState A = {};
static bool s_muted = false;
static uint32_t s_beepAt = 0;
static String s_alarmText;

static void evalAlarms() {
  const EnvAlarms& E = envAlarms;
  auto latch = [](bool& state, bool on, float v, float thr, float hyst, bool high) {
    if (!on || isnan(v)) { state = false; return; }
    if (high) state = state ? v > thr - hyst : v > thr;
    else state = state ? v < thr + hyst : v < thr;
  };
  latch(A.tHi, E.tHiOn, R.t, E.tHi, 0.5f, true);
  latch(A.tLo, E.tLoOn, R.t, E.tLo, 0.5f, false);
  latch(A.rhHi, E.rhHiOn, R.rh, E.rhHi, 2.0f, true);
  latch(A.rhLo, E.rhLoOn, R.rh, E.rhLo, 2.0f, false);
  latch(A.iaq, E.iaqOn, R.iaqValid && R.acc >= 1 ? R.iaq : NAN, E.iaq, 10.0f, true);
  latch(A.co2, E.co2On, R.iaqValid && R.acc >= 1 ? R.eco2 : NAN, E.co2, 50.0f, true);
  latch(A.gas, E.gasOn, gasDropPct(), E.gasDrop, 5.0f, true);
  String s;
  if (A.tHi) s += "TEMP HIGH ";
  if (A.tLo) s += "TEMP LOW ";
  if (A.rhHi) s += "RH HIGH ";
  if (A.rhLo) s += "RH LOW ";
  if (A.iaq) s += "IAQ ";
  if (A.co2) s += "eCO2 ";
  if (A.gas) s += "GAS DROP ";
  s.trim();
  if (s.isEmpty()) s_muted = false;     // all clear: re-arm the sound for the next alarm
  s_alarmText = s;
}

// Three short beeps every 15 s while an alarm is active, without blocking the sensor loop.
static int s_beepN = 0;
static void alarmSound() {
  uint32_t now = millis();
  if (s_alarmText.isEmpty() || s_muted || !envAlarms.sound) { s_beepN = 0; return; }
  if (s_beepN == 0 && (s_beepAt == 0 || now - s_beepAt >= 15000)) { s_beepAt = now; s_beepN = 3; }
  if (s_beepN > 0 && now - s_beepAt >= (uint32_t)(3 - s_beepN) * 180) {
    M5Cardputer.Speaker.tone(2800, 120);
    s_beepN--;
  }
}

// ------------------------------------------------------------------ CSV log
static File s_log;
static int s_logLines = 0;
static String s_logName;
static void logToggle() {
  if (s_log) { s_log.close(); ui::toast("Log closed: " + s_logName); return; }
  if (!ui::sdMount()) { ui::toast("No SD card", C_BAD); return; }
  String path = ui::nextFile("env", "env", "csv");
  s_log = SD.open(path, FILE_WRITE);
  if (!s_log) { ui::toast("SD write failed", C_BAD); return; }
  s_logName = path.substring(path.lastIndexOf('/') + 1);
  s_log.println("uptime_s,temp_c,rh_pct,press_hpa,gas_kohm,iaq,iaq_acc,static_iaq,eco2_ppm,bvoc_ppm,dew_c,alarms");
  s_logLines = 0;
  ui::toast("Logging to " + s_logName);
}
static void logSample() {
  if (!s_log) return;
  char b[200];
  auto f = [](float v, int d) { return isnan(v) ? String("") : String(v, d); };
  snprintf(b, sizeof(b), "%lu,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s", (unsigned long)(millis() / 1000), f(R.t, 2).c_str(),
           f(R.rh, 2).c_str(), f(R.p, 2).c_str(), f(R.gasK, 2).c_str(), f(R.iaqValid ? R.iaq : NAN, 1).c_str(),
           R.iaqValid ? String(R.acc).c_str() : "", f(R.iaqValid ? R.siaq : NAN, 1).c_str(),
           f(R.iaqValid ? R.eco2 : NAN, 0).c_str(), f(R.iaqValid ? R.bvoc : NAN, 2).c_str(),
           f(dewPoint(R.t, R.rh), 2).c_str(), s_alarmText.c_str());
  s_log.println(b);
  if (++s_logLines % 10 == 0) s_log.flush();
}

// ------------------------------------------------------------------ drawing
static void drawStatusLine(int y) {
  if (!s_alarmText.isEmpty()) {
    bool blink = (millis() / 500) % 2;
    cv.fillRect(0, y - 2, SCR_W, 11, blink ? C_BAD : 0x7800);
    cv.setTextColor(C_FG);
    cv.setCursor(4, y);
    cv.print(s_muted ? "MUTED " : "ALARM ");
    cv.print(s_alarmText);
    return;
  }
  cv.setTextColor(C_DIM, C_BG);
  cv.setCursor(4, y);
  cv.printf("%s  #%lu", backendName(), (unsigned long)R.n);
  if (s_log) { cv.setTextColor(C_BAD, C_BG); cv.print("  REC"); }
}

static void viewLive() {
  int y = TOP + 2;
  if (!R.valid) {
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(6, y + 30); cv.print("Waiting for the first sample...");
    return;
  }
  // big temperature and humidity
  cv.setTextSize(3);
  cv.setTextColor(C_FG, C_BG); cv.setCursor(4, y); cv.printf("%.1f", R.t);
  int tx = cv.getCursorX();
  cv.setTextSize(1); cv.setCursor(tx + 2, y); cv.print("o"); cv.setCursor(tx + 8, y + 4); cv.print("C");
  cv.setTextSize(3);
  cv.setTextColor(C_ACC, C_BG); cv.setCursor(132, y); cv.printf("%.0f", R.rh);
  tx = cv.getCursorX();
  cv.setTextSize(1); cv.setCursor(tx + 3, y + 4); cv.print("%RH");
  y += 27;
  cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, y); cv.print("Dew");
  cv.setTextColor(C_FG, C_BG); cv.printf(" %.1fC", dewPoint(R.t, R.rh));
  cv.setTextColor(C_DIM, C_BG); cv.print("  Abs");
  cv.setTextColor(C_FG, C_BG); cv.printf(" %.1fg/m3", absHumidity(R.t, R.rh));
  float hi = heatIndex(R.t, R.rh);
  if (hi > R.t + 0.5f) { cv.setTextColor(C_WARN, C_BG); cv.printf(" HI %.0fC", hi); }
  y += 11;
  cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, y); cv.print("Press");
  cv.setTextColor(C_FG, C_BG); cv.printf(" %.1f hPa  ", R.p);
  cv.setTextColor(C_DIM, C_BG); cv.print(pressureTrend());
  y += 11;
  cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, y); cv.print("Alt");
  cv.setTextColor(C_FG, C_BG); cv.printf(" %.0f m", altitudeM(R.p));
  cv.setTextColor(C_DIM, C_BG); cv.print("  Gas");
  if (isnan(R.gasK)) { cv.setTextColor(C_DIM, C_BG); cv.print(strcmp(gasText(), "off") ? " heating" : " off"); }
  else { cv.setTextColor(C_FG, C_BG); cv.printf(" %.1f kOhm", R.gasK); }
  y += 13;
  // air quality bar
  if (R.iaqValid) {
    uint16_t col = iaqColor(R.iaq);
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, y); cv.print("IAQ");
    cv.setTextColor(col, C_BG); cv.printf(" %.0f %s", R.iaq, iaqText(R.iaq));
    y += 10;
    int w = (int)min(232.0f, R.iaq / 500.0f * 232.0f);
    cv.drawRect(4, y, 232, 6, C_SEL);
    cv.fillRect(5, y + 1, max(1, w - 2), 4, col);
    y += 9;
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, y);
    cv.printf("eCO2 %.0f  bVOC %.2f  acc %d", R.eco2, R.bvoc, R.acc);
  } else {
    float drop = gasDropPct();
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, y);
    if (!strcmp(gasText(), "off")) cv.print("Gas heater off (H to turn on)");
    else if (isnan(drop)) cv.printf("Gas baseline: warming up (%lus)", (unsigned long)max(0L, 300L - (long)((millis() - s_openedAt) / 1000)));
    else { cv.print("Gas vs clean air"); cv.setTextColor(drop > 30 ? C_WARN : C_OK, C_BG); cv.printf(" -%.0f%%", drop); }
    y += 10;
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, y); cv.print("IAQ/eCO2/bVOC need the BSEC2 build");
    y += 9;
  }
  drawStatusLine(SCR_H - 21);
}

static int s_graphCh = CH_T, s_tier = 0;
static float alarmLineFor(int c, bool& has, float& lo, bool& hasLo) {
  has = hasLo = false;
  const EnvAlarms& E = envAlarms;
  switch (c) {
    case CH_T: has = E.tHiOn; hasLo = E.tLoOn; lo = E.tLo; return E.tHi;
    case CH_RH: has = E.rhHiOn; hasLo = E.rhLoOn; lo = E.rhLo; return E.rhHi;
    case CH_IAQ: has = E.iaqOn; return E.iaq;
    case CH_CO2: has = E.co2On; return E.co2;
  }
  return 0;
}

static void viewGraph() {
  Tier& T = s_tiers[s_tier];
  const int gx = 34, gy = TOP + 12, gw = HIST, gh = 86;
  cv.setTextColor(C_ACC, C_BG); cv.setCursor(4, TOP + 1); cv.print(CH_NAME[s_graphCh]);
  cv.setTextColor(C_DIM, C_BG); cv.setCursor(150, TOP + 1); cv.printf("span %s", TIER_TXT[s_tier]);
  float mn = INFINITY, mx = -INFINITY, last = NAN;
  for (int i = 0; i < T.count; i++) {
    float v = T.v[s_graphCh][(T.head - T.count + i + HIST) % HIST];
    if (isnan(v)) continue;
    mn = min(mn, v); mx = max(mx, v); last = v;
  }
  cv.drawRect(gx - 1, gy - 1, gw + 2, gh + 2, C_SEL);
  if (isinf(mn)) {
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(gx + 30, gy + 38);
    cv.print((s_graphCh >= CH_IAQ && !R.iaqValid) ? "needs the BSEC2 build" : "collecting...");
    return;
  }
  float dataMn = mn, dataMx = mx;
  bool hHi, hLo; float lo; float hiThr = alarmLineFor(s_graphCh, hHi, lo, hLo);
  float span = mx - mn;
  if (span < 0.2f) { mn -= 0.1f; mx += 0.1f; span = mx - mn; }
  // Pull an alarm line into view only when it sits close to the data; otherwise mark it
  if (hHi && hiThr > mx && hiThr - mx < span * 0.5f) mx = hiThr;
  if (hLo && lo < mn && mn - lo < span * 0.5f) mn = lo;
  span = mx - mn;
  mn -= span * 0.08f; mx += span * 0.08f; span = mx - mn;
  auto Y = [&](float v) { return gy + gh - 1 - (int)((v - mn) / span * (gh - 1)); };
  cv.setTextColor(C_DIM, C_BG);
  int dec = span < 2 ? 2 : span < 20 ? 1 : 0;
  cv.setCursor(0, gy); cv.printf("%5.*f", dec, mx);
  cv.setCursor(0, gy + gh - 8); cv.printf("%5.*f", dec, mn);
  for (int i = 1; i < 4; i++) cv.drawFastHLine(gx, gy + gh * i / 4, gw, 0x10A2);
  if (hHi) {
    if (hiThr <= mx) cv.drawFastHLine(gx, Y(hiThr), gw, C_BAD);
    else { cv.setTextColor(C_BAD, C_BG); cv.setCursor(gx + gw - 54, gy + 2); cv.printf("^ %.1f", hiThr); }
  }
  if (hLo) {
    if (lo >= mn) cv.drawFastHLine(gx, Y(lo), gw, C_ACC);
    else { cv.setTextColor(C_ACC, C_BG); cv.setCursor(gx + gw - 54, gy + gh - 10); cv.printf("v %.1f", lo); }
  }
  int px = -1, py = 0;
  int x0 = gx + gw - T.count;
  for (int i = 0; i < T.count; i++) {
    float v = T.v[s_graphCh][(T.head - T.count + i + HIST) % HIST];
    if (isnan(v)) { px = -1; continue; }
    int x = x0 + i, y = Y(v);
    if (px >= 0) cv.drawLine(px, py, x, y, C_OK); else cv.drawPixel(x, y, C_OK);
    px = x; py = y;
  }
  cv.setTextColor(C_FG, C_BG);
  // 32 characters max so pressure (1010.27) still fits on one line
  int sd = fabsf(dataMx) >= 1000 ? 1 : 2;
  cv.setCursor(4, gy + gh + 3);
  cv.printf("now %.*f min %.*f max %.*f", sd, last, sd, dataMn, sd, dataMx);
}

static int s_dataTop = 0;
static void buildData(std::vector<String>& L) {
  char b[64];
  auto add = [&](const char* k, const char* fmt, float v) {
    if (isnan(v)) snprintf(b, sizeof(b), "%s\t-", k);
    else { char vb[40]; snprintf(vb, sizeof(vb), fmt, v); snprintf(b, sizeof(b), "%s\t%s", k, vb); }
    L.push_back(b);
  };
  L.push_back("#Climate");
  add("Temperature", "%.2f C", R.t);
  add("Humidity", "%.2f %%RH", R.rh);
  add("Dew point", "%.2f C", R.valid ? dewPoint(R.t, R.rh) : NAN);
  add("Abs humidity", "%.2f g/m3", R.valid ? absHumidity(R.t, R.rh) : NAN);
  add("Heat index", "%.1f C", R.valid ? heatIndex(R.t, R.rh) : NAN);
  add("Temp offset", "%+.1f C", cfg.tempOffsetC10 / 10.0f);
  L.push_back("#Pressure");
  add("Station", "%.2f hPa", R.p);
  add("Altitude", "%.1f m", R.valid ? altitudeM(R.p) : NAN);
  add("Sea-level ref", "%.1f hPa", cfg.seaLevelHpa10 / 10.0f);
  L.push_back(String("3 h trend\t") + pressureTrend());
  L.push_back("#Gas sensor");
  L.push_back(String("Measure every\t") + gasText() + " (H)");
  add("Resistance", "%.2f kOhm", R.gasK);
  add("Clean-air base", "%.2f kOhm", s_gasBase);
  add("Drop vs base", "%.1f %%", gasDropPct());
  L.push_back("#BSEC2 air quality");
  if (R.iaqValid) {
    add("IAQ", "%.1f", R.iaq);
    L.push_back(String("Rating\t") + iaqText(R.iaq));
    L.push_back(String("Accuracy\t") + accText(R.acc));
    add("Static IAQ", "%.1f", R.siaq);
    add("eCO2", "%.0f ppm", R.eco2);
    add("bVOC", "%.2f ppm", R.bvoc);
    add("Gas percentage", "%.1f %%", R.gasPct);
    L.push_back(String("Stabilised\t") + (R.stab >= 1 ? "yes" : "no"));
    L.push_back(String("Run-in done\t") + (R.runIn >= 1 ? "yes" : "no"));
  } else {
    L.push_back("Build\tBME68x raw (no IAQ)");
    L.push_back("For IAQ\tbuild env cardputer-adv-bsec");
  }
  L.push_back("#Session");
  snprintf(b, sizeof(b), "Samples\t%lu (every 3 s)", (unsigned long)R.n); L.push_back(b);
  L.push_back(String("Backend\t") + backendName());
  L.push_back(String("Logging\t") + (s_log ? s_logName : String("off (L)")));
  L.push_back(String("Alarms\t") + (s_alarmText.isEmpty() ? String("clear") : s_alarmText));
}

static void viewData() {
  std::vector<String> L;
  buildData(L);
  int maxTop = max(0, (int)L.size() - (ROWS - 1));
  s_dataTop = constrain(s_dataTop, 0, maxTop);
  for (int r = 0; r < ROWS - 1 && s_dataTop + r < (int)L.size(); r++) {
    const String& l = L[s_dataTop + r];
    int y = TOP + r * LINE_H;
    if (l.startsWith("#")) {
      cv.fillRect(0, y - 1, SCR_W, LINE_H, C_SEL);
      cv.setTextColor(C_YEL, C_SEL); cv.setCursor(2, y); cv.print(l.substring(1));
      continue;
    }
    int tab = l.indexOf('\t');
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(2, y); cv.print(l.substring(0, tab));
    cv.setTextColor(C_FG, C_BG); cv.setCursor(98, y); cv.print(l.substring(tab + 1));
  }
}

static int s_alarmSel = 0;
static const int ALARM_ROWS = 8;
static void viewAlarms() {
  const EnvAlarms& E = envAlarms;
  struct Row { const char* name; bool on; float v; const char* unit; };
  Row rows[ALARM_ROWS] = {
    {"Temp high", E.tHiOn, E.tHi, "C"}, {"Temp low", E.tLoOn, E.tLo, "C"},
    {"RH high", E.rhHiOn, E.rhHi, "%"}, {"RH low", E.rhLoOn, E.rhLo, "%"},
    {"IAQ above", E.iaqOn, E.iaq, ""}, {"eCO2 above", E.co2On, E.co2, "ppm"},
    {"Gas drop", E.gasOn, E.gasDrop, "%"}, {"Beep", E.sound, NAN, ""},
  };
  for (int i = 0; i < ALARM_ROWS; i++) {
    int y = TOP + 1 + i * 11;
    bool sel = i == s_alarmSel;
    if (sel) cv.fillRect(0, y - 2, SCR_W, 11, C_SEL);
    uint16_t bg = sel ? C_SEL : C_BG;
    cv.setTextColor(rows[i].on ? C_OK : C_DIM, bg); cv.setCursor(4, y); cv.print(rows[i].on ? "[on ]" : "[off]");
    cv.setTextColor(sel ? C_FG : C_DIM, bg); cv.setCursor(40, y); cv.print(rows[i].name);
    if (!isnan(rows[i].v)) { cv.setTextColor(sel ? C_YEL : C_FG, bg); cv.setCursor(130, y); cv.printf("%.1f %s", rows[i].v, rows[i].unit); }
  }
#ifndef UL_BSEC2
  cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, TOP + 1 + ALARM_ROWS * 11);
  cv.print("IAQ/eCO2 alarms need the BSEC2 build");
#endif
}

static void alarmsKey(const ui::Key& k) {
  EnvAlarms& E = envAlarms;
  if (k.up) s_alarmSel = (s_alarmSel + ALARM_ROWS - 1) % ALARM_ROWS;
  if (k.down) s_alarmSel = (s_alarmSel + 1) % ALARM_ROWS;
  int step = k.right ? 1 : (k.left ? -1 : 0);
  bool toggle = k.enter || k.space;
  bool* on[ALARM_ROWS] = {&E.tHiOn, &E.tLoOn, &E.rhHiOn, &E.rhLoOn, &E.iaqOn, &E.co2On, &E.gasOn, &E.sound};
  float* val[ALARM_ROWS] = {&E.tHi, &E.tLo, &E.rhHi, &E.rhLo, &E.iaq, &E.co2, &E.gasDrop, nullptr};
  static const float STEP[ALARM_ROWS] = {0.5f, 0.5f, 1, 1, 10, 50, 5, 0};
  static const float LO[ALARM_ROWS] = {-40, -40, 0, 0, 0, 400, 5, 0};
  static const float HI[ALARM_ROWS] = {85, 85, 100, 100, 500, 10000, 95, 0};
  if (toggle) { *on[s_alarmSel] = !*on[s_alarmSel]; alarmsSave(); }
  if (step && val[s_alarmSel]) {
    float& v = *val[s_alarmSel];
    v = constrain(v + step * STEP[s_alarmSel], LO[s_alarmSel], HI[s_alarmSel]);
    alarmsSave();
  }
}

// ------------------------------------------------------------------ section
void appEnv() {
  if (!grove::i2cSafe()) {
    ui::toast("Grove holds a UART/IR unit, not I2C", C_BAD, 1800);
    return;
  }
  if (!grove::i2cBegin(400000) || !grove::i2cPresent(ENV_ADDR)) {
    grove::release();
    ui::toast("No ENV-Pro at 0x77 on Grove", C_BAD, 1800);
    return;
  }
  bool idOk = false;
  if (grove::i2cRead8(ENV_ADDR, 0xD0, &idOk) != 0x61 || !idOk) {
    grove::release();
    ui::toast("0x77 is not a BME68x", C_BAD, 1800);
    return;
  }
  R = Reading();
  s_err = "";
  if (!sensorBegin()) {
    grove::release();
    ui::toast(s_err, C_BAD, 2500);
    return;
  }
  s_tiers = new (std::nothrow) Tier[3];
  if (!s_tiers) { sensorEnd(); grove::release(); ui::toast("Out of memory", C_BAD); return; }
  for (int t = 0; t < 3; t++) {
    s_tiers[t].head = s_tiers[t].count = s_tiers[t].sub = 0;
    for (int c = 0; c < CH_N; c++) { s_tiers[t].acc[c] = 0; s_tiers[t].accN[c] = 0; }
  }
  s_pCount = 0; s_gasBase = NAN; s_openedAt = millis(); A = {}; s_muted = false; s_alarmText = "";
  s_beepAt = 0; s_beepN = 0;
  int view = 0;
  bool dirty = true;
  uint32_t lastDraw = 0;
  static const char* const VIEWS[] = {"Live", "Graph", "Data", "Alarms"};
  while (true) {
    sensorRun();
    if (s_new) {
      s_new = false;
      histPush();
      pressurePush();
      gasBaselineUpdate();
      evalAlarms();
      logSample();
      dirty = true;
    }
    alarmSound();
    ui::Key k = ui::poll();
    if (k.back) break;
    if (k.any) dirty = true;
    if (k.tab) view = (view + 1) % 4;
    if (k.ch >= '1' && k.ch <= '4') view = k.ch - '1';
    if (k.lc == 'l') logToggle();
    if (k.lc == 'm' && !s_alarmText.isEmpty()) s_muted = true;
#ifndef UL_BSEC2
    if (k.lc == 'h') { gasCycle(); s_gasBase = NAN; s_openedAt = millis(); ui::toast(String("Gas every ") + gasText(), C_OK, 700); }
#endif
    if (view == 1) {
      if (k.up) s_graphCh = (s_graphCh + CH_N - 1) % CH_N;
      if (k.down) s_graphCh = (s_graphCh + 1) % CH_N;
      if (k.left) s_tier = (s_tier + 2) % 3;
      if (k.right) s_tier = (s_tier + 1) % 3;
    } else if (view == 2) {
      if (k.up) s_dataTop--;
      if (k.down) s_dataTop++;
      if (k.left) s_dataTop -= ROWS - 1;
      if (k.right) s_dataTop += ROWS - 1;
      if (k.lc == 's') { std::vector<String> L; buildData(L); ui::saveLines("env", L); }
    } else if (view == 3) {
      alarmsKey(k);
    }
    if (!s_alarmText.isEmpty() && millis() - lastDraw > 500) dirty = true;   // blink the banner
    if (!dirty) { delay(10); continue; }
    dirty = false;
    lastDraw = millis();
    ui::stayAwake();   // people watch this screen without pressing keys
    ui::clear();
    char hdr[24];
    snprintf(hdr, sizeof(hdr), "ENV-Pro: %s", VIEWS[view]);
    ui::header(hdr);
    switch (view) {
      case 0: viewLive(); ui::footer("TAB view L log H gas M mute"); break;
      case 1: viewGraph(); ui::footer(";/. channel  ,// span  TAB view"); break;
      case 2: viewData(); ui::footer(";/. scroll  S save  L log  TAB view"); break;
      case 3: viewAlarms(); ui::footer(";/. pick ,// value ENTER on/off"); break;
    }
    ui::push();
  }
  if (s_log) s_log.close();
  sensorEnd();
  delete[] s_tiers;
  s_tiers = nullptr;
  grove::release();
}
