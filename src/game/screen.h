#pragma once
#include <stdint.h>
#include <string.h>

// One 128x64 monochrome panel. The two displays are separate devices on
// separate buses, so each gets its own Screen with its own 0..63 coordinate
// space -- nothing here knows the other panel exists.
//
// Storage matches the SSD1306's page format: byte (x + (y/8)*W) holds 8
// vertically-stacked pixels, LSB topmost. Pushing a Screen to a panel is
// therefore a straight 1024-byte memcpy with no repacking.
namespace gfx {

constexpr int W = 128;
constexpr int H = 64;
constexpr int BUF_BYTES = W * H / 8;  // 1024

class Screen {
public:
  void clear() { memset(_buf, 0, sizeof(_buf)); }
  void fill()  { memset(_buf, 0xFF, sizeof(_buf)); }

  void pixel(int x, int y, bool on) {
    if (x < 0 || x >= W || y < 0 || y >= H) return;
    uint8_t &b = _buf[x + (y >> 3) * W];
    const uint8_t m = 1 << (y & 7);
    if (on) b |= m; else b &= ~m;
  }

  bool pixelAt(int x, int y) const {
    if (x < 0 || x >= W || y < 0 || y >= H) return false;
    return _buf[x + (y >> 3) * W] & (1 << (y & 7));
  }

  void hLine(int x, int y, int w, bool on) { for (int i = 0; i < w; i++) pixel(x + i, y, on); }
  void vLine(int x, int y, int h, bool on) { for (int i = 0; i < h; i++) pixel(x, y + i, on); }

  void fillRect(int x, int y, int w, int h, bool on) {
    for (int j = 0; j < h; j++) hLine(x, y + j, w, on);
  }

  // Flip every pixel in a region. Used to highlight a label strip without
  // having to redraw the glyphs in it.
  void invertRect(int x, int y, int w, int h) {
    for (int j = 0; j < h; j++)
      for (int i = 0; i < w; i++) pixel(x + i, y + j, !pixelAt(x + i, y + j));
  }

  void drawRect(int x, int y, int w, int h, bool on) {
    if (w <= 0 || h <= 0) return;
    hLine(x, y, w, on); hLine(x, y + h - 1, w, on);
    vLine(x, y, h, on); vLine(x + w - 1, y, h, on);
  }

  // 5x7 glyphs on a 6x8 cell. Returns the x just past the drawn text.
  int text(int x, int y, const char *s, bool on = true);
  static int textWidth(const char *s);

  // Same font, every pixel blown up to scale x scale. Cell is 6*scale wide.
  // Used for the HIT / MISS overlays, which have to read at a glance.
  int textScaled(int x, int y, const char *s, int scale, bool on = true);
  static int textScaledWidth(const char *s, int scale) { return textWidth(s) * scale; }

  // Outlined ("bubble") text: the scaled glyph hollowed out, leaving a 1px
  // outline around the letter shape. Reads as large display type where a
  // solid blown-up glyph would just look blocky.
  int textBubble(int x, int y, const char *s, int scale, bool on = true);

  // Compact 3x5 digits and capitals on a 4x6 cell, for grid labels: a 5px
  // grid row has no space for a 7px glyph. Only 0-9 and A-Z are defined;
  // anything else draws blank.
  int textTiny(int x, int y, const char *s, bool on = true);
  static int textTinyWidth(const char *s);

  const uint8_t *buffer() const { return _buf; }
  uint8_t *buffer() { return _buf; }
  static constexpr int size() { return BUF_BYTES; }

private:
  uint8_t _buf[BUF_BYTES];
};

// Where a run of text starts if it is to sit in the middle of a 128px panel.
//
// Centred on the INK, not on the advance: a glyph cell is 6px wide and the
// glyph itself is 5, so the last cell's trailing gap is never drawn. Centring
// on the advance leaves every line half a cell to the left -- two and a half
// pixels for a digit drawn at scale 5, which is the whole of what is on the
// panel while a countdown runs.
int centerScaledX(const char *s, int scale);
int centerTextX(const char *s);
// The same rule for the 3x5 font, between two x bounds -- a gutter, usually.
int centerTinyIn(const char *s, int left, int right);

// A tick and a cross, at an arbitrary size, for yes/no indicators. Drawn from
// lines rather than set as glyphs because neither exists in either font.
void drawTick(Screen &s, int x, int y, int size, bool on = true);
void drawCross(Screen &s, int x, int y, int size, bool on = true);

}  // namespace gfx
