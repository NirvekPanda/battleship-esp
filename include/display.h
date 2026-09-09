#pragma once
#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include "config.h"
#include "soft_ssd1306.h"
#include "../src/game/screen.h"

// Hardware backend for the two panels. Its only job is to push a
// gfx::Screen at each display -- all drawing happens in the portable game
// core, which the WASM host build compiles from the same sources.
//
// Top panel owns the C3's single I2C peripheral; the bottom runs on a
// bit-banged bus, so both modules can keep the same hardwired 0x3C address.
Adafruit_SSD1306 topPanel(SCREEN_W, SCREEN_H, &Wire, -1);
SoftSSD1306 botPanel(S2_SDA, S2_SCL, OLED_ADDR);

bool topOk = false;
bool botOk = false;

// Turn a panel through 180 degrees using the controller's own segment remap
// and COM scan direction. Sent after begin(), which writes the unflipped
// defaults as part of its init sequence. The framebuffer never rotates, so
// displayPush() stays a straight copy whichever way a module is mounted.
static void flipPanel(Adafruit_SSD1306 &p) {
  p.ssd1306_command(0xA0);  // segment remap: column 0 maps to SEG0
  p.ssd1306_command(0xC0);  // COM scan ascending
}

void displayBegin() {
  Wire.begin(I2C_SDA, I2C_SCL, I2C_HZ);

  // periphBegin=false: the bus is already up, don't let the library redo it.
  topOk = topPanel.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR, false, false);
  botOk = botPanel.begin(PANEL2_ROTATION == PANEL_PINS_DOWN);
  if (topOk && PANEL1_ROTATION == PANEL_PINS_DOWN) flipPanel(topPanel);
  Serial.printf("top=%s bot=%s\n", topOk ? "ok" : "FAIL", botOk ? "ok" : "FAIL");

  if (topOk) { topPanel.clearDisplay(); topPanel.display(); }
  if (botOk) { botPanel.clearDisplay(); botPanel.display(); }
}

// ---- Partial updates ----
//
// Sending a frame is what costs, not drawing one. A full 1024-byte panel is
// ~92ms on the 100kHz hardware bus and ~74ms bit-banged, so pushing both
// panels every frame caps the device near 6fps while the render itself takes
// microseconds. Almost nothing changes between frames -- moving the crosshair
// touches a handful of rows -- so each panel keeps a shadow copy of what it
// was last sent, and only the pages that actually differ go out.
//
// A page is a 128-byte band of 8 pixel rows, which is the unit the controller
// addresses; gfx::Screen already stores pixels that way, so page p is simply
// bytes [p*128, p*128+128) and finding the changed ones is a memcmp.
constexpr uint8_t PANEL_PAGES = gfx::H / 8;
constexpr uint16_t PAGE_BYTES = gfx::W;

static uint8_t topShadow[gfx::Screen::size()];
static uint8_t botShadow[gfx::Screen::size()];
static bool shadowValid = false;

// Send one run of pages to the hardware-I2C panel. Adafruit's display()
// always sends all eight, so this addresses the controller directly: a column
// and page window, then the data. The panel is in horizontal addressing mode
// (its init sets 0x20 0x00), so the write wraps within the window on its own.
static void pushPagesHw(const uint8_t *buf, uint8_t first, uint8_t last) {
  Wire.beginTransmission(OLED_ADDR);
  Wire.write(0x00);              // command stream
  Wire.write(0x21); Wire.write(0); Wire.write(gfx::W - 1);
  Wire.write(0x22); Wire.write(first); Wire.write(last);
  Wire.endTransmission();

  // Chunked to stay inside the Wire library's transmit buffer.
  constexpr uint16_t CHUNK = 16;
  const uint16_t end = (last + 1u) * PAGE_BYTES;
  for (uint16_t off = first * PAGE_BYTES; off < end; off += CHUNK) {
    Wire.beginTransmission(OLED_ADDR);
    Wire.write(0x40);            // data stream
    Wire.write(buf + off, CHUNK);
    Wire.endTransmission();
  }
}

// Push the pages of `next` that differ from `shadow`, then bring the shadow
// up to date. Adjacent dirty pages are coalesced into one windowed write,
// since the window setup costs seven bytes and a page is only 128.
template <typename Send>
static void pushDirtyPages(const uint8_t *next, uint8_t *shadow, Send send) {
  int runStart = -1;
  for (uint8_t p = 0; p <= PANEL_PAGES; p++) {
    const bool dirty =
        p < PANEL_PAGES &&
        memcmp(next + p * PAGE_BYTES, shadow + p * PAGE_BYTES, PAGE_BYTES) != 0;
    if (dirty && runStart < 0) runStart = p;
    if (!dirty && runStart >= 0) {
      send((uint8_t)runStart, (uint8_t)(p - 1));
      memcpy(shadow + runStart * PAGE_BYTES, next + runStart * PAGE_BYTES,
             (p - runStart) * PAGE_BYTES);
      runStart = -1;
    }
  }
}

void displayPush(const gfx::Screen &top, const gfx::Screen &bot) {
  static_assert(gfx::W == SCREEN_W && gfx::H == SCREEN_H,
                "gfx::Screen and the panel geometry must agree");
  static_assert(gfx::Screen::size() == PANEL_PAGES * PAGE_BYTES,
                "a Screen must be a whole number of 128-byte pages");

  // The panels were cleared by displayBegin(), so an all-zero shadow is a
  // true picture of them and the first push sends only what is actually lit.
  if (!shadowValid) {
    memset(topShadow, 0, sizeof(topShadow));
    memset(botShadow, 0, sizeof(botShadow));
    shadowValid = true;
  }

  if (topOk) {
    const uint8_t *src = top.buffer();
    pushDirtyPages(src, topShadow,
                   [&](uint8_t a, uint8_t b) { pushPagesHw(src, a, b); });
  }
  if (botOk) {
    const uint8_t *src = bot.buffer();
    pushDirtyPages(src, botShadow, [&](uint8_t a, uint8_t b) {
      memcpy(botPanel.buffer() + a * PAGE_BYTES, src + a * PAGE_BYTES,
             (b - a + 1) * PAGE_BYTES);
      botPanel.displayPages(a, b);
    });
  }
}
