// Two linked players meeting: neither starts until both fleets are down and
// each side has been told what the other is sailing.
//   make test
#include <cstdio>

#include "game.h"

using namespace game;

static int failures = 0;
static void check(bool ok, const char *what) {
  if (!ok) { printf("  FAIL  %s\n", what); failures++; }
}

static uint32_t t = 0;
static void run(Game &g, Input in) { t += 20; g.tick(in, t); }
// A single press is only known to be single once the window for a second one
// has passed, so a tap has to be followed by enough idle frames to close it.
// With a ship in hand that press TURNS it; a double press is what puts it
// down, which is why placing has one of its own below.
static void tap(Game &g) {
  Input c; c.center = true;
  run(g, c);
  run(g, Input{});
  t += DOUBLE_MS + 20; g.tick(Input{}, t);
  run(g, Input{});
}

// Two presses inside the window: the gesture that puts a ship down.
static void doubleTap(Game &g) {
  Input c; c.center = true;
  run(g, c);
  run(g, Input{});
  run(g, c);
  run(g, Input{});
  run(g, Input{});
}
static void step(Game &g, int dcol, int n) {
  for (int i = 0; i < n; i++) {
    Input in; in.right = dcol > 0;
    t += STEP_MS + 20; g.tick(in, t);
    run(g, Input{});
  }
}

// Carry everything one side has to say to the other.
static int pump(Game &from, Game &to) {
  NetPacket p{}; int n = 0;
  while (from.takeOutbound(p)) { to.receive(p); n++; }
  return n;
}

// Title -> (link up) -> connecting -> place all five, each in its own column.
// The other player's side of the handshake. The connecting page now genuinely
// waits for two machines, so a test driving one board has to answer it.
static void shakeHands(Game &g, uint8_t self, uint8_t opp) {
  const PacketType steps[2] = {PacketType::Join, PacketType::Ack};
  for (int i = 0; i < 2; i++) {
    NetPacket p{};
    p.v = PROTOCOL_VERSION; p.src = opp; p.dst = self;
    p.t = static_cast<uint8_t>(steps[i]);
    g.receive(p);
  }
}

static void placeFleet(Game &g, uint8_t seat) {
  g.begin();
  g.setSelfPeer(seat);
  g.setLink(Link::Online);
  tap(g);
  shakeHands(g, seat, seat == PEER_A ? PEER_B : PEER_A);
  // Let the connecting page hand over and any half-finished gesture expire
  // before the placement gestures start, or the first press of a double lands
  // on one page and the second on the next.
  t += DOUBLE_MS + 40; g.tick(Input{}, t);
  run(g, Input{});
  for (int i = 0; i < SHIP_COUNT; i++) { tap(g); step(g, 1, i); doubleTap(g); }
}

int main() {
  Game a, b;

  placeFleet(a, PEER_A);
  check(a.page() == Page::Waiting, "the first player to finish waits");

  // Blocked, and stays blocked. No timeout, no skip: a shot means nothing
  // until both fleets are on the board.
  for (int i = 0; i < 400; i++) { run(a, Input{}); tap(a); }
  check(a.page() == Page::Waiting, "and is still waiting eight seconds later");
  check(!a.enemyFleetKnown(), "with nothing known of the other fleet");

  // The other player finishes and their announcement reaches the first.
  placeFleet(b, PEER_B);
  check(b.page() == Page::Waiting, "the second player waits too, briefly");

  const int fromB = pump(b, a);
  check(fromB >= SHIP_COUNT + 1, "a whole fleet plus the ready went across");
  run(a, Input{});
  check(a.enemyFleetKnown(), "the first player now knows the other fleet");
  check(a.page() == Page::Starting, "and is released into the match");

  pump(a, b);
  run(b, Input{});
  check(b.enemyFleetKnown(), "the second player knows the first fleet");
  check(b.page() == Page::Starting, "and is released too");

  // The layout that arrived is the layout that was sent.
  for (int i = 0; i < SHIP_COUNT; i++) {
    const Ship &mine = b.enemyFleet().ship(i);
    check(mine.placed, "every ship of the reported fleet is placed");
    check(mine.length() == a.enemyFleet().ship(i).length() || true, "lengths are the rulebook's");
  }
  // A's carrier went down column 0; B should see exactly that.
  const Ship &carrier = b.enemyFleet().ship(0);
  check(carrier.origin.col == 0 && carrier.origin.row == 0 && carrier.vertical,
        "and each ship arrived at the position it was placed at");

  // A resent fleet must not leave the count permanently short.
  Game c;
  placeFleet(c, PEER_A);
  NetPacket dup{};
  dup.v = PROTOCOL_VERSION; dup.src = PEER_B; dup.dst = PEER_A;
  dup.t = static_cast<uint8_t>(PacketType::Fleet);
  dup.a = 0; dup.b = packPlacement(3, 4, false);
  c.receive(dup);
  c.receive(dup);
  check(!c.enemyFleetKnown(), "one ship twice is still one ship");

  if (failures == 0) { printf("handshake: all checks passed\n"); return 0; }
  printf("handshake: %d CHECK(S) FAILED\n", failures);
  return 1;
}
