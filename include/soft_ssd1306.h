#pragma once
#include <Arduino.h>
#include <Adafruit_GFX.h>

// A 128x64 SSD1306 driven over bit-banged I2C, so it can live on any pin
// pair. The C3 has only one hardware I2C controller (SOC_I2C_NUM == 1),
// which the top panel owns; this gives the bottom panel its own bus even
// though both modules are hardwired to the same 0x3C address.
//
// Open-drain signalling: release a line by making it an input (the module's
// pullups take it high), assert it by driving it low.
class SoftSSD1306 : public Adafruit_GFX {
public:

  SoftSSD1306(uint8_t sda, uint8_t scl, uint8_t addr = 0x3C)
      : Adafruit_GFX(128, 64), _sda(sda), _scl(scl), _addr(addr) {}

  // flip turns the glass through 180 degrees, for a module mounted pins-down.
  // It is the controller's own segment remap and COM scan direction, so the
  // framebuffer is untouched and a frame still costs one straight copy.
  bool begin(bool flip = false) {
    release(_sda); release(_scl);
    static const uint8_t init[] = {
      0xAE,             // display off
      0xD5, 0x80,       // clock div
      0xA8, 0x3F,       // multiplex = 63
      0xD3, 0x00,       // display offset
      0x40,             // start line 0
      0x8D, 0x14,       // charge pump on
      0x20, 0x00,       // horizontal addressing mode
      0xA1,             // segment remap
      0xC8,             // COM scan descending
      0xDA, 0x12,       // COM pins
      0x81, 0xCF,       // contrast
      0xD9, 0xF1,       // precharge
      0xDB, 0x40,       // VCOM detect
      0xA4,             // resume from RAM
      0xA6,             // normal (not inverted)
      0xAF,             // display on
    };
    if (!start()) return false;
    if (!wr(_addr << 1)) { stop(); return false; }
    wr(0x00);  // Co=0, D/C=0 -> command stream
    for (uint8_t c : init) wr(c);
    stop();

    // The flip overrides the two orientation bytes the list just sent, rather
    // than being patched into it by index: the list is a plain run of opcodes
    // and operands, so an index into it silently rots the moment a command
    // with an operand is added above.
    if (flip) setFlipped(true);
    return true;
  }

  // Turn the glass through 180 degrees. Segment remap and COM scan direction
  // together mirror both axes, which is the controller doing the rotation for
  // free -- the framebuffer is never touched, so a frame stays one copy.
  void setFlipped(bool flip) {
    cmd(flip ? 0xA0 : 0xA1);
    cmd(flip ? 0xC0 : 0xC8);
  }

  static constexpr uint8_t PAGES = 8;  // 64 rows of 8-pixel pages

  void clearDisplay() { memset(_buf, 0, sizeof(_buf)); }

  // Page-format framebuffer, so a gfx::Screen can be memcpy'd straight in.
  uint8_t *buffer() { return _buf; }

  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    if (x < 0 || x >= 128 || y < 0 || y >= 64) return;
    uint16_t i = x + (y / 8) * 128;
    if (color) _buf[i] |= (1 << (y & 7));
    else       _buf[i] &= ~(1 << (y & 7));
  }

  void display() { displayPages(0, PAGES - 1); }

  // Send only pages first..last. A bit-banged byte costs about 70us, so a
  // frame that only moves the crosshair has no business paying for the eight
  // pages that did not change.
  void displayPages(uint8_t first, uint8_t last) {
    if (first > last || last >= PAGES) return;
    start();
    wr(_addr << 1);
    wr(0x00);
    wr(0x21); wr(0); wr(127);       // column range
    wr(0x22); wr(first); wr(last);  // page range
    stop();

    // Data in chunks so a stretch of buffer goes out per transaction.
    for (uint16_t off = first * 128u; off < (last + 1u) * 128u; off += 128) {
      start();
      wr(_addr << 1);
      wr(0x40);  // Co=0, D/C=1 -> data stream
      for (uint16_t i = 0; i < 128; i++) wr(_buf[off + i]);
      stop();
    }
  }

private:
  // One command byte, in its own transaction.
  void cmd(uint8_t c) {
    start();
    wr(_addr << 1);
    wr(0x00);  // Co=0, D/C=0 -> command stream
    wr(c);
    stop();
  }

  uint8_t _sda, _scl, _addr;
  uint8_t _buf[128 * 64 / 8];

  static inline void release(uint8_t p) { pinMode(p, INPUT_PULLUP); }
  static inline void drive(uint8_t p)   { pinMode(p, OUTPUT); digitalWrite(p, LOW); }
  inline void tick() { delayMicroseconds(2); }

  // Let SCL rise, honouring clock stretching by a slow peripheral.
  bool sclHigh() {
    release(_scl);
    tick();
    for (int i = 0; i < 1000 && digitalRead(_scl) == LOW; i++) delayMicroseconds(1);
    return digitalRead(_scl) != LOW;
  }

  bool start() {
    release(_sda);
    if (!sclHigh()) return false;
    drive(_sda); tick();   // SDA falls while SCL high
    drive(_scl); tick();
    return true;
  }

  void stop() {
    drive(_sda); tick();
    sclHigh();
    release(_sda); tick(); // SDA rises while SCL high
  }

  // Write one byte, return true if the peripheral ACKed.
  bool wr(uint8_t b) {
    for (uint8_t m = 0x80; m; m >>= 1) {
      if (b & m) release(_sda); else drive(_sda);
      tick();
      if (!sclHigh()) return false;
      drive(_scl); tick();
    }
    release(_sda);           // hand SDA over for the ACK bit
    tick();
    if (!sclHigh()) return false;
    bool ack = (digitalRead(_sda) == LOW);
    drive(_scl); tick();
    return ack;
  }
};
