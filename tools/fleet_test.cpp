// Host-side tests for the placement and damage rules. No hardware, no
// graphics: this is the one part of the game that can be wrong in ways
// looking at the screen would not reveal.
//   make test
#include <cstdio>

#include "fleet.h"

using namespace game;

static int failures = 0;

static void check(bool ok, const char *what) {
  if (!ok) { printf("  FAIL  %s\n", what); failures++; }
}

// Indices into the fleet, in rulebook order.
enum { CARRIER = 0, BATTLESHIP, CRUISER, SUBMARINE, DESTROYER };

static void testLengths() {
  Fleet f; f.reset();
  int total = 0;
  for (int i = 0; i < SHIP_COUNT; i++) total += f.ship(i).length();
  check(total == FLEET_CELLS, "the fleet covers 17 cells");
  check(f.ship(CARRIER).length() == 5, "carrier is 5");
  check(f.ship(DESTROYER).length() == 2, "destroyer is 2");
  check(f.placedCount() == 0, "a fresh fleet has nothing placed");
  check(!f.allPlaced(), "a fresh fleet is not fully placed");
}

static void testBounds() {
  Fleet f; f.reset();
  check(f.legalAt(CARRIER, Coord{0, 5}, true), "carrier fits at row 5 vertical");
  check(!f.legalAt(CARRIER, Coord{0, 6}, true), "carrier off the bottom is rejected");
  check(f.legalAt(CARRIER, Coord{5, 0}, false), "carrier fits at col 5 horizontal");
  check(!f.legalAt(CARRIER, Coord{6, 0}, false), "carrier off the right is rejected");
  check(!f.legalAt(CARRIER, Coord{-1, 0}, true), "negative column is rejected");
  check(!f.legalAt(CARRIER, Coord{0, -1}, true), "negative row is rejected");
  // HEADER is a cursor position, not a cell: it must never be placeable.
  check(!f.legalAt(CARRIER, Coord{HEADER, HEADER}, true), "the label corner is rejected");
}

static void testOverlap() {
  Fleet f; f.reset();
  check(f.place(CARRIER, Coord{0, 0}, true), "carrier places at A0 vertical");
  check(!f.legalAt(BATTLESHIP, Coord{0, 3}, true), "a ship crossing the carrier is rejected");
  check(!f.legalAt(BATTLESHIP, Coord{0, 0}, false), "a ship sharing the origin cell is rejected");
  check(f.legalAt(BATTLESHIP, Coord{1, 0}, true), "the adjacent column is legal");
  check(!f.place(BATTLESHIP, Coord{0, 1}, true), "place() refuses an illegal position");
  check(f.placedCount() == 1, "a refused placement changes nothing");
}

// The bug the move-a-placed-ship feature introduces: a ship must not be found
// to collide with the copy of itself it is being moved from.
static void testMoveDoesNotCollideWithItself() {
  Fleet f; f.reset();
  f.place(CRUISER, Coord{4, 4}, true);
  check(f.legalAt(CRUISER, Coord{4, 5}, true), "a ship may move one cell along itself");
  check(f.legalAt(CRUISER, Coord{4, 4}, false), "a ship may rotate in place");
  f.pickUp(CRUISER);
  check(f.placedCount() == 0, "picking up decrements the placed count");
  check(f.shipAt(Coord{4, 4}) < 0, "a picked-up ship no longer occupies its cells");
  check(f.place(CRUISER, Coord{7, 7}, false), "it re-places elsewhere");
  check(f.placedCount() == 1, "re-placing restores the count");
}

static void testShotsAndSinking() {
  Fleet f; f.reset();
  f.place(DESTROYER, Coord{2, 2}, true);   // C2..C3
  f.place(CRUISER, Coord{5, 5}, false);    // F5..H5

  check(f.registerHit(Coord{0, 0}) == ShotResult::Miss, "empty water is a miss");
  check(f.registerHit(Coord{2, 2}) == ShotResult::Hit, "first destroyer cell is a hit");
  check(f.remaining() == SHIP_COUNT, "a hit does not sink");
  check(f.registerHit(Coord{2, 3}) == ShotResult::Sunk, "the last cell sinks it");
  check(f.ship(DESTROYER).sunk(), "the destroyer reports sunk");
  check(f.remaining() == SHIP_COUNT - 1, "remaining drops by one");

  // A longer ship must need every one of its cells.
  check(f.registerHit(Coord{5, 5}) == ShotResult::Hit, "cruiser cell 1");
  check(f.registerHit(Coord{7, 5}) == ShotResult::Hit, "cruiser cell 3 out of order");
  check(!f.ship(CRUISER).sunk(), "two of three is not sunk");
  check(f.registerHit(Coord{6, 5}) == ShotResult::Sunk, "the middle cell sinks it");
  check(!f.allSunk(), "unplaced ships do not count as sunk");
}

static void testAllSunk() {
  Fleet f; f.reset();
  f.place(CARRIER, Coord{0, 0}, false);
  f.place(BATTLESHIP, Coord{0, 1}, false);
  f.place(CRUISER, Coord{0, 2}, false);
  f.place(SUBMARINE, Coord{0, 3}, false);
  f.place(DESTROYER, Coord{0, 4}, false);
  check(f.allPlaced(), "all five placed");

  int hits = 0;
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++)
      if (f.registerHit(Coord{c, r}) != ShotResult::Miss) hits++;
  check(hits == FLEET_CELLS, "exactly 17 cells were hits");
  check(f.allSunk(), "the whole fleet is sunk");
  check(f.remaining() == 0, "nothing remains");
}

int main() {
  testLengths();
  testBounds();
  testOverlap();
  testMoveDoesNotCollideWithItself();
  testShotsAndSinking();
  testAllSunk();
  if (failures == 0) { printf("fleet: all checks passed\n"); return 0; }
  printf("fleet: %d CHECK(S) FAILED\n", failures);
  return 1;
}
