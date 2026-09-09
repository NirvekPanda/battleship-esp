// Walking back into a match: the board is handed back, not replayed.
//
// A Result means "the shot you just fired was a hit", which means nothing to
// a board that has restarted and fired nothing. So the relay sends marks that
// name their own cell instead -- and the one rule they must obey is that
// applying them answers nothing and fires nothing, or a reconnect would put
// shots and results back into a match that has moved on.
//   make test
#include <cstdio>

#include "game.h"

using namespace game;

static int failures = 0;
static void check(bool ok, const char *what) {
  if (!ok) { printf("  FAIL  %s\n", what); failures++; }
}

static Game g;
static void feed(uint8_t t, uint8_t a, uint8_t b = 0, uint8_t src = PEER_SERVER) {
  NetPacket p{};
  p.v = PROTOCOL_VERSION; p.src = src; p.dst = PEER_A; p.t = t; p.a = a; p.b = b;
  g.receive(p);
}

// The packet types the relay uses to hand a board back, as game::PacketType.
static const uint8_t MYFLEET = 10, FLEET = 7, READY = 2, PAGE = 9;
static const uint8_t MARK = 12, DAMAGE = 13, TURN = 14;

int main() {
  g.begin();
  g.setSelfPeer(PEER_A);
  g.setLink(Link::Online);

  // Exactly what GET /v1/reconnect answers with, for a player who had laid
  // out a fleet, hit their opponent's cruiser at E2 and taken one hit.
  for (int i = 0; i < SHIP_COUNT; i++) feed(MYFLEET, i, i * 20);
  for (int i = 0; i < SHIP_COUNT; i++) feed(FLEET, i, i * 20, PEER_B);
  feed(READY, 0, 0, PEER_B);
  // Their ships lie along the rows here, so the cruiser is row 2, columns
  // 0..2: a hit on it is cell 21.
  feed(MARK, 21, 1 | 2 << 2);   // C1: a hit on the cruiser
  feed(DAMAGE, 3, 1);           // A3: one of ours struck
  feed(DAMAGE, 7, 0);           // A7: they shot there and missed
  feed(TURN, 1);
  feed(PAGE, 4);                // the match page
  g.tick(Input{}, 100);

  check(g.page() == Page::Match, "a returning player lands back on the board");
  check(g.fleet().placedCount() == SHIP_COUNT, "with their own fleet where they left it");
  check(g.tracking(Coord{1, 2}) == CellState::Hit, "their hit is still on the tracking grid");
  check(g.tracking(Coord{0, 0}) == CellState::Empty, "and a cell nobody shot at is still open water");
  // Their hull remembers it too, so the board revealed at the end of the
  // match is the same board whether or not anybody restarted mid-game.
  check(g.enemyFleet().ship(2).hits != 0, "the hit is on their ship as well as on our grid");
  check(g.fleet().ship(0).hits != 0, "the damage they had taken is still on their own board");
  check(g.incoming(Coord{7, 0}) == CellState::Miss,
        "and the water they wasted a shot on is still marked");
  check(g.myTurn(), "and it is their move, as the relay said");

  // The rule the whole design turns on: restoring is not playing. A mark must
  // not fire and damage must not be answered, or a reconnect would inject a
  // second copy of every shot into a match that has moved on.
  check(g.outboundCount() == 0, "putting a board back says nothing to the other player");

  // A mark is from the SERVER. A player who could mark another player's grid
  // could tell them their shot hit when it did not.
  const int before = static_cast<int>(g.tracking(Coord{9, 9}));
  feed(MARK, 99, 1, PEER_B);
  check(static_cast<int>(g.tracking(Coord{9, 9})) == before,
        "a mark from a player is ignored");

  // ---- their shots, live ----
  // The same marks arrive during a game from the shots themselves: a hit
  // shows as damage to the hull, and a miss as the dot that says they have
  // already spent a turn on that square.
  Game live;
  live.begin();
  live.setSelfPeer(PEER_A);
  live.setLink(Link::Online);
  live.setPage(Page::Match);          // a laid-out board to shoot at
  NetPacket shot{};
  shot.v = PROTOCOL_VERSION; shot.src = PEER_B; shot.dst = PEER_A;
  shot.t = static_cast<uint8_t>(PacketType::Shot);
  shot.a = 9; shot.b = 9;             // open water in the demo layout
  live.receive(shot);
  check(live.incoming(Coord{9, 9}) == CellState::Miss,
        "a shot of theirs that found water is marked on our own board");
  check(live.incoming(Coord{0, 9}) == CellState::Empty,
        "and a square they have not shot at is not");

  if (failures == 0) { printf("reconnect: all checks passed\n"); return 0; }
  printf("reconnect: %d CHECK(S) FAILED\n", failures);
  return 1;
}
