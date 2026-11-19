// Main-menu rendering: large vector icons, big titles and full-width descriptions (from SensorLab).
// Depends only on a LovyanGFX-style canvas so it can also be rendered on a PC for previews.
#pragma once
#include <math.h>
#include <stdint.h>

struct MenuItem { const char* name; const char* desc; uint16_t color; uint8_t icon; };

// Menu order. `icon` selects the drawing in drawMenuIcon(), independent of the position.
static const MenuItem MENU_ITEMS[] = {
  {"ENV-Pro", "U169 climate, IAQ, alarms", 0x07E0, 20},
  {"GPS", "U032 position, speed, sky, GPX", 0xFD20, 21},
  {"Infrared", "U002 capture, save, replay .ir", 0xF800, 11},
  {"NFC / RFID", "U216 / Cap / RFID2 read, emulate", 0x5D1F, 22},
  {"Detect", "What is on the Grove port", 0x07FF, 8},
  {"Settings", "Screen, volume, ENV offsets", 0xBDF7, 16},
};
static const int MENU_COUNT = sizeof(MENU_ITEMS) / sizeof(MENU_ITEMS[0]);

// 28 x 28 icon with its top-left corner at (x, y). c = icon colour, bg = colour behind it.
template <class G>
void drawMenuIcon(G& g, int id, int x, int y, uint16_t c, uint16_t bg) {
  auto thick = [&](int x0, int y0, int x1, int y1) {
    g.drawLine(x + x0, y + y0, x + x1, y + y1, c);
    g.drawLine(x + x0 + 1, y + y0, x + x1 + 1, y + y1, c);
    g.drawLine(x + x0, y + y0 + 1, x + x1, y + y1 + 1, c);
  };
  switch (id) {
    case 0:   // microphone
      g.fillRoundRect(x + 9, y + 1, 10, 16, 5, c);
      g.fillArc(x + 14, y + 12, 8, 10, 0, 180, c);
      g.fillRect(x + 13, y + 21, 3, 4, c);
      g.fillRect(x + 8, y + 25, 13, 2, c);
      break;
    case 1:   // motion: three axes
      thick(11, 17, 11, 3); thick(11, 17, 25, 17); thick(11, 17, 3, 25);
      g.fillTriangle(x + 11, y, x + 7, y + 6, x + 16, y + 6, c);
      g.fillTriangle(x + 28, y + 17, x + 22, y + 13, x + 22, y + 22, c);
      g.fillTriangle(x + 1, y + 27, x + 2, y + 20, x + 8, y + 26, c);
      g.fillCircle(x + 11, y + 17, 3, c);
      break;
    case 2:   // wifi fan
      g.fillArc(x + 14, y + 24, 20, 23, 222, 318, c);
      g.fillArc(x + 14, y + 24, 13, 16, 222, 318, c);
      g.fillArc(x + 14, y + 24, 6, 9, 222, 318, c);
      g.fillCircle(x + 14, y + 24, 3, c);
      break;
    case 3:   // radar sweep
      g.drawCircle(x + 14, y + 14, 13, c);
      g.drawCircle(x + 14, y + 14, 12, c);
      g.drawCircle(x + 14, y + 14, 7, c);
      g.fillArc(x + 14, y + 14, 0, 12, 285, 335, c);
      g.fillCircle(x + 8, y + 19, 2, c);
      g.fillCircle(x + 19, y + 21, 1, c);
      g.fillCircle(x + 14, y + 14, 2, c);
      break;
    case 4:   // bluetooth rune
      thick(13, 1, 13, 26);
      thick(13, 1, 21, 8); thick(21, 8, 5, 20);
      thick(13, 26, 21, 19); thick(21, 19, 5, 7);
      break;
    case 5:   // battery
      g.fillRoundRect(x + 1, y + 7, 23, 15, 3, c);
      g.fillRect(x + 3, y + 9, 19, 11, bg);
      g.fillRect(x + 24, y + 11, 3, 7, c);
      g.fillRect(x + 5, y + 11, 4, 7, c);
      g.fillRect(x + 10, y + 11, 4, 7, c);
      g.fillRect(x + 15, y + 11, 4, 7, c);
      break;
    case 6:   // header pins
      g.fillRoundRect(x + 2, y + 9, 24, 11, 2, c);
      for (int i = 0; i < 5; i++) {
        g.fillRect(x + 4 + i * 4 + (i > 0), y + 12, 2, 5, bg);
        g.fillRect(x + 4 + i * 5, y + 2, 2, 7, c);
        g.fillRect(x + 4 + i * 5, y + 20, 2, 6, c);
      }
      break;
    case 7:   // CPU
      g.fillRoundRect(x + 5, y + 5, 18, 18, 2, c);
      g.fillRect(x + 8, y + 8, 12, 12, bg);
      g.fillRect(x + 11, y + 11, 6, 6, c);
      for (int i = 0; i < 4; i++) {
        g.fillRect(x + 7 + i * 4, y + 1, 2, 4, c);
        g.fillRect(x + 7 + i * 4, y + 23, 2, 4, c);
        g.fillRect(x + 1, y + 7 + i * 4, 4, 2, c);
        g.fillRect(x + 23, y + 7 + i * 4, 4, 2, c);
      }
      break;
    case 8:   // bus with three nodes
      g.fillRect(x + 1, y + 12, 26, 2, c);
      g.fillRect(x + 1, y + 16, 26, 2, c);
      g.fillRoundRect(x + 2, y + 1, 7, 8, 1, c);  g.fillRect(x + 5, y + 9, 2, 3, c);
      g.fillRoundRect(x + 19, y + 1, 7, 8, 1, c); g.fillRect(x + 22, y + 9, 2, 7, c);
      g.fillRoundRect(x + 10, y + 21, 8, 7, 1, c); g.fillRect(x + 13, y + 18, 2, 3, c);
      break;
    case 9:   // SD card
      g.fillRoundRect(x + 5, y + 1, 18, 26, 2, c);
      g.fillTriangle(x + 5, y + 1, x + 12, y + 1, x + 5, y + 8, bg);
      for (int i = 0; i < 3; i++) g.fillRect(x + 12 + i * 4, y + 4, 2, 6, bg);
      g.fillRect(x + 8, y + 16, 12, 7, bg);
      break;
    case 10:  // keyboard
      g.fillRoundRect(x, y + 6, 28, 17, 2, c);
      for (int r = 0; r < 2; r++)
        for (int i = 0; i < 6; i++) g.fillRect(x + 3 + i * 4, y + 9 + r * 4, 2, 2, bg);
      g.fillRect(x + 7, y + 18, 14, 2, bg);
      break;
    case 11:  // IR emitter with rays
      g.fillCircle(x + 9, y + 15, 7, c);
      g.fillRect(x + 2, y + 15, 15, 8, c);
      g.fillRect(x + 5, y + 23, 2, 4, c);
      g.fillRect(x + 12, y + 23, 2, 4, c);
      thick(19, 9, 25, 4); thick(20, 15, 27, 15); thick(19, 21, 25, 25);
      break;
    case 13:  // loudspeaker with sound waves
      g.fillRect(x + 2, y + 10, 7, 9, c);
      g.fillTriangle(x + 8, y + 10, x + 15, y + 3, x + 15, y + 25, c);
      g.fillTriangle(x + 8, y + 10, x + 8, y + 18, x + 15, y + 25, c);
      g.fillArc(x + 15, y + 14, 5, 7, 310, 410, c);
      g.fillArc(x + 15, y + 14, 10, 12, 310, 410, c);
      break;
    case 14:  // text page with a pencil
      g.fillRoundRect(x + 3, y + 1, 17, 26, 2, c);
      g.fillRect(x + 6, y + 6, 11, 2, bg);
      g.fillRect(x + 6, y + 11, 11, 2, bg);
      g.fillRect(x + 6, y + 16, 7, 2, bg);
      g.fillRect(x + 6, y + 21, 5, 2, bg);
      for (int i = 0; i < 4; i++) g.drawLine(x + 15 + i, y + 22, x + 23 + i, y + 10, c);
      g.fillTriangle(x + 13, y + 26, x + 15, y + 21, x + 18, y + 24, c);
      break;
    case 15:  // target crosshair
      g.fillArc(x + 14, y + 14, 10, 12, 0, 360, c);
      g.fillArc(x + 14, y + 14, 4, 6, 0, 360, c);
      g.fillRect(x + 13, y, 2, 7, c);
      g.fillRect(x + 13, y + 21, 2, 7, c);
      g.fillRect(x, y + 13, 7, 2, c);
      g.fillRect(x + 21, y + 13, 7, 2, c);
      g.fillRect(x + 13, y + 13, 2, 2, c);
      break;
    case 16:  // gear
      for (int i = 0; i < 8; i++) {
        float a = i * 0.7854f;
        int cx = x + 14 + (int)(11.0f * cosf(a)), cy = y + 14 + (int)(11.0f * sinf(a));
        g.fillCircle(cx, cy, 3, c);
      }
      g.fillCircle(x + 14, y + 14, 10, c);
      g.fillCircle(x + 14, y + 14, 4, bg);
      break;
    case 20:  // thermometer with a droplet
      g.fillRoundRect(x + 5, y + 1, 7, 18, 3, c);
      g.fillRect(x + 7, y + 3, 3, 14, bg);
      g.fillRect(x + 7, y + 9, 3, 10, c);
      g.fillCircle(x + 8, y + 22, 5, c);
      g.fillTriangle(x + 21, y + 6, x + 16, y + 16, x + 26, y + 16, c);
      g.fillCircle(x + 21, y + 18, 5, c);
      break;
    case 21:  // map pin over a ground ring
      g.fillCircle(x + 14, y + 9, 8, c);
      g.fillTriangle(x + 7, y + 13, x + 21, y + 13, x + 14, y + 25, c);
      g.fillCircle(x + 14, y + 9, 3, bg);
      g.drawEllipse(x + 14, y + 25, 12, 3, c);
      break;
    case 22:  // contactless card with radio waves
      g.fillRoundRect(x + 1, y + 7, 15, 18, 2, c);
      g.fillRoundRect(x + 3, y + 9, 11, 6, 1, bg);
      g.fillArc(x + 20, y + 16, 4, 6, 300, 420, c);
      g.fillArc(x + 20, y + 16, 9, 11, 300, 420, c);
      g.fillArc(x + 20, y + 16, 14, 16, 300, 420, c);
      break;
    default:  // display
      g.fillRoundRect(x + 1, y + 3, 26, 18, 2, c);
      g.fillRect(x + 3, y + 5, 22, 14, bg);
      g.drawLine(x + 6, y + 15, x + 13, y + 8, c);
      g.drawLine(x + 11, y + 16, x + 16, y + 11, c);
      g.fillRect(x + 12, y + 21, 4, 3, c);
      g.fillRect(x + 7, y + 24, 14, 2, c);
      break;
  }
}

// Draws the menu body between y = 12 and y = 124 on a 240 x 135 canvas: three large rows.
static const int MENU_ROWS = 3;
template <class G>
void drawMenuBody(G& g, int sel, int top) {
  const int W = 240, Y0 = 12, RH = 37;
  for (int r = 0; r < MENU_ROWS && top + r < MENU_COUNT; r++) {
    const MenuItem& it = MENU_ITEMS[top + r];
    int y = Y0 + 1 + r * RH;
    bool on = top + r == sel;
    uint16_t bg = on ? 0x2145 : 0x0000;
    if (on) {
      g.fillRoundRect(1, y, W - 9, RH - 2, 5, bg);
      g.drawRoundRect(1, y, W - 9, RH - 2, 5, it.color);
    }
    // icon tile
    uint16_t tile = on ? it.color : 0x18E3;
    uint16_t ink = on ? 0x0000 : it.color;
    g.fillRoundRect(5, y + 2, 34, 31, 5, tile);
    drawMenuIcon(g, it.icon, 8, y + 3, ink, tile);
    // title (double size) and description, vertically centred against the 31px icon tile:
    // 16px title + 2px gap + 7px desc = 25px block, centred in the 35px row -> starts at y+5
    g.setTextSize(2);
    g.setTextColor(on ? 0xFFFF : 0xCE79);
    g.setCursor(45, y + 5);
    g.print(it.name);
    g.setTextSize(1);
    g.setTextColor(on ? it.color : 0x8410);
    g.setCursor(46, y + 24);
    g.print(it.desc);
    // index number, aligned with the title
    g.setTextColor(on ? 0xFFFF : 0x528A);
    g.setCursor(W - 9 - 15, y + 7);
    g.printf("%2d", top + r + 1);
  }
  // scrollbar
  int trackH = 112, sbh = trackH * MENU_ROWS / MENU_COUNT;
  int sby = Y0 + (trackH - sbh) * top / (MENU_COUNT - MENU_ROWS);
  g.fillRect(W - 5, Y0, 4, trackH, 0x18E3);
  g.fillRect(W - 5, sby, 4, sbh, 0x05FF);
}
