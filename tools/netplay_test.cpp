// Two players, driven the way a backend drives them: Input, nowMs, and
// packets carried between the two cores by hand. Nothing here opens a socket
// and nothing reaches into private state -- what is checked is the packets
// that come out, the tracking grid that goes in, and whose turn it is.
//   make test
#include <cstdio>

#include "game.h"

using namespace game;

static int failures = 0;
static uint32_t now = 0;
static Game A, B;

static void check(bool ok, const char *what) {
  if (!ok) { printf("  FAIL  %s\n", what); failures++; }
}

static Input none() { return Input{}; }

static Input dir(int dcol, int drow) {
  Input in;
  in.right = dcol > 0; in.left = dcol < 0;
  in.down = drow > 0;  in.up = drow < 0;
  return in;
}

// One frame at the device's ~50Hz, for both boards on one clock.
static void frame(Input a, Input b, uint32_t dt = 20) {
  now += dt;
  A.tick(a, now);
  B.tick(b, now);
}
static void idle(int frames = 1) { for (int i = 0; i < frames; i++) frame(none(), none()); }

// The transport, in three lines: whatever one board put in its outbox is
// handed to the other. A relay, a radio and this loop are interchangeable as
// far as the core is concerned, which is the point of the seam.
static void pump() {
  NetPacket p;
  while (A.takeOutbound(p)) B.receive(p);
  while (B.takeOutbound(p)) A.receive(p);
  idle();
}

// Move one board's aiming cursor to a cell and fire. The cursor clamps rather
// than wraps, so driving at the low corner first makes the position known.
static void aimAndFire(Game &g, int col, int row) {
  auto step = [&](int dc, int dr, int times) {
    for (int i = 0; i < times; i++) {
      const Input d = dir(dc, dr);
      // Only the board being aimed gets the input; the other sits still.
      now += STEP_MS + 20;
      A.tick(&g == &A ? d : none(), now);
      B.tick(&g == &B ? d : none(), now);
      idle();
    }
  };
  step(-1, 0, COLS + 1);
  step(0, -1, ROWS + 1);
  step(1, 0, col + 1);
  step(0, 1, row + 1);

  // Press, release, then let the double-press window close: a single press is
  // only known to be one once no second press has followed it.
  Input c; c.center = true;
  now += 20;
  A.tick(&g == &A ? c : none(), now);
  B.tick(&g == &B ? c : none(), now);
  now += 20;
  A.tick(none(), now);
  B.tick(none(), now);
  now += DOUBLE_MS + 20;
  A.tick(none(), now);
  B.tick(none(), now);
  idle();
}

// Let a board's result overlay run out so it is back on the match page and
// able to fire again.
static void waitForMatch(Game &g) {
  const uint32_t t = now;
  while (g.page() != Page::Match && g.page() != Page::Over && now - t < OVERLAY_MS * 3)
    idle();
}

// Both boards get the rulebook's Figure 4 layout, which is what setPage()
// fills in when a jump lands past placement, so both fleets are known:
//   carrier    row 0, cols 4..8      battleship col 2, rows 2..5
//   cruiser    row 5, cols 6..8      submarine  col 0, rows 6..8
//   destroyer  col 7, rows 8..9
static void openMatch() {
  A.begin(); B.begin();
  A.setSelfPeer(PEER_A); B.setSelfPeer(PEER_B);
  A.setLink(Link::Online); B.setLink(Link::Online);
  A.setPage(Page::Match); B.setPage(Page::Match);
  idle();
}

int main() {
  openMatch();
  check(A.myTurn(), "player A shoots first");
  check(!B.myTurn(), "and player B waits");

  // ---- firing sends a shot, and nothing else ----
  aimAndFire(A, 4, 0);
  NetPacket p;
  check(A.outboundCount() == 1, "firing produces exactly one packet");
  check(A.takeOutbound(p), "and it can be drained");
  check(p.v == 1 && p.src == PEER_A && p.dst == PEER_B, "addressed A -> B at version 1");
  check(p.type() == PacketType::Shot && p.a == 4 && p.b == 0, "a shot at column 4, row 0");
  check(A.outboundCount() == 0, "the outbox is empty once drained");

  // A second press while the shot is unanswered must not become a second
  // shot: one shot per turn, whatever the link is doing.
  aimAndFire(A, 5, 0);
  check(A.outboundCount() == 0, "no second shot while the first is unanswered");

  // ---- the defender answers with exactly one result ----
  B.receive(p);
  idle();
  check(B.outboundCount() == 1, "an inbound shot produces exactly one result");
  NetPacket r;
  B.takeOutbound(r);
  check(r.type() == PacketType::Result && r.src == PEER_B && r.dst == PEER_A,
        "the result goes back the other way");
  check(r.a == (uint8_t)ShotResult::Hit, "cell 4,0 is the carrier's bow: a hit");
  check(r.b == (uint8_t)ShipType::Carrier, "and the result names the carrier");
  check(B.myTurn(), "answering a shot passes the turn to the defender");

  // ---- the result lands on the attacker's tracking grid ----
  A.receive(r);
  idle();
  check(A.tracking(Coord{4, 0}) == CellState::Hit, "a hit lands on the tracking grid as Hit");
  check(!A.myTurn(), "a HIT does not grant another shot: the turn is over");
  waitForMatch(A);

  // ---- out of turn is refused ----
  aimAndFire(A, 6, 0);
  check(A.outboundCount() == 0, "firing out of turn sends nothing");
  check(A.tracking(Coord{6, 0}) == CellState::Empty, "and marks nothing");

  // ---- a miss records as a miss, and names no ship ----
  aimAndFire(B, 1, 1);   // row 1 is open water in the Figure 4 layout
  check(B.outboundCount() == 1, "B can fire now that it is B's turn");
  B.takeOutbound(p);
  A.receive(p); idle();
  A.takeOutbound(r);
  check(r.a == (uint8_t)ShotResult::Miss && r.b == 0, "a miss is 0 and names no ship");
  B.receive(r); idle();
  check(B.tracking(Coord{1, 1}) == CellState::Miss, "a miss lands on the tracking grid as Miss");
  check(A.myTurn() && !B.myTurn(), "a MISS ends the turn too: strictly alternating");
  waitForMatch(B);

  // ---- shooting the same cell twice is refused ----
  const int before = (int)A.outboundCount();
  aimAndFire(A, 4, 0);
  check(A.outboundCount() == before, "a cell already resolved cannot be shot again");

  // ---- play it out: B sinks A's whole fleet ----
  // A fires harmlessly along row 3 to hand the turn back each time; B works
  // through all 17 cells of A's fleet. Fleet::allSunk() is the only trigger.
  const Coord fleetCells[FLEET_CELLS] = {
      {4,0},{5,0},{6,0},{7,0},{8,0},          // carrier
      {2,2},{2,3},{2,4},{2,5},                // battleship
      {6,5},{7,5},{8,5},                      // cruiser
      {0,6},{0,7},{0,8},                      // submarine
      {7,8},{7,9},                            // destroyer
  };
  int filler = 0;
  for (int i = 0; i < FLEET_CELLS && A.page() != Page::Over; i++) {
    if (!A.myTurn()) { check(false, "A should hold the turn here"); break; }
    // A's filler shot: row 3 then row 4, skipping column 2 where B's
    // battleship stands, so it is always open water and never a repeat.
    do { filler++; } while (filler % COLS == 2);
    aimAndFire(A, filler % COLS, 3 + filler / COLS);
    pump(); pump();
    waitForMatch(A); waitForMatch(B);

    aimAndFire(B, fleetCells[i].col, fleetCells[i].row);
    pump(); pump();
    waitForMatch(A); waitForMatch(B);
  }

  check(A.fleet().allSunk(), "every cell of A's fleet has been hit");
  check(A.page() == Page::Over, "allSunk() ends the match on the loser's board");
  check(!A.won(), "and A did not win it");
  check(B.page() == Page::Over, "the winner's board is over too");
  check(B.won(), "and B won");

  if (failures == 0) { printf("netplay: all checks passed\n"); return 0; }
  printf("netplay: %d CHECK(S) FAILED\n", failures);
  return 1;
}
