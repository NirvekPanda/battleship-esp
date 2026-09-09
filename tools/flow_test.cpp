// Drives the game through a whole session with nothing but Input and nowMs,
// the way a backend does. This is the test that the pages actually connect:
// select a ship, turn it, place it, start the match, hit and sink.
//
// There is no link here -- the session presses through the connecting page
// with no server, which is the unlinked practice path, where the board
// resolves its own shots against its own fleet and the turn never passes.
// The two-player game, its packets and its turn order are tools/netplay_test.
//
// Placement is checked by shooting at it afterwards rather than by reading
// private state: where a ship really ended up is exactly what a shot reports.
//   make test
#include <cstdio>

#include "game.h"

using namespace game;

static int failures = 0;
static Game g;
static uint32_t now = 0;

static void check(bool ok, const char *what) {
  if (!ok) { printf("  FAIL  %s\n", what); failures++; }
}

// One frame at the device's ~50Hz.
static void frame(Input in, uint32_t dt = 20) { now += dt; g.tick(in, now); }

static Input none() { return Input{}; }

static Input dir(int dcol, int drow) {
  Input in;
  in.right = dcol > 0; in.left = dcol < 0;
  in.down = drow > 0;  in.up = drow < 0;
  return in;
}

// A single press: down, up, then wait out the double-press window. A press is
// only known to be a single one once the window for a second has passed, so a
// test that does not wait sees nothing happen.
//
// With a ship in hand this TURNS it; everywhere else -- the title, the picker,
// a result -- the press acts on the edge and the wait is simply harmless.
static void tap() {
  Input c; c.center = true;
  frame(c);
  frame(none());
  frame(none(), DOUBLE_MS + 20);
}

// Two presses inside the window: the gesture that puts the ship in hand down.
// It is recognised the instant the second press lands, which is why it is the
// one that cannot be undone -- and why, unlike a tap, this needs no wait
// afterwards: the gesture is already spent.
static void doubleTap() {
  Input c; c.center = true;
  frame(c);
  frame(none(), 20);
  frame(c, 20);
  frame(none());
}

// Held direction, one cell per STEP_MS.
static void move(int dcol, int drow, int times) {
  for (int i = 0; i < times; i++) {
    frame(dir(dcol, drow), STEP_MS + 20);
    frame(none());
  }
}

// Fire at a cell and report what the game said, then let the result clear.
static Page shootAt(int col, int row);

int main() {
  g.begin();
  check(g.page() == Page::Start, "opens on the start page");

  // This board has no network configured, so the connecting page has nothing
  // to wait for and does not hold it: press, and the fleet goes down. A board
  // that IS on a network blocks there until both players have answered each
  // other -- tools/intro_test.cpp covers that.
  tap();
  frame(none());
  check(g.page() == Page::Place, "with no network there is nothing to wait for");

  // ---- carrier: turned across the board and driven at the right wall ----
  tap();               // take the carrier (5 cells)
  tap();               // one press: lay it along the row
  move(1, 0, 9);       // drive as far right as it will go
  doubleTap();         // two presses: put it down
  check(g.page() == Page::Place, "the carrier is down, four ships to go");

  // ---- battleship: straight down column 0 ----
  tap();               // take it
  doubleTap();         // and straight back down, upright

  // ---- cruiser: turned across, then turned back upright ----
  tap();
  tap();               // horizontal
  tap();               // and back upright, to prove the turn is a toggle
  move(1, 0, 1);       // column 1
  doubleTap();

  // ---- submarine: upright, and driven hard at the BOTTOM wall ----
  // The same clamp has to hold on the other axis, or a standing ship hangs
  // off the foot of the board exactly as a lying one hung off the side.
  tap(); move(1, 0, 2); move(0, 1, 9); doubleTap();

  // ---- destroyer, upright in its own column ----
  tap(); move(1, 0, 3); doubleTap();

  check(g.page() == Page::Starting, "the fifth placement starts the match");

  const uint32_t waveStart = now;
  while (g.page() == Page::Starting && now - waveStart < WAVE_MS * 3) frame(none());
  check(g.page() == Page::Match, "the wave hands over to the match");
  check(g.myTurn(), "an unlinked board always holds the turn: there is nobody to pass it to");
  check(g.outboundCount() == 0, "and it sends no packets, having nowhere to send them");
  check(now - waveStart >= WAVE_MS, "the wave lasted at least WAVE_MS");

  // ---- what the placement actually did ----
  // A five-cell ship laid across a ten-column board cannot start further
  // right than column 5, so driving it at the wall must leave it on columns
  // 5..9 of row 0 -- wholly on the board, with nothing hanging over the edge.
  check(shootAt(4, 0) == Page::Miss, "column 4 is open water: the carrier stopped at the wall");
  check(shootAt(5, 0) == Page::Hit, "the carrier's bow sits on column 5");
  check(g.hitShip() == ShipType::Carrier, "and it is the carrier");
  for (int c = 6; c <= 8; c++)
    check(shootAt(c, 0) == Page::Hit, "the carrier lies along row 0");
  check(shootAt(9, 0) == Page::Sunk, "its stern reaches the last column, and that sinks it");

  // The cruiser was turned across and then back: if the second gesture had
  // not taken, it would be lying along row 0 instead of standing in column 1.
  check(shootAt(1, 2) == Page::Hit, "the cruiser stands upright in column 1");
  check(g.hitShip() == ShipType::Cruiser, "and it is the cruiser");

  // A three-cell ship driven at the foot of the board stops at row 7, so
  // rows 7..9 are hull and row 6 is open water.
  check(shootAt(2, 6) == Page::Miss, "row 6 is open water: the submarine stopped at the foot");
  check(shootAt(2, 7) == Page::Hit, "the submarine's bow sits on row 7");
  check(g.hitShip() == ShipType::Submarine, "and it is the submarine");
  check(shootAt(2, 8) == Page::Hit, "it stands down column 2");
  check(shootAt(2, 9) == Page::Sunk, "its stern reaches the last row, and that sinks it");

  // Firing at a resolved cell must do nothing: the rulebook's own rule that
  // the same shot is never called twice.
  // The cursor is still on the cell shootAt() just fired at, so this is the
  // same shot a second time and must be refused.
  tap();
  check(g.page() == Page::Match, "a resolved cell cannot be shot again");
  check(g.outboundCount() == 0, "nothing went out over a link that was never up");

  if (failures == 0) { printf("flow: all checks passed\n"); return 0; }
  printf("flow: %d CHECK(S) FAILED\n", failures);
  return 1;
}

// Walk the aiming cursor to a cell from wherever it is and fire. The cursor
// clamps rather than wraps, so driving hard at the low corner first makes the
// position known without needing to track it.
static Page shootAt(int col, int row) {
  move(-1, 0, COLS + 1);   // into the row-letter strip
  move(0, -1, ROWS + 1);   // and the column-number strip
  move(1, 0, col + 1);     // HEADER -> 0 -> col
  move(0, 1, row + 1);
  tap();
  const Page result = g.page();
  const uint32_t t = now;
  while (g.page() != Page::Match && now - t < OVERLAY_MS * 3) frame(none());
  return result;
}
