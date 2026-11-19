// U032-V11 GPS v1.1 (ATGM336H-6N / AT6668, NMEA 0183 4.1, UART on Grove).
// TinyGPSPlus decodes position, speed and time; a small parser of our own adds GSV (satellites in
// view, SNR, elevation, azimuth) and GSA (satellites used, fix mode, PDOP/HDOP/VDOP).
// Views: 1 Dash  2 Sky  3 Signal  4 Trip  5 Data.  L = GPX log, R = update rate, C = cold start.
#include "apps.h"
#include "grove.h"
#include "settings.h"
#include "ui.h"
#include <TinyGPSPlus.h>
#include <math.h>
#include <algorithm>

static TinyGPSPlus gps;

// ------------------------------------------------------------------ satellites (GSV / GSA)
struct Sat { char sys; uint16_t prn; int8_t elev; int16_t az; int8_t snr; bool used; uint32_t seen; };
static const int MAX_SATS = 64;
static Sat s_sats[MAX_SATS];
static int s_nSats = 0;
static int s_fixMode = 1;            // GSA: 1 none, 2 = 2D, 3 = 3D
static float s_pdop = NAN, s_hdop = NAN, s_vdop = NAN;
static uint32_t s_gsvCount = 0, s_gsaCount = 0, s_badLines = 0;
static String s_lastSentence;

// NMEA talker -> constellation letter (RINEX style)
static char sysFromTalker(const char* t) {
  if (!strncmp(t, "GP", 2)) return 'G';
  if (!strncmp(t, "GL", 2)) return 'R';
  if (!strncmp(t, "GA", 2)) return 'E';
  if (!strncmp(t, "GB", 2) || !strncmp(t, "BD", 2)) return 'C';
  if (!strncmp(t, "GQ", 2) || !strncmp(t, "QZ", 2)) return 'J';
  if (!strncmp(t, "GI", 2)) return 'I';
  return '?';
}
static char sysFromId(int id) {           // NMEA 4.1 GSA system id
  switch (id) { case 1: return 'G'; case 2: return 'R'; case 3: return 'E'; case 4: return 'C'; case 5: return 'J'; case 6: return 'I'; }
  return '?';
}
static const char* sysName(char s) {
  switch (s) { case 'G': return "GPS"; case 'R': return "GLONASS"; case 'E': return "Galileo"; case 'C': return "BeiDou";
               case 'J': return "QZSS"; case 'I': return "NavIC"; }
  return "other";
}
static uint16_t sysColor(char s) {
  switch (s) { case 'G': return C_OK; case 'R': return C_BAD; case 'E': return C_ACC; case 'C': return C_YEL;
               case 'J': return C_MAG; case 'I': return C_WARN; }
  return C_DIM;
}

static Sat* satFind(char sys, uint16_t prn, bool create) {
  for (int i = 0; i < s_nSats; i++) if (s_sats[i].sys == sys && s_sats[i].prn == prn) return &s_sats[i];
  if (!create || s_nSats >= MAX_SATS) return nullptr;
  Sat& s = s_sats[s_nSats++];
  s = Sat{sys, prn, -1, -1, -1, false, 0};
  return &s;
}

static void satExpire() {
  uint32_t now = millis();
  int j = 0;
  for (int i = 0; i < s_nSats; i++) if (now - s_sats[i].seen < 10000) s_sats[j++] = s_sats[i];
  s_nSats = j;
}

// Splits "a,b,,c" in place. Returns the field count.
static int splitFields(char* s, char** f, int maxF) {
  int n = 0;
  f[n++] = s;
  for (char* p = s; *p && n < maxF; p++) if (*p == ',') { *p = 0; f[n++] = p + 1; }
  return n;
}

static void parseGsv(char sys, char** f, int n) {
  // f[0]=xxGSV f[1]=msgs f[2]=msg# f[3]=sats in view, then groups of 4, optional signal id last
  for (int i = 4; i + 3 < n; i += 4) {
    if (!*f[i]) continue;
    uint16_t prn = atoi(f[i]);
    Sat* s = satFind(sys, prn, true);
    if (!s) return;
    s->elev = *f[i + 1] ? atoi(f[i + 1]) : -1;
    s->az = *f[i + 2] ? atoi(f[i + 2]) : -1;
    int snr = *f[i + 3] ? atoi(f[i + 3]) : -1;
    if (snr > s->snr || millis() - s->seen > 1500) s->snr = snr;   // several signals per sat: keep the best
    s->seen = millis();
  }
  s_gsvCount++;
}

static void parseGsa(char talkerSys, char** f, int n) {
  // f[1]=A/M f[2]=fix mode f[3..14]=PRNs used f[15]=PDOP f[16]=HDOP f[17]=VDOP f[18]=system id (4.1)
  if (n < 18) return;
  s_fixMode = atoi(f[2]);
  s_pdop = *f[15] ? atof(f[15]) : NAN;
  s_hdop = *f[16] ? atof(f[16]) : NAN;
  s_vdop = *f[17] ? atof(f[17]) : NAN;
  char sys = n >= 19 && *f[18] ? sysFromId(atoi(f[18])) : talkerSys;
  for (int i = 0; i < s_nSats; i++) if (s_sats[i].sys == sys || sys == '?') s_sats[i].used = false;
  for (int i = 3; i <= 14; i++) {
    if (!*f[i]) continue;
    uint16_t prn = atoi(f[i]);
    for (int k = 0; k < s_nSats; k++)
      if (s_sats[k].prn == prn && (sys == '?' || s_sats[k].sys == sys)) s_sats[k].used = true;
  }
  s_gsaCount++;
}

static char s_line[120];
static int s_lineLen = 0;
static void nmeaChar(char c) {
  if (c == '$') s_lineLen = 0;
  if (c == '\r' || c == '\n') {
    if (s_lineLen < 7) { s_lineLen = 0; return; }
    s_line[s_lineLen] = 0;
    s_lineLen = 0;
    if (!grove::nmeaChecksumOk(s_line)) { s_badLines++; return; }
    s_lastSentence = s_line;
    char* star = strchr(s_line, '*');
    if (star) *star = 0;
    char* f[24];
    int n = splitFields(s_line + 1, f, 24);
    if (strlen(f[0]) < 5) return;
    char sys = sysFromTalker(f[0]);
    if (!strcmp(f[0] + 2, "GSV")) parseGsv(sys, f, n);
    else if (!strcmp(f[0] + 2, "GSA")) parseGsa(f[0][1] == 'N' ? '?' : sys, f, n);
    return;
  }
  if (s_lineLen < (int)sizeof(s_line) - 1) s_line[s_lineLen++] = c;
}

// ------------------------------------------------------------------ trip
struct Trip { double dist = 0; float maxKmh = 0; uint32_t movingMs = 0, startMs = 0; double lastLat = NAN, lastLng = NAN; uint32_t lastAt = 0; };
static Trip s_trip;
static void tripUpdate() {
  if (!gps.location.isValid() || gps.location.age() > 2000 || s_fixMode < 2) return;
  float kmh = gps.speed.kmph();
  uint32_t now = millis();
  if (gps.speed.isValid() && kmh > s_trip.maxKmh && (isnan(s_hdop) || s_hdop < 5)) s_trip.maxKmh = kmh;
  double lat = gps.location.lat(), lng = gps.location.lng();
  // Ignore drift while standing still: accumulate only when moving and the geometry is decent
  if (!isnan(s_trip.lastLat) && kmh > 2.0f && (isnan(s_hdop) || s_hdop < 5)) {
    s_trip.dist += TinyGPSPlus::distanceBetween(s_trip.lastLat, s_trip.lastLng, lat, lng);
    s_trip.movingMs += now - s_trip.lastAt;
  }
  s_trip.lastLat = lat; s_trip.lastLng = lng; s_trip.lastAt = now;
}

// ------------------------------------------------------------------ GPX log
static File s_gpx;
static String s_gpxName;
static uint32_t s_gpxPts = 0, s_gpxLastAt = 0;
static String isoTime() {
  if (!gps.date.isValid() || !gps.time.isValid()) return "";
  char b[24];
  snprintf(b, sizeof(b), "%04d-%02d-%02dT%02d:%02d:%02dZ", gps.date.year(), gps.date.month(), gps.date.day(),
           gps.time.hour(), gps.time.minute(), gps.time.second());
  return b;
}
static void gpxClose() {
  if (!s_gpx) return;
  s_gpx.println("</trkseg></trk></gpx>");
  s_gpx.close();
}
static void gpxToggle() {
  if (s_gpx) { gpxClose(); ui::toast("Saved " + s_gpxName + " (" + String(s_gpxPts) + " pts)"); return; }
  if (!ui::sdMount()) { ui::toast("No SD card", C_BAD); return; }
  String path = ui::nextFile("gps", "track", "gpx");
  s_gpx = SD.open(path, FILE_WRITE);
  if (!s_gpx) { ui::toast("SD write failed", C_BAD); return; }
  s_gpxName = path.substring(path.lastIndexOf('/') + 1);
  s_gpx.println("<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
  s_gpx.println("<gpx version=\"1.1\" creator=\"ADV-UnitLab\" xmlns=\"http://www.topografix.com/GPX/1/1\">");
  s_gpx.printf("<trk><name>%s</name><trkseg>\n", s_gpxName.c_str());
  s_gpxPts = 0;
  ui::toast("Logging " + s_gpxName);
}
static void gpxPoint() {
  if (!s_gpx || !gps.location.isValid() || gps.location.age() > 1500 || s_fixMode < 2) return;
  if (millis() - s_gpxLastAt < 1000) return;      // 1 point per second whatever the update rate
  s_gpxLastAt = millis();
  s_gpx.printf("<trkpt lat=\"%.7f\" lon=\"%.7f\">", gps.location.lat(), gps.location.lng());
  if (gps.altitude.isValid()) s_gpx.printf("<ele>%.1f</ele>", gps.altitude.meters());
  String t = isoTime();
  if (t.length()) s_gpx.printf("<time>%s</time>", t.c_str());
  if (gps.satellites.isValid()) s_gpx.printf("<sat>%u</sat>", (unsigned)gps.satellites.value());
  if (!isnan(s_hdop)) s_gpx.printf("<hdop>%.1f</hdop>", s_hdop);
  if (!isnan(s_vdop)) s_gpx.printf("<vdop>%.1f</vdop>", s_vdop);
  if (!isnan(s_pdop)) s_gpx.printf("<pdop>%.1f</pdop>", s_pdop);
  s_gpx.println("</trkpt>");
  if (++s_gpxPts % 10 == 0) s_gpx.flush();
}

// ------------------------------------------------------------------ receiver commands (CASIC)
static int s_rateIdx = 0;
static const uint16_t RATE_MS[] = {1000, 200, 100};
static const char* const RATE_TXT[] = {"1 Hz", "5 Hz", "10 Hz"};
static void sendCmd(const char* body) { Serial1.print(grove::nmeaWithChecksum(body)); }

// ------------------------------------------------------------------ views
static String fmtCoord(double v, bool lat) {
  char h = lat ? (v >= 0 ? 'N' : 'S') : (v >= 0 ? 'E' : 'W');
  double a = fabs(v);
  int d = (int)a;
  double m = (a - d) * 60;
  char b[32];
  snprintf(b, sizeof(b), "%d %07.4f' %c", d, m, h);
  return b;
}
static int usedCount(char sys = 0) {
  int n = 0;
  for (int i = 0; i < s_nSats; i++) if (s_sats[i].used && (!sys || s_sats[i].sys == sys)) n++;
  return n;
}
static const char* fixText() {
  if (!gps.location.isValid() || gps.location.age() > 3000) return "no fix";
  return s_fixMode >= 3 ? "3D fix" : s_fixMode == 2 ? "2D fix" : "fix";
}

static void viewDash() {
  bool fix = gps.location.isValid() && gps.location.age() < 3000;
  int y = TOP + 2;
  cv.setTextSize(3);
  cv.setTextColor(fix ? C_FG : C_DIM, C_BG);
  cv.setCursor(4, y);
  cv.printf("%5.1f", gps.speed.isValid() ? gps.speed.kmph() : 0.0);
  cv.setTextSize(1);
  cv.setTextColor(C_DIM, C_BG); cv.setCursor(98, y + 14); cv.print("km/h");
  cv.setTextColor(fix ? C_OK : C_WARN, C_BG); cv.setCursor(140, y); cv.print(fixText());
  cv.setTextColor(C_FG, C_BG); cv.setCursor(140, y + 10);
  cv.printf("sats %d/%d", usedCount(), s_nSats);
  cv.setCursor(140, y + 20);
  if (gps.course.isValid()) cv.printf("%3.0f %s", gps.course.deg(), TinyGPSPlus::cardinal(gps.course.deg()));
  y += 30;
  cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, y); cv.print("Lat");
  cv.setTextColor(C_FG, C_BG); cv.setCursor(30, y);
  if (fix) cv.printf("%11.6f  %s", gps.location.lat(), fmtCoord(gps.location.lat(), true).c_str()); else cv.print("-");
  y += 10;
  cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, y); cv.print("Lon");
  cv.setTextColor(C_FG, C_BG); cv.setCursor(30, y);
  if (fix) cv.printf("%11.6f  %s", gps.location.lng(), fmtCoord(gps.location.lng(), false).c_str()); else cv.print("-");
  y += 10;
  cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, y); cv.print("Alt");
  cv.setTextColor(C_FG, C_BG); cv.setCursor(30, y);
  if (gps.altitude.isValid()) cv.printf("%.1f m", gps.altitude.meters()); else cv.print("-");
  cv.setTextColor(C_DIM, C_BG); cv.setCursor(120, y); cv.print("HDOP");
  cv.setTextColor(C_FG, C_BG); cv.setCursor(150, y);
  if (!isnan(s_hdop)) cv.printf("%.2f", s_hdop); else cv.print("-");
  y += 10;
  cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, y); cv.print("UTC");
  cv.setTextColor(C_FG, C_BG); cv.setCursor(30, y);
  String t = isoTime();
  cv.print(t.length() ? t : String("-"));
  y += 12;
  cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, y);
  cv.printf("%s  %s", RATE_TXT[s_rateIdx], s_gpx ? "" : "L = start GPX log");
  if (s_gpx) { cv.setTextColor(C_BAD, C_BG); cv.printf("REC %s %lu pts", s_gpxName.c_str(), (unsigned long)s_gpxPts); }
}

static void viewSky() {
  const int cx = 62, cy = TOP + 56, r = 52;
  cv.drawCircle(cx, cy, r, C_SEL);
  cv.drawCircle(cx, cy, r * 2 / 3, 0x18E3);
  cv.drawCircle(cx, cy, r / 3, 0x18E3);
  cv.drawFastHLine(cx - r, cy, 2 * r, 0x18E3);
  cv.drawFastVLine(cx, cy - r, 2 * r, 0x18E3);
  cv.setTextColor(C_DIM, C_BG);
  cv.setCursor(cx - 2, cy - r - 1); cv.print("N");
  for (int i = 0; i < s_nSats; i++) {
    const Sat& s = s_sats[i];
    if (s.elev < 0 || s.az < 0) continue;
    float rr = r * (90 - s.elev) / 90.0f, a = s.az * DEG_TO_RAD;
    int x = cx + (int)(rr * sinf(a)), y = cy - (int)(rr * cosf(a));
    uint16_t c = sysColor(s.sys);
    if (s.used) cv.fillCircle(x, y, 3, c); else cv.drawCircle(x, y, 3, c);
  }
  int y = TOP + 4;
  cv.setTextColor(C_FG, C_BG); cv.setCursor(126, y); cv.print("sys     view used");
  y += 11;
  const char SYS[] = {'G', 'R', 'E', 'C', 'J'};
  for (char sy : SYS) {
    int v = 0;
    for (int i = 0; i < s_nSats; i++) if (s_sats[i].sys == sy) v++;
    cv.fillCircle(129, y + 3, 3, sysColor(sy));
    cv.setTextColor(v ? C_FG : C_DIM, C_BG); cv.setCursor(136, y);
    cv.printf("%-7s %3d %3d", sysName(sy), v, usedCount(sy));
    y += 10;
  }
  y += 4;
  cv.setTextColor(C_DIM, C_BG); cv.setCursor(126, y); cv.print("filled = used in fix");
  y += 10;
  cv.setCursor(126, y); cv.print("rings 0/30/60 deg elev");
  y += 12;
  cv.setTextColor(C_FG, C_BG); cv.setCursor(126, y); cv.print(fixText());
}

static void viewSignal() {
  Sat sorted[MAX_SATS];
  int n = 0;
  for (int i = 0; i < s_nSats; i++) if (s_sats[i].snr >= 0) sorted[n++] = s_sats[i];
  std::sort(sorted, sorted + n, [](const Sat& a, const Sat& b) { return a.snr > b.snr; });
  if (n > 26) n = 26;
  const int base = TOP + 92, h = 82, bw = 9;
  cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, TOP + 1); cv.print("C/N0 dB-Hz, strongest first");
  for (int lvl = 10; lvl <= 50; lvl += 10) {
    int yy = base - lvl * h / 55;
    cv.drawFastHLine(0, yy, SCR_W, 0x10A2);
    cv.setCursor(SCR_W - 12, yy - 8); cv.printf("%d", lvl);
  }
  int sum = 0, used = 0;
  for (int i = 0; i < n; i++) {
    const Sat& s = sorted[i];
    int bh = min(h, s.snr * h / 55);
    int x = 2 + i * bw;
    uint16_t c = sysColor(s.sys);
    if (s.used) cv.fillRect(x, base - bh, bw - 2, bh, c); else cv.drawRect(x, base - bh, bw - 2, max(1, bh), c);
    cv.setTextColor(c, C_BG); cv.setCursor(x + 1, base + 2); cv.print(s.sys);
    if (s.used) { sum += s.snr; used++; }
  }
  cv.setTextColor(C_FG, C_BG); cv.setCursor(4, base + 12);
  if (used) cv.printf("avg of %d used: %d dB-Hz", used, sum / used); else cv.print("no satellites in use yet");
}

static void viewTrip() {
  int y = TOP + 4;
  uint32_t el = (millis() - s_trip.startMs) / 1000;
  auto row = [&](const char* k, const String& v) {
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, y); cv.print(k);
    cv.setTextColor(C_FG, C_BG); cv.setCursor(100, y); cv.print(v);
    y += 13;
  };
  char b[40];
  snprintf(b, sizeof(b), "%.3f km", s_trip.dist / 1000.0); row("Distance", b);
  snprintf(b, sizeof(b), "%.1f km/h", s_trip.maxKmh); row("Max speed", b);
  float avg = s_trip.movingMs ? s_trip.dist / (s_trip.movingMs / 1000.0f) * 3.6f : 0;
  snprintf(b, sizeof(b), "%.1f km/h", avg); row("Avg moving", b);
  snprintf(b, sizeof(b), "%lu:%02lu:%02lu", (unsigned long)(s_trip.movingMs / 3600000UL),
           (unsigned long)(s_trip.movingMs / 60000UL % 60), (unsigned long)(s_trip.movingMs / 1000 % 60)); row("Moving time", b);
  snprintf(b, sizeof(b), "%lu:%02lu:%02lu", (unsigned long)(el / 3600), (unsigned long)(el / 60 % 60), (unsigned long)(el % 60));
  row("Elapsed", b);
  row("GPX log", s_gpx ? s_gpxName + " " + String(s_gpxPts) + " pts" : String("off"));
  cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, y + 2); cv.print("Z = reset trip (counts only when moving)");
}

static int s_dataTop = 0;
static uint32_t s_baud = 0;
static int s_rx = -1, s_tx = -1;
static void buildData(std::vector<String>& L) {
  char b[64];
  auto kv = [&](const char* k, const String& v) { L.push_back(String(k) + "\t" + v); };
  bool fix = gps.location.isValid();
  L.push_back("#Position");
  kv("Fix", fixText());
  if (fix) {
    snprintf(b, sizeof(b), "%.7f", gps.location.lat()); kv("Latitude", b);
    snprintf(b, sizeof(b), "%.7f", gps.location.lng()); kv("Longitude", b);
    kv("Lat DM", fmtCoord(gps.location.lat(), true));
    kv("Lon DM", fmtCoord(gps.location.lng(), false));
    snprintf(b, sizeof(b), "%lu ms", (unsigned long)gps.location.age()); kv("Fix age", b);
  }
  if (gps.altitude.isValid()) { snprintf(b, sizeof(b), "%.1f m", gps.altitude.meters()); kv("Altitude MSL", b); }
  L.push_back("#Motion");
  if (gps.speed.isValid()) {
    snprintf(b, sizeof(b), "%.2f km/h", gps.speed.kmph()); kv("Speed", b);
    snprintf(b, sizeof(b), "%.2f kn  %.2f m/s", gps.speed.knots(), gps.speed.mps()); kv(" ", b);
  }
  if (gps.course.isValid()) { snprintf(b, sizeof(b), "%.1f deg %s", gps.course.deg(), TinyGPSPlus::cardinal(gps.course.deg())); kv("Course", b); }
  L.push_back("#Quality");
  auto f2 = [](float v) { return isnan(v) ? String("-") : String(v, 2); };
  kv("PDOP", f2(s_pdop)); kv("HDOP", f2(s_hdop)); kv("VDOP", f2(s_vdop));
  snprintf(b, sizeof(b), "%d used / %d in view", usedCount(), s_nSats); kv("Satellites", b);
  L.push_back("#Time (UTC)");
  String t = isoTime();
  kv("Date/time", t.length() ? t : String("-"));
  L.push_back("#Satellites  sys prn elev az snr");
  for (int i = 0; i < s_nSats; i++) {
    const Sat& s = s_sats[i];
    snprintf(b, sizeof(b), "%c%-3u %s\t%2d  %3d  %2d dB", s.sys, s.prn, s.used ? "*" : " ", s.elev, s.az, s.snr);
    L.push_back(b);
  }
  L.push_back("#Link");
  snprintf(b, sizeof(b), "%lu baud, RX G%d TX G%d", (unsigned long)s_baud, s_rx, s_tx); kv("UART", b);
  kv("Update rate", RATE_TXT[s_rateIdx]);
  snprintf(b, sizeof(b), "%lu", (unsigned long)gps.charsProcessed()); kv("Chars", b);
  snprintf(b, sizeof(b), "%lu ok / %lu bad", (unsigned long)gps.passedChecksum(), (unsigned long)gps.failedChecksum()); kv("Checksums", b);
  snprintf(b, sizeof(b), "%lu GSV, %lu GSA", (unsigned long)s_gsvCount, (unsigned long)s_gsaCount); kv("Parsed", b);
  kv("Last", s_lastSentence.substring(0, 60));
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
    cv.setTextColor(C_FG, C_BG); cv.setCursor(80, y); cv.print(l.substring(tab + 1));
  }
}

// ------------------------------------------------------------------ section
void appGps() {
  ui::clear();
  ui::header("GPS");
  cv.setTextColor(C_FG, C_BG);
  cv.setCursor(6, TOP + 30); cv.print("Looking for NMEA on Grove...");
  cv.setTextColor(C_DIM, C_BG);
  cv.setCursor(6, TOP + 44); cv.print("115200 / 9600 / 38400, both wire orders");
  ui::push();
  if (!grove::nmeaProbe(s_rx, s_tx, s_baud, 1500)) {
    ui::toast("No GPS data on Grove", C_BAD, 1800);
    return;
  }
  gps = TinyGPSPlus();
  s_nSats = 0; s_fixMode = 1; s_pdop = s_hdop = s_vdop = NAN; s_gsvCount = s_gsaCount = s_badLines = 0;
  s_trip = Trip(); s_trip.startMs = millis();
  s_rateIdx = 0;
  int view = 0;
  bool dirty = true;
  uint32_t lastDraw = 0;
  static const char* const VIEWS[] = {"Dash", "Sky", "Signal", "Trip", "Data"};
  while (true) {
    while (Serial1.available()) {
      char c = Serial1.read();
      gps.encode(c);
      nmeaChar(c);
    }
    if (gps.location.isUpdated()) { tripUpdate(); gpxPoint(); }
    ui::Key k = ui::poll();
    if (k.back) break;
    if (k.any) dirty = true;
    if (k.tab) view = (view + 1) % 5;
    if (k.ch >= '1' && k.ch <= '5') view = k.ch - '1';
    if (k.lc == 'l') gpxToggle();
    if (k.lc == 'z') { s_trip = Trip(); s_trip.startMs = millis(); ui::toast("Trip reset"); }
    if (k.lc == 'r') {
      s_rateIdx = (s_rateIdx + 1) % 3;
      char body[20];
      snprintf(body, sizeof(body), "PCAS02,%u", RATE_MS[s_rateIdx]);
      sendCmd(body);
      ui::toast(String("Rate ") + RATE_TXT[s_rateIdx]);
    }
    if (k.lc == 'c' && ui::confirm("Cold start? Fix is lost for ~30 s")) {
      sendCmd("PCAS10,2");
      s_nSats = 0;
    }
    if (view == 4) {
      if (k.up) s_dataTop--;
      if (k.down) s_dataTop++;
      if (k.left) s_dataTop -= ROWS - 1;
      if (k.right) s_dataTop += ROWS - 1;
      if (k.lc == 's') { std::vector<String> L; buildData(L); ui::saveLines("gps", L); }
    }
    if (millis() - lastDraw > 500) { satExpire(); dirty = true; }
    if (!dirty) { delay(5); continue; }
    dirty = false;
    lastDraw = millis();
    ui::stayAwake();
    ui::clear();
    char hdr[20];
    snprintf(hdr, sizeof(hdr), "GPS: %s", VIEWS[view]);
    ui::header(hdr);
    switch (view) {
      case 0: viewDash(); ui::footer("TAB view L log R rate C cold"); break;
      case 1: viewSky(); ui::footer("TAB view  L log  ` back"); break;
      case 2: viewSignal(); ui::footer("TAB view  L log  ` back"); break;
      case 3: viewTrip(); ui::footer("TAB view  Z reset  L log"); break;
      case 4: viewData(); ui::footer(";/. scroll  S save  TAB view"); break;
    }
    ui::push();
  }
  gpxClose();
  grove::release();
}
