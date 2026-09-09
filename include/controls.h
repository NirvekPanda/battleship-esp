#pragma once
#include <Arduino.h>
#include "config.h"

// Direction inputs come from two sources that OR together: the physical
// 5-way switch, and keystrokes on the serial monitor (arrow keys + space).
// The keyboard path exists so the Wokwi emulator is usable without clicking
// buttons; on real hardware nothing is typed and it simply stays quiet.
//
// A keystroke is an event, not a held state, so each one latches its
// direction for KEY_HOLD_MS -- long enough for the loop to act on it.
#define KEY_HOLD_MS 120

// Declared clockwise from up. The order is load-bearing: BUTTON_ROTATION is
// applied as a shift along it, so a 90-degree turn is +1.
enum Dir { DIR_UP = 0, DIR_RIGHT, DIR_DOWN, DIR_LEFT, DIR_CENTER, DIR_COUNT };

static uint32_t keyUntil[DIR_COUNT];

void controlsBegin() {
  pinMode(BTN_A, INPUT_PULLUP);
  pinMode(BTN_B, INPUT_PULLUP);
  pinMode(BTN_C, INPUT_PULLUP);
  pinMode(BTN_D, INPUT_PULLUP);
  pinMode(BTN_CENTER, INPUT_PULLUP);
  for (int i = 0; i < DIR_COUNT; i++) keyUntil[i] = 0;
}

static void latch(Dir d) { keyUntil[d] = millis() + KEY_HOLD_MS; }

// Drain the serial input. Arrow keys arrive as the ANSI sequence
// ESC '[' 'A'..'D'; WASD and space are accepted too, since not every
// terminal forwards the escape sequence.
void controlsPoll() {
  static uint8_t esc = 0;  // how much of ESC '[' we have seen
  while (Serial.available()) {
    int c = Serial.read();
    if (esc == 1) { esc = (c == '[') ? 2 : 0; continue; }
    if (esc == 2) {
      esc = 0;
      switch (c) {
        case 'A': latch(DIR_UP);    break;
        case 'B': latch(DIR_DOWN);  break;
        case 'C': latch(DIR_RIGHT); break;
        case 'D': latch(DIR_LEFT);  break;
      }
      continue;
    }
    switch (c) {
      case 0x1B: esc = 1;            break;
      case ' ':  latch(DIR_CENTER);  break;
      case 'w': case 'W': latch(DIR_UP);    break;
      case 's': case 'S': latch(DIR_DOWN);  break;
      case 'a': case 'A': latch(DIR_LEFT);  break;
      case 'd': case 'D': latch(DIR_RIGHT); break;
    }
  }
}

// buttons pull LOW when pressed
static bool pressed(uint8_t pin, Dir d) {
  if (digitalRead(pin) == LOW) return true;
  return (int32_t)(keyUntil[d] - millis()) > 0;
}

// The four direction pins in clockwise order, which is the order Dir itself
// is declared in -- that is what makes a rotation a shift along this array.
static const uint8_t DIR_PIN[4] = {BTN_A, BTN_B, BTN_C, BTN_D};

// Report a logical direction, accounting for how the switch is mounted.
// BUTTON_ROTATION counts 90-degree steps clockwise, so a switch turned one
// step puts the button wired as "left" where "up" now points: step backwards
// around the ring to find which pin currently points the way we are asking
// about.
//
// Only the pin is rotated. A key typed at the serial monitor already means
// the direction it names, so it stays indexed by the logical direction and is
// deliberately left out of the mapping -- rotating it too would cancel out and
// leave the keyboard steering the old orientation.
static bool dirPressed(Dir logical) {
  const int phys = ((int)logical - BUTTON_ROTATION + 4) % 4;
  return pressed(DIR_PIN[phys], logical);
}

bool upPressed()     { return dirPressed(DIR_UP); }
bool rightPressed()  { return dirPressed(DIR_RIGHT); }
bool downPressed()   { return dirPressed(DIR_DOWN); }
bool leftPressed()   { return dirPressed(DIR_LEFT); }
// Centre sits on the axis of rotation, so it is the same button either way.
bool centerPressed() { return pressed(BTN_CENTER, DIR_CENTER); }
