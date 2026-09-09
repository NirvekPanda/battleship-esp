// The dashboard marching both boards to a screen, and the reason that is safe:
// a page packet is obeyed from the server and from nobody else.
//   make test
#include <cstdio>

#include "game.h"

using namespace game;

static int failures = 0;
static void check(bool ok, const char *what) {
  if (!ok) { printf("  FAIL  %s\n", what); failures++; }
}

static NetPacket pageFrom(uint8_t src, Page page) {
  NetPacket p{};
  p.v = PROTOCOL_VERSION;
  p.src = src;
  p.dst = PEER_BROADCAST;
  p.t = static_cast<uint8_t>(PacketType::Page);
  p.a = static_cast<uint8_t>(page);
  return p;
}

int main() {
  Game g;
  g.begin();
  g.setSelfPeer(PEER_A);
  check(g.page() == Page::Start, "starts on the title");

  // The server can move it.
  g.receive(pageFrom(PEER_SERVER, Page::Place));
  check(g.page() == Page::Place, "the server can send it to the placement page");

  g.receive(pageFrom(PEER_SERVER, Page::Connecting));
  check(g.page() == Page::Connecting, "and back to connecting");

  // The other player cannot. Moving an opponent's screen mid-match is a way
  // to cheat, not a feature, so it is refused by where it came from rather
  // than by what it says.
  const Page before = g.page();
  g.receive(pageFrom(PEER_B, Page::Over));
  check(g.page() == before, "the other player cannot move our screen");

  // Nor can a page that does not exist.
  NetPacket bad = pageFrom(PEER_SERVER, Page::Start);
  bad.a = 200;
  g.receive(bad);
  check(g.page() == before, "a page number that does not exist is ignored");

  if (failures == 0) { printf("forcepage: all checks passed\n"); return 0; }
  printf("forcepage: %d CHECK(S) FAILED\n", failures);
  return 1;
}
