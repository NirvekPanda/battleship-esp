#pragma once
#include <Arduino.h>
#include <Wire.h>
#include "config.h"
#include "display.h"

// A boot-time self-test for the wiring, printed as a table over serial.
//
// It exists because every failure mode below looks identical from the couch:
// a board that boots to two blank panels. The panels cannot report their own
// absence, and a direction that never fires is indistinguishable from one
// nobody pressed. So the firmware says, once per boot, exactly what it can
// see on each pin it was told to use.
//
// Nothing here is diagnostic-only scaffolding to be stripped later -- it runs
// on every boot and costs a few milliseconds, which is cheaper than owning a
// logic analyser.

// ---- Compile-time guards ----
//
// The pin map came from a C3, where these numbers meant something else
// entirely. On the classic ESP32, GPIO 6-11 are the SPI flash: a firmware
// that claims one does not misbehave at runtime, it fails to boot, with no
// serial output to say why. That is the worst possible failure to debug, so
// it is caught at compile time instead.
#define HC_IS_FLASH_PIN(p) ((p) >= 6 && (p) <= 11)
static_assert(!HC_IS_FLASH_PIN(I2C_SDA) && !HC_IS_FLASH_PIN(I2C_SCL) &&
              !HC_IS_FLASH_PIN(S2_SDA) && !HC_IS_FLASH_PIN(S2_SCL),
              "an I2C pin is on the SPI flash (GPIO6-11); the board will not boot");
static_assert(!HC_IS_FLASH_PIN(BTN_A) && !HC_IS_FLASH_PIN(BTN_B) &&
              !HC_IS_FLASH_PIN(BTN_C) && !HC_IS_FLASH_PIN(BTN_D) &&
              !HC_IS_FLASH_PIN(BTN_CENTER),
              "a button pin is on the SPI flash (GPIO6-11); the board will not boot");

// Input-only pins have no internal pullup, so a button on one floats and
// reads as a press whenever it feels like it.
#define HC_IS_INPUT_ONLY(p) ((p) >= 34 && (p) <= 39)
static_assert(!HC_IS_INPUT_ONLY(BTN_A) && !HC_IS_INPUT_ONLY(BTN_B) &&
              !HC_IS_INPUT_ONLY(BTN_C) && !HC_IS_INPUT_ONLY(BTN_D) &&
              !HC_IS_INPUT_ONLY(BTN_CENTER),
              "a button pin is input-only (GPIO34-39) and has no pullup to hold it high");

// UART0 is the only console this board has -- there is no native USB.
static_assert(I2C_SDA != 1 && I2C_SCL != 1 && S2_SDA != 1 && S2_SCL != 1 &&
              I2C_SDA != 3 && I2C_SCL != 3 && S2_SDA != 3 && S2_SCL != 3,
              "an I2C pin is on UART0 (GPIO1/3); this would silence the serial monitor");

// ---- Runtime probes ----

static bool hcAllOk = true;

static void hcRow(const char *what, bool ok, const char *detail) {
  if (!ok) hcAllOk = false;
  Serial.printf("  %-18s %-4s %s\n", what, ok ? "ok" : "FAIL", detail);
}

// Walk the hardware bus and report every address that answers. The addresses
// matter as much as the pass/fail: 0x3C with nothing else is the expected
// picture, an empty bus means the panel is unpowered or unwired, and a
// DIFFERENT address means the module's ADDR jumper is set the other way
// (0x3D) rather than anything being broken.
static void hcHardwareBus() {
  uint8_t found[8];
  uint8_t n = 0;
  for (uint8_t a = 0x08; a < 0x78 && n < 8; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) found[n++] = a;
  }

  char detail[96];
  int off = snprintf(detail, sizeof(detail), "SDA=%d SCL=%d, saw", I2C_SDA, I2C_SCL);
  if (n == 0) {
    snprintf(detail + off, sizeof(detail) - off, " nothing");
  } else {
    for (uint8_t i = 0; i < n && off < (int)sizeof(detail) - 8; i++)
      off += snprintf(detail + off, sizeof(detail) - off, " 0x%02X", found[i]);
  }

  bool sawPanel = false;
  for (uint8_t i = 0; i < n; i++) if (found[i] == OLED_ADDR) sawPanel = true;
  hcRow("top panel bus", sawPanel, detail);
}

// The bit-banged bus cannot be scanned the same way -- there is no controller
// to report a bus error, only whether our own bit-banging saw an ACK. Check
// the lines idle high first: a line stuck low never produces a START, so
// every probe would fail and blame the panel for a wiring fault.
static void hcSoftBus() {
  char detail[80];
  const bool sda = botPanel.sdaIdleHigh();
  const bool scl = botPanel.sclIdleHigh();

  if (!sda || !scl) {
    snprintf(detail, sizeof(detail), "SDA=%d SCL=%d, %s stuck LOW (short, or no pullup)",
             S2_SDA, S2_SCL, !sda ? (!scl ? "both lines" : "SDA") : "SCL");
    hcRow("bottom panel bus", false, detail);
    return;
  }

  const bool ack = botPanel.probe();
  snprintf(detail, sizeof(detail), "SDA=%d SCL=%d, 0x%02X %s", S2_SDA, S2_SCL,
           OLED_ADDR, ack ? "ACKed" : "did not answer");
  hcRow("bottom panel bus", ack, detail);
}

// Every direction must idle HIGH: the switch commons to GND and the pins are
// INPUT_PULLUP, so a pin reading LOW at boot is either held down or shorted.
// Reported per-direction rather than as one verdict, because "which one" is
// the whole question when a single direction is dead.
static void hcButtons() {
  struct { const char *name; uint8_t pin; } b[] = {
    {"btn A (up)", BTN_A}, {"btn B (right)", BTN_B}, {"btn C (down)", BTN_C},
    {"btn D (left)", BTN_D}, {"btn centre", BTN_CENTER},
  };
  for (auto &e : b) {
    // controlsBegin() already set the pullups; re-assert so the check does
    // not depend on having been called after it.
    pinMode(e.pin, INPUT_PULLUP);
    delayMicroseconds(50);
    const bool idleHigh = digitalRead(e.pin) == HIGH;
    char detail[64];
    snprintf(detail, sizeof(detail), "GPIO%-2d reads %s", e.pin,
             idleHigh ? "HIGH (idle)" : "LOW -- held down or shorted to GND");
    hcRow(e.name, idleHigh, detail);
  }
}

// Run every probe and print the table. Returns true if everything passed.
//
// Call it after displayBegin(), so the hardware bus is already up: this
// reports on the wiring, it does not configure it.
bool healthCheck() {
  hcAllOk = true;
  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, STATUS_LED_OFF);

  Serial.println();
  Serial.println("health check");
  hcHardwareBus();
  hcSoftBus();
  hcButtons();
  Serial.printf("  -> %s\n\n", hcAllOk ? "all peripherals present"
                                       : "SOMETHING IS WRONG (see FAIL rows above)");

  // Say it on the board too, for a boot with no monitor attached: a steady
  // LED is a pass, a slow blink is a fail. The blink is bounded -- it must
  // not become a device that never reaches the game.
  if (hcAllOk) {
    digitalWrite(STATUS_LED, STATUS_LED_ON);
  } else {
    for (int i = 0; i < 6; i++) {
      digitalWrite(STATUS_LED, i % 2 ? STATUS_LED_OFF : STATUS_LED_ON);
      delay(150);
    }
    digitalWrite(STATUS_LED, STATUS_LED_OFF);
  }
  return hcAllOk;
}
