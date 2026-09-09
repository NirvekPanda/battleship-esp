// The opening sequence, which is what a person sees first and so is the part
// most worth pinning down: the title holds until a button is pressed, the
// water runs, and then the fleet goes down.
//   make test
#include <cstdio>

#include "game.h"

using namespace game;

static int failures = 0;
static void check(bool ok, const char *what) {
  if (!ok) { printf("  FAIL  %s\n", what); failures++; }
}

static int lit(const gfx::Screen &s) {
  int n = 0;
  for (int i = 0; i < gfx::Screen::size(); i++)
    for (int b = 0; b < 8; b++)
      if (s.buffer()[i] & (1 << b)) n++;
  return n;
}

// A signature that changes when the picture changes, unlike a pixel count,
// which can stay equal while everything moves.
static unsigned sig(const gfx::Screen &s) {
  unsigned h = 2166136261u;
  for (int i = 0; i < gfx::Screen::size(); i++) { h ^= s.buffer()[i]; h *= 16777619u; }
  return h;
}

static Input down() { Input in; in.down = true; return in; }

static void tapCentre(Game &g, uint32_t &t) {
  Input c; c.center = true;
  t += 20; g.tick(c, t);
  t += 20; g.tick(Input{}, t);
  t += DOUBLE_MS + 20; g.tick(Input{}, t);  // close the double-press window
}

// Inject the other player's side of the handshake.
static NetPacket from(uint8_t src, uint8_t self, PacketType t) {
  NetPacket p{};
  p.v = PROTOCOL_VERSION; p.src = src; p.dst = self;
  p.t = static_cast<uint8_t>(t);
  return p;
}

int main() {
  // ---- the title holds ----
  Game g;
  g.begin();
  check(g.page() == Page::Start, "the device starts on the start page");

  uint32_t t = 0;
  for (int i = 0; i < 100; i++) { t += 20; g.tick(Input{}, t); }
  check(g.page() == Page::Start, "and stays there, untouched, for two seconds");
  check(lit(g.top()) > 0 && lit(g.bottom()) > 0, "with something drawn on both panels");

  // ---- the title asks a question, so it is not left by just any press ----
  // Left and right choose between ONLINE and OFFLINE; only centre commits.
  g.setSelfPeer(PEER_A);
  g.setLink(Link::Online);
  for (int i = 0; i < 5; i++) { t += 20; g.tick(down(), t); }
  check(g.page() == Page::Start, "a direction that is not a choice does nothing");

  tapCentre(g, t);
  check(g.mode() == Mode::Online, "ONLINE is the default");
  check(g.page() == Page::Connecting, "and centre takes it");

  // ---- and then it BLOCKS ----
  // No timeout and no skip. Being connected is a fact about two machines, and
  // one of them cannot assume it.
  for (int i = 0; i < 5; i++) { t += 20; g.tick(down(), t); }
  check(g.page() == Page::Connecting, "holding a direction does not skip it");
  for (int i = 0; i < 20; i++) { tapCentre(g, t); }
  check(g.page() == Page::Connecting, "nor does pressing centre repeatedly");
  const uint32_t waitedFrom = t;
  while (g.page() == Page::Connecting && t - waitedFrom < 30000) { t += 20; g.tick(Input{}, t); }
  check(g.page() == Page::Connecting, "and it never times out");

  // It has been saying hello all along, which is what a Join is for.
  int hellos = 0;
  NetPacket out{};
  while (g.takeOutbound(out)) if (out.type() == PacketType::Join) hellos++;
  check(hellos > 1, "it keeps saying hello while it waits");

  // ---- the water moves, so waiting is not mistaken for hung ----
  t += 20; g.tick(Input{}, t);
  const unsigned first = sig(g.bottom());
  bool moved = false;
  for (int i = 0; i < 10 && !moved; i++) { t += 40; g.tick(Input{}, t); moved = sig(g.bottom()) != first; }
  check(moved, "the wave animates rather than sitting still");

  // ---- hearing them is not enough on its own ----
  g.receive(from(PEER_B, PEER_A, PacketType::Join));
  t += 20; g.tick(Input{}, t);
  check(g.page() == Page::Connecting, "hearing them alone does not start the game");
  bool ackedBack = false;
  while (g.takeOutbound(out)) if (out.type() == PacketType::Ack) ackedBack = true;
  check(ackedBack, "but it answers them");

  // ---- their Ack is the half that cannot be inferred ----
  g.receive(from(PEER_B, PEER_A, PacketType::Ack));
  t += 20; g.tick(Input{}, t);
  check(g.handshakeDone(), "both directions are now proven");
  check(g.page() == Page::Place, "and only then does the fleet go down");

  // ---- a board with no network at all is not stranded ----
  Game alone;
  alone.begin();
  alone.setLink(Link::Offline);
  // OFFLINE is chosen before any network exists, which is the point: a board
  // with no WiFi configured can still be played.
  uint32_t u = 0;
  Input right; right.right = true;
  u += STEP_MS + 20; alone.tick(right, u);
  u += 20; alone.tick(Input{}, u);
  // The press itself, tick by tick: the title confirms on the press rather
  // than on the gesture, so the choice lands the moment the button goes down
  // and the page is inspected before the board is ticked on.
  Input centre; centre.center = true;
  u += 20; alone.tick(centre, u);
  check(alone.mode() == Mode::Offline, "right selects OFFLINE");
  // Offline waits too -- for the board on the other side of the table rather
  // than for a relay. The core cannot tell the two apart, which is the whole
  // point of the seam: the same page, the same handshake, a different radio.
  check(alone.page() == Page::Connecting, "and it waits for the other board");

  // Searching. The radio is up but nobody has answered yet, which is what the
  // firmware reports while it is broadcasting its hello. Set before the next
  // tick, because Link::Offline is reserved for having no radio at all and
  // does still fall through to solo play -- a board with nothing to talk to
  // must not be stuck on a page that waits for something that cannot arrive.
  alone.setLink(Link::Connecting);
  u += 20; alone.tick(Input{}, u);

  // With nothing answering, it stays there rather than starting a game with
  // an opponent that does not exist.
  for (int i = 0; i < 200; i++) { u += 20; alone.tick(Input{}, u); }
  check(alone.page() == Page::Connecting, "and keeps waiting while nobody answers");

  // The other board turns up: the same Join/Ack that the relay carries, only
  // over the radio this time.
  alone.setSelfPeer(PEER_A);
  alone.setLink(Link::Online);
  alone.receive(from(PEER_B, PEER_A, PacketType::Join));
  alone.receive(from(PEER_B, PEER_A, PacketType::Ack));
  u += 20; alone.tick(Input{}, u);
  check(alone.page() == Page::Place, "and starts once the other board answers");

  if (failures == 0) { printf("intro: all checks passed\n"); return 0; }
  printf("intro: %d CHECK(S) FAILED\n", failures);
  return 1;
}
