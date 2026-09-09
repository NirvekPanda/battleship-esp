#pragma once
#include <stdint.h>

// Tunables that govern how the game feels, gathered in one place rather than
// buried as literals at their point of use. Every one of them is a duration in
// milliseconds measured against the nowMs argument to Game::tick(), never a
// frame count -- the browser ticks at ~60Hz and the device at ~50Hz, so a
// count would mean two different games.
namespace game {

// Cursor auto-repeat while a direction is held.
constexpr uint32_t STEP_MS = 120;

// How long after a centre press a second one still counts as a double.
//
// Single TURNS a ship, double puts it down. Because a single press is only
// known to be single once this window has passed, it is the delay between
// pressing and the ship turning -- and a turn is taken back for free by
// turning again, so it can afford to wait where placing could not.
//
// 500ms is a comfortable double press on a real 5-way switch, and comfortably
// clear of the hold that leaves a lobby: placing is the double press now, and
// a place that does not register is a worse failure than a turn that arrives
// half a second late.
constexpr uint32_t DOUBLE_MS = 500;

// How long centre has to be held before it means "go back" rather than
// "choose this" -- the point at which the countdown appears. Comfortably past
// the double press, so putting a ship down never starts one.
constexpr uint32_t HOLD_MS = 800;

// And how long it has to be held in total to actually leave. Five seconds,
// counted down on the panel a second at a time, because leaving a match is
// not something to do by resting a thumb on a button: the count is both the
// warning and the way out of it -- let go and nothing happened.
constexpr uint32_t LEAVE_HOLD_MS = 5000;

// How long the verdict has the bottom panel to itself before it gives way to
// the two boards. Three seconds is long enough to read WIN or LOSE and the
// name under it, and short enough that nobody is waiting on it.
//
// It also blocks: a press during those three seconds does nothing, so the
// press that fired the winning shot cannot skip past the answer to it.
constexpr uint32_t VERDICT_MS = 3000;

// How long a shot result stays up before the board comes back.
constexpr uint32_t OVERLAY_MS = 3000;

// How many characters of a name are shown on a panel. A player picks their
// own name and a 128px line has room for so much; truncating is what keeps a
// long one from pushing everything else off the screen. Team names are always
// within this anyway, which is why the panels show the team rather than the
// player.
constexpr int NAME_CHARS = 5;

// The lobby list has a whole line per player and no team letter beside it, so
// it can show far more of a name than the match pages can. 16 characters at
// 6px sits inside 128 with room left for the tick that marks a player who has
// already chosen you.
constexpr int LOBBY_NAME_CHARS = 16;

// How often to repeat the hello while waiting for the other player. The room
// may be empty when the first one is sent, and a packet sent to nobody is
// simply lost, so it is repeated until it is answered.
constexpr uint32_t HELLO_EVERY_MS = 1000;

// Length of the wave that plays while the match is starting.
constexpr uint32_t WAVE_MS = 2000;

}  // namespace game
