#pragma once
#include <stdint.h>

namespace game {

// Level state of the five controls, sampled once per tick. Backends fill
// this in from whatever they have -- GPIO on device, keydown in the browser.
struct Input {
  bool up = false, down = false, left = false, right = false, center = false;
};

}  // namespace game
