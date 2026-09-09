// Whether a board plays alone is decided once, when the last ship goes down.
// Deciding it from the link's value at that instant makes it a coin toss, and
// getting it wrong strands the other player waiting for a fleet that is never
// coming. It is decided from whether a link was ever seen.
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

// Place the whole fleet, with the link in whatever state the caller leaves it.
static void placeAll(Game &g) {
  for (int i = 0; i < SHIP_COUNT; i++) { tap(g); step(g, i); doubleTap(g); }
}

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

static void toPlacement(Game &g, Link start) {
  g.begin();
  g.setSelfPeer(PEER_A);
  g.setLink(start);
  tap(g);
  // A linked board waits for the other player; an unlinked one has nothing to
  // wait for and goes straight on.
  if (start == Link::Online) shakeHands(g, PEER_A, PEER_B);
  run(g, Input{});
  run(g, Input{});
}

int main() {
  // ---- a genuinely unlinked board plays alone, as it must ----
  Game solo;
  toPlacement(solo, Link::Offline);
  placeAll(solo);
  check(!solo.everLinked(), "a board that never linked knows it");
  check(solo.practice() && solo.page() == Page::Starting,
        "and goes straight on to play alone");

  // ---- the failure this guards against ----
  // Linked all through placement, then the link blips at the very moment the
  // last ship goes down: between polls, or a seat still being claimed.
  Game blip;
  toPlacement(blip, Link::Online);
  for (int i = 0; i < SHIP_COUNT - 1; i++) { tap(blip); step(blip, i); doubleTap(blip); }
  blip.setLink(Link::Connecting);   // the blip
  tap(blip); step(blip, SHIP_COUNT - 1); doubleTap(blip);

  check(blip.everLinked(), "a board that linked earlier remembers it");
  check(!blip.practice(), "so a blip at the last ship does not latch solo play");
  check(blip.page() == Page::Waiting,
        "it waits for the other player instead of stranding them");

  // And it really did hand its fleet over.
  int fleetPackets = 0, ready = 0;
  NetPacket p{};
  while (blip.takeOutbound(p)) {
    if (p.type() == PacketType::Fleet) fleetPackets++;
    if (p.type() == PacketType::Ready) ready++;
  }
  check(fleetPackets == SHIP_COUNT, "the whole fleet went out");
  check(ready == 1, "followed by exactly one ready");

  // ---- a board seated only part way through placement ----
  // The link comes up after the first ship is down; it must still play the
  // real game rather than the solo one.
  Game late;
  toPlacement(late, Link::Offline);
  tap(late); doubleTap(late);       // first ship down, still unlinked
  late.setLink(Link::Online);       // the seat is claimed at last
  for (int i = 1; i < SHIP_COUNT; i++) { tap(late); step(late, i); doubleTap(late); }
  check(!late.practice(), "a link that arrives mid-placement still counts");
  check(late.page() == Page::Waiting, "and the board waits for its opponent");

  if (failures == 0) { printf("solo: all checks passed\n"); return 0; }
  printf("solo: %d CHECK(S) FAILED\n", failures);
  return 1;
}
