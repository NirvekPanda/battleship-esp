#pragma once
#include <stdint.h>

#include "game_config.h"

namespace game {

// Single vs double press on the centre button.
//
// Rotating and placing share one button, so a single press cannot be
// classified until the double-press window has expired: it is only a single
// press once the window for a second one has passed.
//
// Triple press and press-and-hold are deliberately undefined: a held button
// produces one Single once the window has passed and nothing further.
class PressDetector {
public:
  enum class Event : uint8_t { None, Single, Double };

  // Level in, edge out. Call once per tick with the raw button state.
  //
  // A single press cannot be recognised at the moment it happens: it is only
  // a single press once the window for a second one has passed. So Single is
  // reported LATE, when the window closes, and Double as soon as the second
  // press lands.
  //
  // The latency therefore lands on the single press, which is why turning is
  // the single one: a turn that arrives late is taken back by turning again,
  // where a ship put down when it was meant to be turned has to be picked up,
  // moved back and turned. The action that cannot be undone gets the gesture
  // that is recognised the instant it happens.
  Event update(bool center, uint32_t nowMs) {
    const bool edge = center && !_prev;
    _edge = edge;
    _prev = center;

    if (edge) {
      if (_armed && nowMs - _last <= DOUBLE_MS) {
        _armed = false;
        return Event::Double;
      }
      _armed = true;
      _last = nowMs;
      return Event::None;  // not yet: it may still become a double
    }

    // The window closed with no second press, so it was a single after all.
    if (_armed && nowMs - _last > DOUBLE_MS) {
      _armed = false;
      return Event::Single;
    }
    return Event::None;
  }

  // The raw press, as it happens, for the pages where a press is a press and
  // not a gesture: aiming, confirming, dismissing a result. Those have no
  // second meaning for the button, so making them wait out the double window
  // would put DOUBLE_MS between the click and the shot for no reason. Valid
  // for the tick update() was last called on.
  bool edge() const { return _edge; }

  // Forget any half-finished gesture. Called when focus moves, so the press
  // that selected a ship cannot become the first half of a double that then
  // turns it.
  //
  // Only the gesture is cleared, never _prev: that is the button's actual
  // level, which update() set this same tick, and it is what stops a still-
  // held press from firing again as a fresh edge.
  void reset() { _armed = false; }

private:
  bool _prev = false;
  bool _edge = false;
  bool _armed = false;
  uint32_t _last = 0;
};

}  // namespace game
