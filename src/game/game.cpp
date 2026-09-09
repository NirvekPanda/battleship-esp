#include "game.h"

#include "sprite.h"

namespace game {

namespace {
// One period of a sine, scaled to +/-127. A table rather than <math.h> so the
// wave costs no floating point on a chip without an FPU, and so the browser
// and the device produce identical pixels.
static const int8_t SIN64[64] = {
       0,   12,   25,   37,   49,   60,   71,   81,
      90,   98,  106,  112,  117,  122,  125,  126,
     127,  126,  125,  122,  117,  112,  106,   98,
      90,   81,   71,   60,   49,   37,   25,   12,
       0,  -12,  -25,  -37,  -49,  -60,  -71,  -81,
     -90,  -98, -106, -112, -117, -122, -125, -126,
    -127, -126, -125, -122, -117, -112, -106,  -98,
     -90,  -81,  -71,  -60,  -49,  -37,  -25,  -12,
};

// Centre a run of text of the given scale on a 128px-wide panel. The rule
// itself lives in gfx (see screen.h): every page centres the same way.
int centerX(const char *s, int scale) { return gfx::centerScaledX(s, scale); }

// The rule itself lives in gfx (see screen.h): every page centres the same
// way, in the small font as in the large one.
int centerTinyX(const char *s, int left, int right) {
  return gfx::centerTinyIn(s, left, right);
}

int clampv(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
}  // namespace

const char *teamName(Team t) { return t == Team::Red ? "RED" : "BLACK"; }

void Game::begin() {
  _page = Page::Start;
  _fleet.reset();
  _press.reset();
  _lastStep = 0;

  _hover = 0;
  _active = -1;
  _place = Coord{0, 0};
  _vertical = true;

  _aim = Coord{HEADER, HEADER};
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++) {
      _tracking[r][c] = CellState::Empty;
      _incoming[r][c] = CellState::Empty;
    }

  _myTurn = (_self == PEER_A);
  _inFlight = false;
  _pendingResult = -1;
  _pendingOver = false;
  // _everLinked deliberately survives begin(): a reset in the middle of a
  // session must not make a linked board forget it has an opponent.
  _won = false;
  _enemySunk = 0;
  _practice = true;
  _oppJoined = false;
  _oppReady = false;
  _acked = false;
  _nextHello = 0;
  _enemy.reset();
  _enemyShipsKnown = 0;
  _shotAt = Coord{0, 0};
  _outCount = 0;

  render(0);
}

// Jumping straight to a page is a browser convenience, not part of the game.
// Landing on Match without a fleet would show an empty board and shots that
// can never hit, so a jump past placement fills the board with the rulebook's
// Figure 4 layout to look at.
// Which pages are "past placement". Named rather than compared ordinally:
// Waiting, Lobby and Rooms were APPENDED to the enum -- the numbering is an
// interface shared with web/main.ts, so they could not be slotted in where
// they belong in the flow -- and `p >= Page::Starting` therefore swept them
// in. Opening the rooms page stamped the demo fleet onto the board, and the
// player then arrived at placement with five ships they never laid out and no
// way to finish, since allPlaced() was already true.
static bool showsABoard(Page p) {
  return p == Page::Starting || p == Page::Match || p == Page::Hit ||
         p == Page::Miss || p == Page::Sunk || p == Page::Over;
}

void Game::setPage(Page p) {
  if (showsABoard(p) && !_fleet.allPlaced()) {
    _fleet.reset();
    _fleet.place(0, Coord{4, 0}, false);
    _fleet.place(1, Coord{2, 2}, true);
    _fleet.place(2, Coord{6, 5}, false);
    _fleet.place(3, Coord{0, 6}, true);
    _fleet.place(4, Coord{7, 8}, true);
  }
  _page = p;
  _press.reset();
  // A button that is already down when a page opens has to be seen as down by
  // that page, or the press that left the page before becomes a fresh press
  // here -- which is how choosing ONLINE used to join lobby 1 on the way past.
  _roomCentre = _centreLevel;
  _roomArmed = false;
  _roomHeld = false;
  // Both timed pages start their clock on the next tick, which is the first
  // time a nowMs is in hand. Zero is the "not started yet" sentinel; without
  // it a jump straight to a result page would time out on arrival, because a
  // deadline of zero is always already past.
  if (p == Page::Starting) _waveUntil = 0;
  if (p == Page::Over) _overSince = 0;
  if (p == Page::Hit || p == Page::Miss || p == Page::Sunk) _resultUntil = 0;
  // Jumping past the wave skips where _practice is decided, and a browser
  // dropped straight onto the match with no relay behind it would fire one
  // shot and then wait forever for an answer nobody is going to send.
  //
  // The pages are named, not compared: Waiting (9) and Rooms (10) were
  // appended to the enum after Match (4), so ">= Match" swept them in -- and
  // a server page packet to Waiting then cleared _inFlight mid-match, which
  // made onResult drop the outstanding answer and left that player unable to
  // fire again for the rest of the game.
  if (p == Page::Match || p == Page::Hit || p == Page::Miss ||
      p == Page::Sunk || p == Page::Over) {
    _practice = !_everLinked;  // solo only if a link was NEVER seen
    if (_practice) _myTurn = true;
    _inFlight = false;
    _pendingResult = -1;
  }
}

// Move a cursor at a fixed rate regardless of how often a backend calls us:
// the browser runs at 60fps and the device does not. loBound is HEADER for
// the aiming cursor, which may sit on the label strips, and 0 for placement,
// where every position must be a real cell.
bool Game::stepCursor(const Input &in, uint32_t nowMs, Coord &c, int loBound) {
  if (!(in.up || in.down || in.left || in.right)) return false;
  if (nowMs - _lastStep < STEP_MS) return false;
  _lastStep = nowMs;
  if (in.up) c.row--;
  if (in.down) c.row++;
  if (in.left) c.col--;
  if (in.right) c.col++;
  c.col = clampv(c.col, loBound, COLS - 1);
  c.row = clampv(c.row, loBound, ROWS - 1);
  return true;
}

void Game::tick(const Input &in, uint32_t nowMs) {
  // The centre button's level, so a press that leaves one page is never seen
  // again as a fresh press by the page it lands on -- the button is usually
  // still held when the next page draws its first frame.
  _centreLevel = in.center;

  // Held centre outranks whatever the page would have done with it: a press
  // on its way to leaving must not also be firing, turning or joining.
  if (updateLeaveHold(in, nowMs) || (_leaveSince != 0 && nowMs - _leaveSince >= HOLD_MS)) {
    render(nowMs);
    return;
  }

  switch (_page) {
    case Page::Start: {
      // Left and right choose; centre confirms. No longer "any press": the
      // title screen now asks a question, and a page that asks a question
      // cannot also be left by answering it with whichever button was nearest.
      if (in.left || in.right) {
        if (nowMs - _lastStep >= STEP_MS) {
          _lastStep = nowMs;
          _startOnline = in.left;
        }
      }
      _press.update(in.center, nowMs);
      if (_press.edge()) {
        _mode = _startOnline ? Mode::Online : Mode::Offline;
        _press.reset();
        _lastStep = nowMs;
        // Either way the next screen is the same one: wait for the other
        // player. What differs is only who is doing the waiting-for --
        // a relay, or the board on the other side of the table -- and the
        // core is deliberately unable to tell the difference.
        _page = Page::Connecting;
        _oppJoined = false;
        _acked = false;
        _nextHello = nowMs;
      }
      break;
    }

    case Page::Rooms:
      // Five rows, always: the cursor wraps around a list that does not
      // change length, so a room that empties under it is still a room.
      if ((in.up || in.down) && nowMs - _lastStep >= STEP_MS) {
        _lastStep = nowMs;
        _roomCursor += in.down ? 1 : -1;
        if (_roomCursor < 0) _roomCursor = ROOM_COUNT - 1;
        if (_roomCursor >= ROOM_COUNT) _roomCursor = 0;
      }
      updateRoomsPress(in, nowMs);
      break;


    case Page::Connecting: {
      // A real wait, not a timed animation. The page holds until both players
      // are demonstrably on the server: we have heard from them, and they
      // have answered us. There is no timeout and no skip, because "connected"
      // is a fact about two machines and cannot be assumed by one of them.
      //
      // The one exception is a board with no network at all -- WIFI_SSID
      // empty, so Link::Offline. Nothing can ever arrive there, and blocking
      // would make an unconfigured board unusable rather than merely
      // opponent-less; it plays solo instead.
      if (_link == Link::Offline) {
        _page = Page::Place;
        _press.reset();
        _lastStep = nowMs;
        break;
      }

      // Say hello, and keep saying it. The other player may not have arrived
      // yet, and a Join sent into an empty room is simply lost.
      if (_link == Link::Online && (int32_t)(nowMs - _nextHello) >= 0) {
        _nextHello = nowMs + HELLO_EVERY_MS;
        send(PacketType::Join, 0, 0);
      }

      if (handshakeDone()) {
        _page = Page::Place;
        _press.reset();
        _lastStep = nowMs;
      }
      break;
    }

    case Page::Place:
      updatePlace(in, nowMs);
      break;

    case Page::Waiting:
      // Blocks, like the connecting page before it, and for the same reason:
      // both fleets have to be on the board before a shot means anything, so
      // there is no timeout and no skip.
      // Both conditions, not just the announcement: their layout has to have
      // arrived too, or the ship list would be drawn from nothing on the
      // first frame of the match.
      if (_oppReady && enemyFleetKnown()) {
        _page = Page::Starting;
        _waveUntil = 0;
        _press.reset();
      }
      break;

    case Page::Starting:
      // Whether there is anyone on the other end is settled here, once, so it
      // cannot change under the match: a linked board plays the packet game
      // and turns alternate, an unlinked one resolves its own shots so it is
      // still playable on a bench -- the same reason the connecting page can
      // be pressed through.
      _practice = !_everLinked;  // solo only if a link was NEVER seen
      if (_practice) _myTurn = true;
      // The wave is a fixed beat, not something to press through.
      if (_waveUntil == 0) _waveUntil = nowMs + WAVE_MS;
      if ((int32_t)(nowMs - _waveUntil) >= 0) {
        _page = Page::Match;
        _press.reset();
      }
      break;

    case Page::Match:
      // A result that landed between ticks starts its overlay now, on the
      // first clock we have been given since it arrived.
      if (_pendingResult >= 0) {
        showResult(static_cast<ShotResult>(_pendingResult), nowMs);
        _pendingResult = -1;
        break;
      }
      updateMatch(in, nowMs);
      break;

    case Page::Hit:
    case Page::Miss:
    case Page::Sunk:
      // The result stands for OVERLAY_MS and then the board comes back. A
      // press skips the wait rather than being the only way out, so the game
      // never sits waiting on a player who has looked away.
      if (_resultUntil == 0) _resultUntil = nowMs + OVERLAY_MS;
      _press.update(in.center, nowMs);
      if ((int32_t)(nowMs - _resultUntil) >= 0 || _press.edge()) {
        // The last announcement of the match is followed by the verdict, not
        // by the board: there is nothing left to shoot at.
        _page = _pendingOver ? Page::Over : Page::Match;
        _pendingOver = false;
        _press.reset();
      }
      break;

    case Page::Over: {
      // The verdict first, alone on the panel and unpressable: the press that
      // fired the winning shot must not skip the answer to it. Then both
      // boards, for as long as anyone wants to look at them -- there is no
      // timer on that, only a press.
      if (_overSince == 0) _overSince = nowMs;
      const bool banner = nowMs - _overSince < VERDICT_MS;
      _press.update(in.center, nowMs);
      if (!banner && _press.edge()) {
        forgetMatch();
        _myRoom = 0;
        _page = Page::Rooms;
      }
      break;
    }
  }

  render(nowMs);
}

void Game::render(uint32_t nowMs) {
  _top.clear();
  _bot.clear();

  // The countdown covers whatever page is underneath it. It is not a page of
  // its own: it can be let go of, and then the game is exactly where it was.
  if (_leaveSince != 0 && nowMs - _leaveSince >= HOLD_MS) {
    const uint32_t held = nowMs - _leaveSince;
    const uint32_t left = held >= LEAVE_HOLD_MS ? 0 : LEAVE_HOLD_MS - held;
    renderLeaving(nowMs, static_cast<int>(left / 1000) + 1);
    return;
  }

  switch (_page) {
    case Page::Start:      renderStart(); break;
    case Page::Rooms:      renderRooms(nowMs); break;
    case Page::Connecting: renderConnecting(nowMs); break;
    case Page::Place:    renderPlace(); break;
    case Page::Waiting:  renderWaiting(); break;
    case Page::Starting: renderStarting(nowMs); break;
    case Page::Match:    renderMatch(); break;
    case Page::Hit:
    case Page::Miss:
    case Page::Sunk:     renderResult(); break;
    case Page::Over:     renderOver(nowMs); break;
  }
}

// Top: the title, and the choice that has to be made before anything else --
// whether this board looks for a relay or for the board next to it. Bottom:
// the ship, as before.
void Game::renderStart() {
  _top.textScaled(centerX("BATTLESHIP", 1), 6, "BATTLESHIP", 1);

  // Two buttons side by side under the title, the selected one filled. Filled
  // rather than outlined, because on a 1-bit panel that is the only
  // difference that survives being looked at from across a table.
  constexpr int BW = 52, BH = 18, GAP = 8;
  const int y = 24;
  const int lx = (gfx::W - (2 * BW + GAP)) / 2;
  const int rx = lx + BW + GAP;

  for (int i = 0; i < 2; i++) {
    const int x = i == 0 ? lx : rx;
    const bool on = (i == 0) == _startOnline;
    const char *label = i == 0 ? "ONLINE" : "OFFLINE";
    _top.fillRect(x, y, BW, BH, on);
    _top.drawRect(x, y, BW, BH, true);
    // The label is drawn in the opposite ink to the button it sits on.
    _top.text(x + (BW - gfx::Screen::textWidth(label)) / 2, y + 6, label, !on);
  }

  const char *hint = _startOnline ? "PLAY OVER THE NETWORK" : "PLAY THE BOARD NEXT TO YOU";
  _top.textTiny(centerTinyX(hint, 0, gfx::W - 1), 50, hint);

  gfx::SHIP.blitCentered(_bot);
}


// ---- the five numbered lobbies ------------------------------------------
// Drawn by the core rather than by whoever is hosting it, so the browser and
// the board pick a room on the same screen with the same cursor. The one
// thing a browser does that a board cannot is type a name, and that is the
// whole of the difference between them.

namespace {
// Copy a name in truncated, the way the lobby list does.
void copyName(char *dst, const char *src) {
  int n = 0;
  for (; src && src[n] && n < LOBBY_NAME_CHARS; n++) dst[n] = src[n];
  dst[n] = '\0';
}
}  // namespace

void Game::setNames(const char *you, const char *them) {
  copyName(_myName, you);
  copyName(_theirName, them);
}

void Game::clearRooms() {
  for (int i = 0; i < ROOM_COUNT; i++) {
    _rooms[i].count = 0;
    _rooms[i].yours = false;
    _rooms[i].a[0] = '\0';
    _rooms[i].b[0] = '\0';
  }
}

void Game::setRoom(int n, uint8_t count, const char *a, const char *b, bool yours) {
  if (n < 1 || n > ROOM_COUNT) return;
  RoomEntry &e = _rooms[n - 1];
  e.count = count;
  e.yours = yours;
  copyName(e.a, a);
  copyName(e.b, b);
}

// Centre on the rooms page: a press joins the room under the cursor. The hold
// that goes back belongs to updateLeaveHold(), which works from every page of
// a game rather than only this one.
//
// The join lands on the RELEASE, not on the press, because a press that turns
// into a hold must not also have joined something on its way there. And a
// press that was already down when the page opened is ignored entirely --
// _roomArmed is only set by a press that began here -- since otherwise the
// press that chose ONLINE carries straight through and joins room 1 before
// the player has seen the list.
// Which pages a game can be left FROM. Not the title, which is not in a game,
// and not the connecting page, which has nothing to leave yet.
static bool inAGame(Page p) {
  return p == Page::Rooms || p == Page::Place || p == Page::Waiting ||
         p == Page::Starting || p == Page::Match || p == Page::Hit ||
         p == Page::Miss || p == Page::Sunk || p == Page::Over;
}

// Centre held, anywhere in a game: go back to the lobby list.
//
// Counted down rather than done at once, and counted down on the screen: five
// seconds is long enough that nobody leaves a match by resting a thumb on the
// button, and showing the count is both the warning and the way out of it --
// let go and nothing happened.
bool Game::updateLeaveHold(const Input &in, uint32_t nowMs) {
  if (!in.center || !inAGame(_page)) {
    _leaveSince = 0;
    return false;
  }
  if (_leaveSince == 0) {
    _leaveSince = nowMs;
    return false;
  }
  if (nowMs - _leaveSince < LEAVE_HOLD_MS) {
    // Once the count is showing, the page underneath stops taking input: a
    // hold that is on its way to leaving must not also be firing shots.
    const bool counting = nowMs - _leaveSince >= HOLD_MS;
    if (counting) {
      // And the press is spent. Without this, letting go to cancel would be
      // read by the rooms page as the release that joins a lobby -- the exact
      // opposite of what the player just decided.
      _roomArmed = false;
      _roomHeld = true;
    }
    return counting;
  }

  // Gone. The board is cleared here rather than by the backend, because what
  // is left of a match you have walked out of is not yours to keep: the next
  // lobby has to start from nothing.
  _leaveSince = 0;
  _roomLeave = true;
  _myRoom = 0;
  forgetMatch();
  _page = Page::Rooms;
  return true;
}

// Everything a match consisted of. Called when leaving one, and by the
// backend when the seat is gone -- the two ways a game ends without a winner.
// Everything a match consisted of. Called when leaving one, and by the
// backend when the seat is gone -- the two ways a game ends without a winner.
//
// EVERYTHING, not just what is drawn. The counters are the half that bites:
// a player who walked out having sunk three ships carried _enemySunk into the
// next game and won it two ships early, and an _oppReady left standing let
// the waiting page through before the new opponent had said anything. The
// browser never calls begin() on this path, so what is not cleared here is
// not cleared at all.
void Game::forgetMatch() {
  _fleet.reset();
  _enemy.reset();
  _enemySunk = 0;
  _enemyShipsKnown = 0;
  _oppReady = false;
  _won = false;
  _sunkWasMine = false;
  _shotAt = Coord{0, 0};
  _aim = Coord{HEADER, HEADER};
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++) {
      _tracking[r][c] = CellState::Empty;
      _incoming[r][c] = CellState::Empty;
    }
  _active = -1;
  _hover = 0;
  _place = Coord{0, 0};
  _vertical = true;
  _inFlight = false;
  _myTurn = true;
  _oppJoined = false;
  _acked = false;
  _pendingOver = false;
  _pendingResult = -1;
  _resultUntil = 0;
  _outCount = 0;
}

// The countdown itself: the number over a blacked-out board, so what is
// underneath is plainly no longer the thing being looked at.
void Game::renderLeaving(uint32_t nowMs, int count) {
  // The wave first, then a band cleared through it for the words. Drawing the
  // text first and the water over it is what made the message unreadable: a
  // 1-bit panel has no way to put one thing in front of another except by
  // taking the other one out.
  drawWave(_top, nowMs);
  _top.fillRect(0, 0, gfx::W, 30, false);

  const char *title = "RETURNING TO LOBBY";
  _top.text(centerX(title, 1), 6, title);
  const char *sub = "LET GO TO STAY IN THE GAME";
  _top.textTiny(centerTinyX(sub, 0, gfx::W - 1), 20, sub);

  if (count < 1) count = 1;
  if (count > 9) count = 9;
  char n[2] = {static_cast<char>('0' + count), '\0'};
  // The grid is not drawn at all rather than drawn and covered: a blacked-out
  // panel with one number on it cannot be misread as a board.
  const int scale = 5;
  _bot.textScaled(centerX(n, scale), (gfx::H - 7 * scale) / 2, n, scale);
}

void Game::updateRoomsPress(const Input &in, uint32_t nowMs) {
  if (in.center) {
    if (!_roomCentre) {
      _roomCentre = true;
      _roomArmed = true;
      _roomCentreSince = nowMs;
    }
    return;
  }

  if (_roomCentre && _roomArmed && !_roomHeld && !roomFull(roomChoice())) {
    _roomJoin = roomChoice();
  }
  _roomCentre = false;
  _roomArmed = false;
  _roomHeld = false;
}

int Game::takeRoomJoin() {
  const int n = _roomJoin;
  _roomJoin = 0;
  return n;
}

bool Game::takeRoomLeave() {
  const bool go = _roomLeave;
  _roomLeave = false;
  return go;
}

void Game::renderRooms(uint32_t nowMs) {
  _top.text(centerX("PICK A LOBBY", 1), 2, "PICK A LOBBY");
  _top.hLine(0, 12, gfx::W, true);

  for (int i = 0; i < ROOM_COUNT; i++) {
    const RoomEntry &e = _rooms[i];
    const int y = 16 + i * 8;
    const bool on = (i == _roomCursor);
    if (on) _top.fillRect(0, y - 1, gfx::W, 8, true);

    // "1 2/2" then who is in it. The count is drawn from its parts rather
    // than formatted, since the only numbers it can hold are 0..2 and the
    // room number, and a printf costs more flash than it saves here.
    char head[6] = {(char)('0' + i + 1), ' ', (char)('0' + (e.count > 2 ? 2 : e.count)),
                    '/', '2', '\0'};
    _top.text(2, y, head, !on);

    // The rest of the line says who, in as many characters as are left.
    char who[LOBBY_NAME_CHARS * 2 + 4];
    int n = 0;
    if (e.count == 0) {
      for (const char *c = "NO PLAYERS"; *c; c++) who[n++] = *c;
    } else {
      for (const char *c = e.a; *c; c++) who[n++] = *c;
      if (e.count >= 2) {
        for (const char *c = " V "; *c; c++) who[n++] = *c;
        for (const char *c = e.b; *c; c++) who[n++] = *c;
      }
    }
    who[n] = '\0';
    // Tiny type for the names: a 4px cell fits 22 characters past the count,
    // where the 6px one would cut "ann V bo" in half on a full room.
    _top.textTiny(38, y + 1, who, !on);

    // The room that is yours: waiting in it, or with a match in it to walk
    // back into.
    if (e.yours || _myRoom == i + 1) gfx::drawTick(_top, gfx::W - 9, y, 7, !on);
  }

  const int under = roomChoice();
  const bool back = under >= 1 && _rooms[under - 1].yours && _rooms[under - 1].count >= 2;
  const char *foot = back     ? "CENTRE TO REJOIN YOUR MATCH"
                     : _myRoom ? "HOLD CENTRE TO LEAVE THIS LOBBY"
                               : "CENTRE TO JOIN THIS LOBBY";
  _top.textTiny(centerTinyX(foot, 0, gfx::W - 1), 57, foot);

  drawWave(_bot, nowMs);
}

// Waiting on the link. The top panel says what is happening and the bottom
// runs a bar back and forth, because a still screen and a hung screen look
// identical and this page is exactly where a player would suspect the latter.
void Game::renderConnecting(uint32_t nowMs) {
  _top.textScaled(centerX("CONNECTING", 1), 8, "CONNECTING", 1);

  // Name the step that is actually outstanding. "Connecting" covers four
  // different situations, and which one you are in decides what to go and do
  // about it: turn the relay on, check the address, or fetch the other player.
  const char *state = _link == Link::Offline      ? "NO NETWORK"
                      : _link != Link::Online     ? "FINDING SERVER"
                      : !_oppJoined               ? "WAITING FOR PLAYER 2"
                      : !_acked                   ? "SAYING HELLO"
                                                  : "READY";
  _top.text(centerX(state, 1), 24, state);

  // The two halves of the handshake, shown separately, because they fail for
  // different reasons: we may hear them without them hearing us.
  const char *heard = _oppJoined ? "HEARD THEM  YES" : "HEARD THEM  NO";
  const char *reply = _acked ? "THEY HEARD US  YES" : "THEY HEARD US  NO";
  _top.textTiny(centerTinyX(heard, 0, gfx::W - 1), 40, heard);
  _top.textTiny(centerTinyX(reply, 0, gfx::W - 1), 50, reply);

  // Moving water below, so a page that is waiting cannot be mistaken for a
  // page that has hung -- which matters far more now that it never gives up
  // on its own.
  drawWave(_bot, nowMs);
}

// Three crests at different heights, speeds and depths, which is enough for
// the eye to read it as water rather than as three sine curves. Phase comes
// from nowMs, so it runs at the same speed on a 60Hz browser and a 50Hz
// device.
void Game::drawWave(gfx::Screen &s, uint32_t nowMs) {
  struct Crest { int baseY, amp, speed, step; };
  static const Crest crests[3] = {{18, 7, 24, 3}, {34, 9, 16, 2}, {50, 6, 32, 4}};
  for (const Crest &w : crests) {
    const int phase = (int)(nowMs / (uint32_t)w.speed);
    for (int x = 0; x < gfx::W; x++) {
      const int i = ((x * w.step) / 2 + phase) & 63;
      const int y = w.baseY + (SIN64[i] * w.amp) / 127;
      s.pixel(x, y, true);
      s.pixel(x, y + 1, true);
    }
  }
}

// A stack of two tiny words over a count, centred in one of the side gutters.
// Both panels label their gutters this way -- PLACED SHIPS while laying out,
// SHIPS LEFT once the shooting starts -- so the layout is written once.
void Game::drawGutterCount(gfx::Screen &s, int x0, const char *l1, const char *l2,
                           int n, int of) {
  const int x1 = (x0 == GUTTER_L_LEFT) ? GUTTER_L_RIGHT : GUTTER_R_RIGHT;
  const char count[4] = {(char)('0' + n), '/', (char)('0' + of), '\0'};
  s.textTiny(centerTinyX(l1, x0, x1), 22, l1);
  s.textTiny(centerTinyX(l2, x0, x1), 30, l2);
  s.textTiny(centerTinyX(count, x0, x1), 40, count);
}

// The match is starting: the title on one panel, moving water on the other.
// Phase comes from nowMs, so the wave runs at the same speed on a 60Hz
// browser and a 50Hz device.
void Game::renderStarting(uint32_t nowMs) {
  _top.textScaled(centerX("STARTING", 1), 22, "STARTING", 1);
  _top.textScaled(centerX("MATCH", 2), 34, "MATCH", 2);
  drawWave(_bot, nowMs);
}

// Your fleet is down and theirs is not. The top panel says what is being
// waited for and the bottom keeps your own board up, so the wait is spent
// looking at the layout you are about to defend rather than at a spinner.
void Game::renderWaiting() {
  // Who is being waited for, by name, in one sentence read straight down the
  // panel: WAITING FOR / <them> / TO PLACE FLEET.
  //
  // Nothing about whether they are connected. Both players got here by
  // sitting down in the same lobby, and the relay pairs two people who are
  // both there -- so "they are here" is never news, and a line that is always
  // true is a line that stops being read.
  const char *who = _theirName[0] != '\0' ? _theirName : "THE OTHER PLAYER";

  _top.textScaled(centerX("WAITING FOR", 1), 16, "WAITING FOR", 1);
  _top.textTiny(centerTinyX(who, 0, gfx::W - 1), 30, who);
  _top.textScaled(centerX("TO PLACE FLEET", 1), 42, "TO PLACE FLEET", 1);

  drawPanelFrame(_bot);
  drawGrid(_bot);
  for (int i = 0; i < SHIP_COUNT; i++) {
    const Ship &s = _fleet.ship(i);
    if (s.placed) drawShip(_bot, s.origin, s.length(), s.vertical);
  }
  drawGutterCount(_bot, GUTTER_L_LEFT, "YOUR", "FLEET", _fleet.placedCount(), SHIP_COUNT);
}

// The shot result: the tracking grid stays up top so the shot keeps its
// context, and the bottom panel becomes one large framed announcement. The
// border is as thick as a grid label is tall, which is what makes it read as
// a frame rather than as a box.
// A framed announcement filling a panel: the border as thick as a grid label
// is tall, which is what makes it read as a frame rather than as a box.
static void drawAnnouncementFrame(gfx::Screen &s) {
  constexpr int MARGIN = 2;
  constexpr int BORDER = LABEL_H - 1;
  const int fw = gfx::W - 2 * MARGIN;
  const int fh = gfx::H - 2 * MARGIN;
  s.fillRect(MARGIN, MARGIN, fw, fh, true);
  s.fillRect(MARGIN + BORDER, MARGIN + BORDER, fw - 2 * BORDER, fh - 2 * BORDER, false);
}

void Game::renderResult() {
  // A sinking is the one result that names a ship, so it is set out as the
  // name over the word -- "CRUISER" above "SUNK" -- and put on the top panel,
  // where the eye already is while aiming. Both players see it: the one who
  // sank it and the one who lost it.
  if (_page == Page::Sunk) {
    drawAnnouncementFrame(_top);

    // Whose ship went down, and which one: "BLACK SUBMARINE". The owner is
    // the team, not the player's name -- a team is always short enough for a
    // 128px panel and always known to both sides, where a name is neither.
    // The sinking side reports its own team; the scoring side reports theirs.
    const char *owner = teamName(_page == Page::Sunk && !_sunkWasMine
                                     ? (_team == Team::Red ? Team::Black : Team::Red)
                                     : _team);
    // Built by hand rather than with snprintf: pulling the whole formatting
    // machinery into the firmware to join two words with a space is a poor
    // trade for the flash it costs. Names are truncated to NAME_CHARS so a
    // long one can never push the line off a 128px panel.
    // The OWNER is truncated, not the ship: a player picks their own name and
    // a team is a stand-in for it, while a ship name is one of five known
    // words and BATTLESHIP is meant to read as BATTLESHIP.
    constexpr int MAX_SHIP = 10;  // BATTLESHIP
    char line[NAME_CHARS + 1 + MAX_SHIP + 1];
    int n = 0;
    for (const char *c = owner; *c && n < NAME_CHARS; c++) line[n++] = *c;
    line[n++] = ' ';
    for (const char *c = shipName(_hitShip); *c && n < NAME_CHARS + 1 + MAX_SHIP; c++)
      line[n++] = *c;
    line[n] = '\0';

    // Two sizes: whichever fits. A long pairing like BLACK BATTLESHIP is
    // beyond double width, and shrinking it is better than clipping it.
    const int scale = gfx::Screen::textScaledWidth(line, 2) <= gfx::W - 16 ? 2 : 1;
    _top.textScaled(centerX(line, scale), 16, line, scale);

    constexpr int SCALE = 2;
    _top.textBubble(gfx::centerScaledX("SUNK", SCALE),
                    36, "SUNK", SCALE);

    renderOwnPanel();
    return;
  }

  renderTargetPanel();

  drawAnnouncementFrame(_bot);

  const char *word = _page == Page::Miss ? "MISS" : "HIT";
  constexpr int SCALE = 3;
  const int wx = (gfx::W - gfx::Screen::textScaledWidth(word, SCALE)) / 2 + SCALE;
  _bot.textBubble(wx, 25, word, SCALE);

  // Which ship, in the small font: the bubble is capped to the grid width so
  // it cannot cover the inventory, and a 5x7 name fits under it.
  if (_page != Page::Miss) {
    const char *name = shipName(_hitShip);
    _bot.text(centerX(name, 1), 45, name);
  }
}

}  // namespace game
