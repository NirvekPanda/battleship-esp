// WASM host backend: the same game core the firmware runs, with the browser
// standing in for the hardware. Keys replace the 5-way switch, and the two
// Screen buffers are handed to JS as raw pointers for the page to unpack
// into its two canvases -- no serial link, no protocol, no server.
#include <emscripten/emscripten.h>
#include <stdint.h>
#include "game.h"

namespace {
game::Game g;
game::Input in;
}  // namespace

// Bit layout matches the firmware's button trace order: U R D L C.
enum : int { M_UP = 1, M_RIGHT = 2, M_DOWN = 4, M_LEFT = 8, M_CENTER = 16 };

extern "C" {

EMSCRIPTEN_KEEPALIVE void sim_begin() { g.begin(); }

EMSCRIPTEN_KEEPALIVE void sim_set_input(int mask) {
  in.up     = mask & M_UP;
  in.right  = mask & M_RIGHT;
  in.down   = mask & M_DOWN;
  in.left   = mask & M_LEFT;
  in.center = mask & M_CENTER;
}

// Page selection. These exist only for the browser's inspector buttons: the
// firmware never calls them, so the device stays on the start page. The
// rendering they select lives in the shared core, so a page shown here is
// pixel-identical to what the ESP32 would show in the same state.
// The browser owns the socket, so it tells the core how the link is getting
// on; the core only decides what to draw and when to leave the page.
EMSCRIPTEN_KEEPALIVE void sim_set_link(int state) {
  g.setLink(static_cast<game::Link>(state));
}

// ---- the packet seam ----
// The core owns no transport, so the page carries packets for it: whatever
// arrives from the relay is pushed in here, and whatever the core wants to
// say is drained out. Fields are passed as plain ints rather than a struct so
// there is no shared memory layout for the two languages to disagree about.
EMSCRIPTEN_KEEPALIVE void sim_set_self_peer(int id) {
  g.setSelfPeer(static_cast<uint8_t>(id));
}

EMSCRIPTEN_KEEPALIVE void sim_receive(int v, int seq, int src, int dst, int t, int a, int b) {
  game::NetPacket p{};
  p.v = static_cast<uint8_t>(v);
  p.seq = static_cast<uint16_t>(seq);
  p.src = static_cast<uint8_t>(src);
  p.dst = static_cast<uint8_t>(dst);
  p.t = static_cast<uint8_t>(t);
  p.a = static_cast<uint8_t>(a);
  p.b = static_cast<uint8_t>(b);
  g.receive(p);
}

// Returns a pointer to seven bytes -- v, seq lo, seq hi, src, dst, t, a, b is
// eight, so seq is split -- or null when the outbox is empty. A static buffer
// rather than an allocation: the caller reads it before calling again.
namespace {
uint8_t outbound[8];
}
EMSCRIPTEN_KEEPALIVE const uint8_t *sim_take_outbound() {
  game::NetPacket p{};
  if (!g.takeOutbound(p)) return nullptr;
  game::packWire(p, outbound);
  return outbound;
}

EMSCRIPTEN_KEEPALIVE int sim_my_turn() { return g.myTurn() ? 1 : 0; }

// What the crosshair is on and whether a shot is outstanding. The page has no
// use for these; a test driving the game does, and guessing at them from
// pixels is how a test comes to believe something the game never said.
EMSCRIPTEN_KEEPALIVE int sim_aim_col() { return g.aim().col; }
EMSCRIPTEN_KEEPALIVE int sim_aim_row() { return g.aim().row; }
EMSCRIPTEN_KEEPALIVE int sim_in_flight() { return g.inFlight() ? 1 : 0; }

EMSCRIPTEN_KEEPALIVE void sim_set_page(int page) {
  g.setPage(static_cast<game::Page>(page));
}

// ---- the five rooms ----
// Names do not fit in the 8-byte frame, so the page fetches the list and
// hands it to the core to draw and steer -- exactly what the firmware does
// with the same calls. The browser therefore gets the panel's own lobby
// selection rather than a second, differently-behaved list of its own.
//
// The names go through static staging buffers rather than an allocation: the
// page writes UTF-8 into one and calls the setter, which copies it straight
// into the entry before the next name is written.
namespace {
char roomA[game::LOBBY_NAME_CHARS + 1];
char roomB[game::LOBBY_NAME_CHARS + 1];
}
EMSCRIPTEN_KEEPALIVE char *sim_room_a_buf() { return roomA; }
EMSCRIPTEN_KEEPALIVE char *sim_room_b_buf() { return roomB; }
EMSCRIPTEN_KEEPALIVE int sim_lobby_name_max() { return game::LOBBY_NAME_CHARS; }

EMSCRIPTEN_KEEPALIVE void sim_clear_rooms() { g.clearRooms(); }

EMSCRIPTEN_KEEPALIVE void sim_set_room(int n, int count, int yours) {
  g.setRoom(n, static_cast<uint8_t>(count), roomA, roomB, yours != 0);
}

// The gestures live in the core, so the page reads decisions rather than
// button states: a press joins, a hold goes back, and a press carried in from
// the page before is ignored -- identically on a board and in a browser.
EMSCRIPTEN_KEEPALIVE int sim_take_room_join() { return g.takeRoomJoin(); }
EMSCRIPTEN_KEEPALIVE int sim_take_room_leave() { return g.takeRoomLeave() ? 1 : 0; }
EMSCRIPTEN_KEEPALIVE void sim_set_my_room(int n) { g.setMyRoom(n); }
EMSCRIPTEN_KEEPALIVE int sim_room_count() { return game::ROOM_COUNT; }

// The two players by name, through the same staging buffers the room list
// uses: A is us, B is them.
EMSCRIPTEN_KEEPALIVE void sim_set_names() { g.setNames(roomA, roomB); }

// Which page the core is on, so the host can tell a page it chose from one
// the core moved to by itself.
EMSCRIPTEN_KEEPALIVE int sim_page() { return static_cast<int>(g.page()); }

EMSCRIPTEN_KEEPALIVE void sim_set_hit_ship(int ship) {
  g.setHitShip(static_cast<game::ShipType>(ship));
}

EMSCRIPTEN_KEEPALIVE void sim_tick(unsigned int nowMs) { g.tick(in, nowMs); }

EMSCRIPTEN_KEEPALIVE const uint8_t *sim_top() { return g.top().buffer(); }
EMSCRIPTEN_KEEPALIVE const uint8_t *sim_bottom() { return g.bottom().buffer(); }
EMSCRIPTEN_KEEPALIVE int sim_buf_size() { return gfx::Screen::size(); }
EMSCRIPTEN_KEEPALIVE int sim_width() { return gfx::W; }
EMSCRIPTEN_KEEPALIVE int sim_height() { return gfx::H; }

}  // extern "C"
