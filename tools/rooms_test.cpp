// The five numbered lobbies, and the gestures that pick one.
//
// The gestures live in the core rather than in each backend, so a board and a
// browser cannot disagree about what a press means. Two failures this guards
// against, both of which happened: a press carried in from the page before
// joining a lobby the player had not looked at yet, and a hold with no way
// back out of a lobby once in it.
//   make test
#include <cstdio>

#include "game.h"

using namespace game;

static int failures = 0;
static void check(bool ok, const char *what) {
  if (!ok) { printf("  FAIL  %s\n", what); failures++; }
}

static uint32_t t = 0;
static Game g;
static void run(Input in, uint32_t ms = 20) { t += ms; g.tick(in, t); }
static Input centre() { Input in; in.center = true; return in; }
static Input down() { Input in; in.down = true; return in; }

int main() {
  g.begin();

  // Five rows whoever is in them: an empty lobby has to be there to be joined
  // again, so the list never shrinks.
  g.clearRooms();
  g.setRoom(1, 0, "", "");
  g.setRoom(2, 1, "nirvek", "");
  g.setRoom(3, 2, "nirvek", "esp-red");
  g.setRoom(4, 0, "", "");
  g.setRoom(5, 0, "", "");

  // ---- a press carried in from the page before joins nothing ----
  // The centre button is already down when the page opens, which is exactly
  // what happens after it was pressed to choose ONLINE.
  run(centre());
  g.setPage(Page::Rooms);
  run(centre());
  run(centre());
  run(Input{});
  check(g.takeRoomJoin() == 0, "a press already down when the page opened joins nothing");

  // ---- a press that starts here joins the room under the cursor ----
  run(centre());
  run(Input{});
  check(g.takeRoomJoin() == 1, "a press joins the room the cursor is on");
  check(g.takeRoomJoin() == 0, "and the decision is taken only once");

  // ---- the cursor moves, and so does what a press joins ----
  run(down(), STEP_MS + 20);
  run(Input{});
  run(centre());
  run(Input{});
  check(g.takeRoomJoin() == 2, "the cursor chose the second lobby");

  // ---- a full lobby refuses the cursor's press ----
  for (int i = 0; i < 1; i++) { run(down(), STEP_MS + 20); run(Input{}); }
  run(centre());
  run(Input{});
  check(g.takeRoomJoin() == 0, "a lobby with two players in it cannot be joined");

  // ---- hold to go back ----
  // A hold is counted down rather than acted on at once, and letting go
  // during the count is how the player says they did not mean it.
  g.setMyRoom(2);
  run(centre());
  run(centre(), HOLD_MS + 40);
  check(!g.takeRoomLeave(), "the count is a warning, not the leaving itself");
  run(Input{});
  check(!g.takeRoomLeave(), "letting go during the count leaves nothing");
  // And it must not be read as the press that joins a lobby either: that is
  // the opposite of what the player just decided.
  check(g.takeRoomJoin() == 0, "letting go after a hold joins nothing");
  check(g.myRoom() == 2, "and they are still in the lobby they were in");

  // Held all the way: gone, and the page says so without waiting for the
  // relay to confirm it.
  run(centre());
  run(centre(), LEAVE_HOLD_MS + 40);
  check(g.takeRoomLeave(), "holding centre for the whole count leaves the lobby");
  check(g.myRoom() == 0, "and the page says so at once");
  check(g.page() == Page::Rooms, "back on the lobby list");
  run(Input{});
  check(g.takeRoomJoin() == 0, "and letting go afterwards joins nothing");

  // ---- leaving a game, not just a lobby ----
  // The hold works from every page a game is played on, and what it leaves
  // behind is nothing: the next lobby starts from an empty board.
  Game m;
  m.begin();
  m.setPage(Page::Match);          // a board, laid out by the debug path
  check(m.fleet().allPlaced(), "the match page has a fleet on it to begin with");
  uint32_t u = 0;
  Input c2; c2.center = true;
  u += 20; m.tick(c2, u);
  u += LEAVE_HOLD_MS + 40; m.tick(c2, u);
  check(m.takeRoomLeave(), "holding centre in a match asks to leave it");
  check(m.page() == Page::Rooms, "and lands back on the lobby list");
  check(m.fleet().placedCount() == 0, "with the fleet cleared, not carried into the next game");

  // ---- what walking out leaves behind ----
  // Nothing. The counters are the half that bites: a player who left having
  // sunk three ships used to carry that count into their next game and win it
  // two ships early, and a stale "they are ready" let the waiting page
  // through before the new opponent had said a word.
  Game left;
  left.begin();
  left.setSelfPeer(PEER_A);
  left.setPage(Page::Match);
  // Three of their ships down, their fleet known, their ready received.
  for (int i = 0; i < SHIP_COUNT; i++) {
    NetPacket f{};
    f.v = PROTOCOL_VERSION; f.src = PEER_B; f.dst = PEER_A;
    f.t = static_cast<uint8_t>(PacketType::Fleet);
    f.a = static_cast<uint8_t>(i); f.b = static_cast<uint8_t>(i * 20);
    left.receive(f);
  }
  NetPacket ready{};
  ready.v = PROTOCOL_VERSION; ready.src = PEER_B; ready.dst = PEER_A;
  ready.t = static_cast<uint8_t>(PacketType::Ready);
  left.receive(ready);
  check(left.opponentReady(), "their ready arrived");
  check(left.enemyFleetKnown(), "their fleet arrived");

  uint32_t v = 0;
  Input hold; hold.center = true;
  v += 20; left.tick(hold, v);
  v += LEAVE_HOLD_MS + 40; left.tick(hold, v);
  check(left.takeRoomLeave(), "holding centre leaves the match");
  check(!left.opponentReady(), "and forgets that anyone was ready");
  check(!left.enemyFleetKnown(), "and forgets their fleet");
  check(left.fleet().placedCount() == 0, "and clears our own board");

  if (failures == 0) { printf("rooms: all checks passed\n"); return 0; }
  printf("rooms: %d CHECK(S) FAILED\n", failures);
  return 1;
}
