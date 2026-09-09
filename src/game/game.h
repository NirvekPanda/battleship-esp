#pragma once
#include <stdint.h>

#include "fleet.h"
#include "game_config.h"
#include "grid.h"
#include "input.h"
#include "netplay.h"
#include "press.h"
#include "screen.h"

namespace game {

// The two sides. Short on purpose: a team name is printed beside a ship name
// on a 128px panel, and these are the two that always fit.
enum class Team : uint8_t { Red, Black };
const char *teamName(Team t);

// How this board is playing, chosen on the title screen before any network
// exists -- which is the point, since a board with no WiFi configured can
// still play the board next to it.
enum class Mode : uint8_t {
  Online,   // through the relay: lobby, an opponent anywhere on the network
  Offline,  // board to board over ESP-NOW, or alone if there is nobody there
};

// How the backend's link to the other player is getting on. The core cannot
// open a socket -- it has no idea what one is -- so a backend sets this and
// the core only decides what to draw and when to move on.
enum class Link : uint8_t { Offline, Connecting, Online };

// Which screen the game is on. The order is part of the interface: the
// browser's page buttons pass these values straight through sim_set_page(),
// so web/main.ts holds the same numbering.
//
// Match is the page you shoot from -- it was called Placement before there
// was a real ship-placement page to confuse it with.
enum class Page : uint8_t {
  Start = 0,
  Connecting,  // waiting on the backend's link
  Place,     // lay out your own fleet
  Starting,  // "STARTING MATCH" + the wave
  Match,     // aim and fire
  Hit,
  Miss,
  Sunk,
  Over,  // the match is decided: every ship of one fleet is sunk
  // Appended rather than slotted in between Place and Starting, which is
  // where it sits in the flow: these numbers are an interface, mirrored in
  // web/main.ts and cast straight through by sim_set_page, so renumbering the
  // existing pages to keep the list in play order would break every caller.
  Waiting,  // your fleet is down; waiting for the other player
  Rooms,    // the five numbered lobbies, and which one to play in
};

// One of the five numbered lobbies, as a board holds it: how many are in it
// and who they are. The names are the whole of what a row shows, so red and
// black arrive as an ESP32's name rather than as a column.
constexpr int ROOM_COUNT = 5;
struct RoomEntry {
  uint8_t count;
  // Yours: the lobby you are waiting in, or the one holding the match you
  // walked away from. It is the row you may press even when it reads 2/2,
  // because the seat in it is already yours.
  bool yours;
  char a[LOBBY_NAME_CHARS + 1];
  char b[LOBBY_NAME_CHARS + 1];
};

// The whole game, free of any hardware dependency: it takes Input and
// produces two independent 128x64 Screens. Both the ESP32 firmware and the
// WASM host build compile this unchanged -- they differ only in where the
// Input comes from and where the Screens are pushed.
class Game {
public:
  void begin();

  // Advance one frame. nowMs is a free-running millisecond clock.
  void tick(const Input &in, uint32_t nowMs);

  const gfx::Screen &top() const { return _top; }
  const gfx::Screen &bottom() const { return _bot; }

  // Page selection, for a backend that wants to jump straight to a screen.
  // Only the browser's buttons use this; the game drives itself otherwise.
  void setPage(Page p);
  Page page() const { return _page; }
  void setHitShip(ShipType t) { _hitShip = t; }
  ShipType hitShip() const { return _hitShip; }
  void setTeam(Team t) { _team = t; }
  Team team() const { return _team; }
  Mode mode() const { return _mode; }

  // ---- the five numbered lobbies ----
  // Fetched by the backend and handed to the core to draw and steer: names do
  // not fit in an 8-byte frame, and the core owns no transport. Rooms always
  // number ROOM_COUNT, so a row that emptied is still there to be joined.
  void clearRooms();
  void setRoom(int n, uint8_t count, const char *a, const char *b, bool yours = false);

  // What the player asked for, read and cleared by the backend -- joining and
  // leaving are HTTP calls. The GESTURES live here rather than in each
  // backend, so a board and a browser cannot disagree about what a press
  // means: a press that was already down when the page opened is ignored, and
  // leaving is a hold counted down on the panel.
  int takeRoomJoin();   // the room to join, or 0 for nothing
  bool takeRoomLeave(); // they held centre: back to the lobby list

  // The room the cursor is on, 1..ROOM_COUNT. Never 0: there is always a row
  // to be on, whether or not anybody is in it.
  int roomChoice() const { return _roomCursor + 1; }
  // Full to everyone but its own players: a row that is yours is one you can
  // walk back into, which is what reconnecting to a match is.
  bool roomFull(int n) const {
    if (n < 1 || n > ROOM_COUNT) return true;
    return _rooms[n - 1].count >= 2 && !_rooms[n - 1].yours;
  }
  // Which room this player has sat down in, so the page can say so rather
  // than looking as though the press did nothing. 0 until they have.
  void setMyRoom(int n) { _myRoom = n; }
  int myRoom() const { return _myRoom; }

  // The two players by name, set by the backend when a match starts. The core
  // has no way to learn them -- a name does not fit in the frame -- and the
  // verdict is the one screen that has to say WHOSE win it is: "WIN" on both
  // panels is the same word to the winner and the loser.
  void setNames(const char *you, const char *them);
  const char *myName() const { return _myName; }
  const char *theirName() const { return _theirName; }

  // Set by the backend as its link comes up. The core watches it on the
  // connecting page and moves on by itself once the link is Online.
  void setLink(Link l) {
    _link = l;
    // Remembered, not just held. Whether this board is playing alone is
    // decided once, at the moment the last ship goes down, and deciding it
    // from the link's value at that instant makes it a coin toss: a board
    // that is linked but happened to be between polls -- or that had not
    // finished claiming its seat yet -- would latch solo for the whole match
    // and leave the other player waiting for a fleet that never comes.
    if (l == Link::Online) _everLinked = true;
  }
  Link link() const { return _link; }
  // True once a link has been seen at any point. This, not the link's current
  // value, is what decides solo play.
  bool everLinked() const { return _everLinked; }
  bool practice() const { return _practice; }

  // ---- the match link (see netplay.h) ----
  // Which seat this board sits in. PEER_A shoots first, which is the only
  // thing the two seats disagree about; a backend that has been given a seat
  // by the relay passes it straight through. Set it before the match starts.
  void setSelfPeer(uint8_t id) {
    _self = (id == PEER_B) ? PEER_B : PEER_A;
    _myTurn = (_self == PEER_A);
  }
  uint8_t selfPeer() const { return _self; }
  uint8_t opponentPeer() const { return _self == PEER_A ? PEER_B : PEER_A; }

  // One inbound packet from whatever transport the backend speaks. Invalid
  // packets, packets addressed elsewhere and packets the core did not ask for
  // are dropped rather than acted on.
  void receive(const NetPacket &p);

  // Drain the outbox, oldest first: true and fills `out` while there is one.
  // The backend loop is `while (g.takeOutbound(p)) send(p);`.
  bool takeOutbound(NetPacket &out);
  int outboundCount() const { return _outCount; }

  // Whose turn it is, and how the match ended. Strict alternation: a hit
  // ends your turn exactly as a miss does (PLAN.md 4.4).
  bool myTurn() const { return _myTurn; }
  // Where the crosshair is, and whether a shot of ours is still unanswered.
  // Read by the browser host so an automated game can see what a player would
  // see; the firmware never asks.
  Coord aim() const { return _aim; }
  bool inFlight() const { return _inFlight; }
  bool won() const { return _won; }
  bool opponentReady() const { return _oppReady; }
  // Both directions proven: they answered us, and we heard them.
  bool handshakeDone() const { return _oppJoined && _acked; }
  // Their whole fleet has arrived: five ships, all placed.
  bool enemyFleetKnown() const { return _enemyShipsKnown == SHIP_COUNT; }
  const Fleet &enemyFleet() const { return _enemy; }

  // Observable board state, for a backend or a test that wants to check what
  // the panels are showing without reading pixels.
  CellState tracking(Coord c) const { return _tracking[c.row][c.col]; }
  CellState incoming(Coord c) const { return _incoming[c.row][c.col]; }
  const Fleet &fleet() const { return _fleet; }

private:
  // ---- per-page update, defined beside that page's rendering ----
  void updatePlace(const Input &in, uint32_t nowMs);
  void updateMatch(const Input &in, uint32_t nowMs);
  void fire(uint32_t nowMs);

  // ---- netplay ----
  void send(PacketType t, uint8_t a, uint8_t b);
  void onShot(const NetPacket &p);      // the opponent fired at us
  void onResult(const NetPacket &p);    // the answer to the shot we fired
  void showResult(ShotResult r, uint32_t nowMs);
  void endMatch(bool won);

  // ---- rendering ----
  void render(uint32_t nowMs);
  void renderStart();
  void renderRooms(uint32_t nowMs);
  void renderLeaving(uint32_t nowMs, int count);
  // Holding centre to go back, from any page that is part of a game. Returns
  // true when it has drawn the countdown, which replaces the page under it.
  bool updateLeaveHold(const Input &in, uint32_t nowMs);
  void forgetMatch();
  void updateRoomsPress(const Input &in, uint32_t nowMs);
  void drawTeamShip(gfx::Screen &s, int x, int y, const char *word, int scale);
  void renderConnecting(uint32_t nowMs);
  void renderWaiting();
  void drawWave(gfx::Screen &s, uint32_t nowMs);
  void renderPlace();
  void renderStarting(uint32_t nowMs);
  void renderMatch();
  void renderResult();
  void renderOver();
  void renderTargetPanel();   // the tracking grid, shared by Match and results
  void renderOwnPanel();
  // One fleet drawn as a board: hulls, the damage on them, the misses around
  // them. Used for your own panel during a match and for THEIRS once it is
  // over, which is the same picture of a different fleet.
  void drawFleetBoard(gfx::Screen &s, const Fleet &f, const CellState marks[ROWS][COLS]);      // your board with its damage
  void drawEnemyRoster();
  void drawAimReadout();
  void drawTurnReadout();
  void drawGutterCount(gfx::Screen &s, int x, const char *l1, const char *l2, int n, int of);

  // Repeat-limited cursor movement, shared by placement and aiming. Returns
  // true if the cursor moved.
  bool stepCursor(const Input &in, uint32_t nowMs, Coord &c, int loBound);

  gfx::Screen _top, _bot;

  Page _page = Page::Start;
  Team _team = Team::Red;
  Mode _mode = Mode::Online;
  // Which of the two title-screen buttons is under the cursor.
  bool _startOnline = true;

  RoomEntry _rooms[ROOM_COUNT] = {};
  int _roomCursor = 0;
  int _myRoom = 0;
  // The centre button on the rooms page: whether it is down, when it went
  // down, whether that press started on this page, and whether the hold has
  // already been reported.
  bool _centreLevel = false;  // the button's level on the last tick, any page
  char _myName[LOBBY_NAME_CHARS + 1] = {0};
  char _theirName[LOBBY_NAME_CHARS + 1] = {0};
  bool _roomCentre = false;
  bool _roomArmed = false;
  bool _roomHeld = false;
  uint32_t _roomCentreSince = 0;
  int _roomJoin = 0;
  bool _roomLeave = false;
  // Holding centre anywhere in a game to go back to the lobby list. Zero
  // means the button is not down; the count is drawn once the hold passes
  // HOLD_MS and the leave happens at LEAVE_HOLD_MS.
  uint32_t _leaveSince = 0;
  Link _link = Link::Offline;
  bool _everLinked = false;

  // The connecting handshake. _oppJoined is their Join reaching us; _acked is
  // their Ack reaching us, which is the only proof our own Join arrived. Both
  // are required, so the page cannot be satisfied by traffic in one direction.
  bool _acked = false;
  uint32_t _nextHello = 0;
  Fleet _fleet;
  PressDetector _press;
  uint32_t _lastStep = 0;

  // ---- placement ----
  int _hover = 0;    // ship the picker is over
  int _active = -1;  // ship in hand, or -1 when the picker has focus
  Coord _place{0, 0};
  bool _vertical = true;

  // ---- match ----
  // The cursor starts on the corner of the labels, off the board both ways.
  Coord _aim{HEADER, HEADER};
  // What you know about the opponent's water. Only Hit and Miss go in it.
  CellState _tracking[ROWS][COLS] = {};
  // Where THEY have shot at US. Their hits are already visible in the fleet's
  // damage; this is what remembers the misses, so the water they have wasted
  // a shot on is marked and you can see where they have been.
  CellState _incoming[ROWS][COLS] = {};
  ShipType _hitShip = ShipType::Cruiser;
  // Whether the ship just sunk was ours. The announcement names its owner, so
  // the two sides of the same sinking must not both claim it.
  bool _sunkWasMine = false;
  uint32_t _resultUntil = 0;
  uint32_t _waveUntil = 0;

  // ---- netplay ----
  uint8_t _self = PEER_A;
  bool _myTurn = true;
  // A shot has been sent and its result has not come back. Nothing may be
  // fired in the meantime, so a slow link cannot become two shots in a turn.
  bool _inFlight = false;
  // The cell our in-flight shot was fired at: the result names an outcome,
  // not a coordinate, so this is what says where the mark goes.
  Coord _shotAt{0, 0};
  // A result that arrived between ticks, waiting for a nowMs to start its
  // overlay with. -1 when there is none.
  int8_t _pendingResult = -1;
  // The result page currently showing is the last of the match: when it
  // times out, the verdict follows instead of the board.
  bool _pendingOver = false;
  bool _won = false;
  // Sunk answers counted off the opponent's fleet. Their board is theirs, so
  // the only thing we know about it is what our results have told us.
  int _enemySunk = 0;
  // No opponent on the far end of the link: the board resolves its own shots
  // against its own fleet so it stays playable on a bench with no relay near
  // it, which is the same reason the connecting page can be pressed through.
  bool _practice = true;
  // What we know of the other player, learned from their packets. Both stay
  // false in a solo game, which is why an unlinked board never waits.
  bool _oppJoined = false;
  bool _oppReady = false;
  // The opponent's layout, as they reported it. It drives the ship list on
  // the top panel: which vessels are out there and how big they are, rather
  // than an assumed five. Their POSITIONS are deliberately never drawn on the
  // tracking grid -- knowing where they are is the whole game.
  Fleet _enemy;
  int _enemyShipsKnown = 0;
  uint16_t _seq = 0;
  NetPacket _outbox[OUTBOX_MAX];
  int _outCount = 0;
};

}  // namespace game
