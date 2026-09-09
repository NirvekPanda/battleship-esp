// What the rulebook says happens when a ship goes down, checked on both
// sides: the owner of a sunk ship is the one who announces it, so the player
// being sunk has to be told, not only the player doing the sinking.
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
static void step(Game &g, int n) {
  for (int i = 0; i < n; i++) {
    Input in; in.right = true;
    t += STEP_MS + 20; g.tick(in, t);
    run(g, Input{});
  }
}
static int pump(Game &from, Game &to) {
  NetPacket p{}; int n = 0;
  while (from.takeOutbound(p)) { to.receive(p); n++; }
  return n;
}
static void settle(Game &g) { for (int i = 0; i < 3; i++) run(g, Input{}); }

// Both boards into the match, fleets exchanged.
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

static void bringUp(Game &a, Game &b) {
  for (int pass = 0; pass < 2; pass++) {
    Game &g = pass == 0 ? a : b;
    g.begin();
    g.setSelfPeer(pass == 0 ? PEER_A : PEER_B);
    g.setLink(Link::Online);
    tap(g);
    shakeHands(g, pass == 0 ? PEER_A : PEER_B, pass == 0 ? PEER_B : PEER_A);
    // Let the connecting page hand over and any half-finished gesture expire
    // before the placement gestures start, or the first press of a double
    // lands on one page and the second on the next.
    t += DOUBLE_MS + 40; g.tick(Input{}, t);
    run(g, Input{});
    for (int i = 0; i < SHIP_COUNT; i++) { tap(g); step(g, i); doubleTap(g); }
  }
  pump(a, b); pump(b, a);
  settle(a); settle(b);
  for (int i = 0; i < 200 && (a.page() == Page::Starting || b.page() == Page::Starting); i++) {
    run(a, Input{}); run(b, Input{});
  }
}

// Walk a cursor to a cell and fire. It clamps rather than wraps, so driving
// hard at the corner first makes the position known without tracking it.
static void aimAndFire(Game &g, int col, int row) {
  for (int i = 0; i < COLS + 1; i++) { Input in; in.left = true; t += STEP_MS + 20; g.tick(in, t); run(g, Input{}); }
  for (int i = 0; i < ROWS + 1; i++) { Input in; in.up = true; t += STEP_MS + 20; g.tick(in, t); run(g, Input{}); }
  for (int i = 0; i < col + 1; i++) { Input in; in.right = true; t += STEP_MS + 20; g.tick(in, t); run(g, Input{}); }
  for (int i = 0; i < row + 1; i++) { Input in; in.down = true; t += STEP_MS + 20; g.tick(in, t); run(g, Input{}); }
  tap(g);
}

static void clearOverlay(Game &g) {
  for (int i = 0; i < 250 && g.page() != Page::Match; i++) run(g, Input{});
}

int main() {
  Game a, b;
  bringUp(a, b);
  check(a.page() == Page::Match && b.page() == Page::Match, "both boards reach the match");
  check(a.enemyFleetKnown() && b.enemyFleetKnown(), "each knows the other fleet");

  // Each ship i was placed down column i, so the destroyer is the two cells
  // at the top of column 4. A fires at both, for real, through its controls.
  // One shot per turn, and the turn passes whatever the outcome -- so B has
  // to take a turn of its own between A's two shots. That is the rulebook,
  // not a quirk: "after a hit or a miss, your turn is over".
  auto exchange = [&](Game &shooter, Game &target, int col, int row) {
    aimAndFire(shooter, col, row);
    pump(shooter, target);   // the shot reaches the defender
    settle(target);
    pump(target, shooter);   // the answer comes back
    settle(shooter);
  };

  exchange(a, b, 4, 0);
  check(a.page() == Page::Hit, "the attacker is shown the hit");
  check(b.page() == Page::Hit, "and the defender is shown it too");
  check(!a.myTurn(), "a hit does not earn another shot");
  check(b.myTurn(), "the turn has passed to the defender");
  clearOverlay(a);
  clearOverlay(b);

  exchange(b, a, 9, 9);      // open water, to hand the turn back
  check(a.myTurn(), "and comes back after their shot");
  clearOverlay(a);
  clearOverlay(b);

  exchange(a, b, 4, 1);      // the destroyer's second cell

  check(a.page() == Page::Sunk, "the attacker sees the sinking");
  check(a.hitShip() == ShipType::Destroyer, "and it names the destroyer");
  check(b.page() == Page::Sunk, "the defender sees it too -- the owner announces the sink");
  check(b.hitShip() == ShipType::Destroyer, "and names the same ship");

  // The announcement is on the TOP panel now, name over word.
  int litTop = 0;
  for (int i = 0; i < gfx::Screen::size(); i++)
    for (int k = 0; k < 8; k++)
      if (b.top().buffer()[i] & (1 << k)) litTop++;
  check(litTop > 400, "the sunk announcement fills the top panel");

  // ---- the deadlock this review turned up ----
  // The two players leave the waiting page a poll apart, so one can be firing
  // while the other is still watching the wave. A shot dropped there is never
  // answered, and the shooter waits for ever.
  Game d;
  d.begin();
  d.setSelfPeer(PEER_B);
  d.setLink(Link::Online);
  tap(d);
  shakeHands(d, PEER_B, PEER_A);
  t += DOUBLE_MS + 40; d.tick(Input{}, t);
  run(d, Input{});
  for (int i = 0; i < SHIP_COUNT; i++) { tap(d); step(d, i); doubleTap(d); }
  check(d.page() == Page::Waiting, "the defender has placed and is waiting");

  for (int i = 0; i < SHIP_COUNT; i++) {
    NetPacket f{};
    f.v = PROTOCOL_VERSION; f.src = PEER_A; f.dst = PEER_B; f.seq = (uint16_t)i;
    f.t = static_cast<uint8_t>(PacketType::Fleet);
    f.a = (uint8_t)i; f.b = packPlacement(i, 0, true);
    d.receive(f);
  }
  NetPacket ready{};
  ready.v = PROTOCOL_VERSION; ready.src = PEER_A; ready.dst = PEER_B;
  ready.t = static_cast<uint8_t>(PacketType::Ready);
  d.receive(ready);
  run(d, Input{});
  check(d.page() == Page::Starting, "and is released onto the wave");

  NetPacket drain{};
  while (d.takeOutbound(drain)) {
  }  // its own fleet handover, already sent

  NetPacket early{};
  early.v = PROTOCOL_VERSION; early.seq = 7; early.src = PEER_A; early.dst = PEER_B;
  early.t = static_cast<uint8_t>(PacketType::Shot);
  early.a = 0; early.b = 0;
  d.receive(early);

  NetPacket answer{};
  check(d.takeOutbound(answer), "a shot arriving during the wave is still answered");
  check(answer.t == static_cast<uint8_t>(PacketType::Result),
        "and the answer is a result, so the shooter is never left hanging");

  if (failures == 0) { printf("sinking: all checks passed\n"); return 0; }
  printf("sinking: %d CHECK(S) FAILED\n", failures);
  return 1;
}
