#include "game.h"

// The match: aim at the top panel, watch your own board take damage on the
// bottom one. Two boards, never one -- the top panel is the tracking grid,
// which holds only what our results have told us about the opponent's water,
// and the bottom panel is our own Fleet with the damage the opponent has done
// to it. The two carry different marks on purpose: X/O on the tracking grid
// (drawCell), a knocked-out X on a filled cell for a hit on us (drawOwnHit).
//
// Firing sends a `shot` and waits; the answer arrives as a `result`. Nothing
// here knows how either travels -- see netplay.h.
namespace game {

namespace {
// The enemy roster in the right gutter: two rows of hulls at grid scale.
constexpr int PITCH = 10;
constexpr int HULL_W = CELL - 1;
constexpr int ROSTER_GAP = 4;

// Which cell of a ship a coordinate is, or -1. Ship::occupies() answers the
// yes/no; the index is what the hits bitmask is keyed by.
int cellIndexOf(const Ship &s, Coord c) {
  for (int i = 0; i < s.length(); i++) {
    const Coord k = s.cell(i);
    if (k.col == c.col && k.row == c.row) return i;
  }
  return -1;
}

// The match is under way once the shooting page has been reached, results
// and all. Before that a shot has nowhere to land, and after Over it has
// nothing to decide.
bool inMatch(Page p) {
  // Every page from the wave onwards, not just the shooting page. The fleets
  // are final once the wave starts, so a shot can always be answered from
  // there -- and it MUST be. The two players leave the waiting page when the
  // other's ready reaches them, which is a poll apart, so one can be firing
  // while the other is still watching the wave. A shot dropped there is never
  // answered, and the shooter waits on a result that will never come.
  //
  // Answering after the match is over is harmless and closes the same hole
  // for a shot already in flight when the last ship went down.
  return p == Page::Starting || p == Page::Match || p == Page::Hit ||
         p == Page::Miss || p == Page::Sunk || p == Page::Over;
}
}  // namespace

void Game::updateMatch(const Input &in, uint32_t nowMs) {
  _press.update(in.center, nowMs);
  stepCursor(in, nowMs, _aim, HEADER);
  // Aiming fires on the press itself, not on a gesture. The button means one
  // thing here, so waiting out the double window would put DOUBLE_MS between
  // the click and the shot -- and a fast second press is a second shot rather
  // than being swallowed as half of a double.
  if (_press.edge()) fire(nowMs);
}

// ---- the link ------------------------------------------------------------

void Game::send(PacketType t, uint8_t a, uint8_t b) {
  if (_outCount >= OUTBOX_MAX) return;  // a backend that never drains
  NetPacket p;
  p.v = PROTOCOL_VERSION;
  p.seq = _seq++;
  p.src = _self;
  p.dst = opponentPeer();
  p.t = static_cast<uint8_t>(t);
  p.a = a;
  p.b = b;
  _outbox[_outCount++] = p;
}

bool Game::takeOutbound(NetPacket &out) {
  if (_outCount == 0) return false;
  out = _outbox[0];
  for (int i = 1; i < _outCount; i++) _outbox[i - 1] = _outbox[i];
  _outCount--;
  return true;
}

// Inbound. The relay validates too, but ESP-NOW has no relay in the middle,
// so the core refuses the same packets either way rather than trusting the
// transport it happens to be on today.
void Game::receive(const NetPacket &p) {
  if (!packetValid(p)) return;
  if (p.src == _self) return;  // never act on our own packet echoed back
  if (p.dst != _self && p.dst != PEER_BROADCAST) return;

  switch (p.type()) {
    case PacketType::Shot:   onShot(p); break;
    case PacketType::Result: onResult(p); break;

    // The other player announcing themselves, and announcing that their fleet
    // is down. These are what the waiting page is waiting for. They may well
    // arrive before we finish placing -- the relay replays its log to a client
    // reading from the start -- so they are recorded whenever they turn up
    // rather than only while that page is showing.
    // They are there. Say so back, every time: a repeated Join means our
    // earlier Ack did not reach them, and answering again is what closes the
    // handshake rather than leaving both sides waiting politely.
    case PacketType::Join:
      _oppJoined = true;
      send(PacketType::Ack, 0, 0);
      break;

    // Our own Join reached them. This is the half that cannot be inferred:
    // sending a Join proves nothing about whether anyone received it.
    case PacketType::Ack:
      _oppJoined = true;
      _acked = true;
      break;
    case PacketType::Ready: _oppJoined = true; _oppReady = true; break;

    // One ship of their layout. Recorded rather than counted, so a resend of
    // the same ship does not look like a second one and leave the fleet
    // permanently one short of complete.
    case PacketType::Fleet: {
      _oppJoined = true;
      const int i = p.a;
      const bool firstTime = !_enemy.ship(i).placed;
      _enemy.place(i, Coord{placementCol(p.b), placementRow(p.b)},
                   placementVertical(p.b));
      if (firstTime && _enemy.ship(i).placed) _enemyShipsKnown++;
      break;
    }

    // The match is on. Only the server says this, and it says it to both
    // players at once, which is what makes the two boards agree about who is
    // seat A -- and therefore about who shoots first.
    case PacketType::Start:
      if (p.src == PEER_SERVER) {
        setSelfPeer(p.a == 0 ? PEER_A : PEER_B);
        _team = p.b == 1 ? Team::Black : Team::Red;
        _practice = false;
        _everLinked = true;
        _oppJoined = true;
        _acked = true;
        if (_page == Page::Rooms || _page == Page::Connecting ||
            _page == Page::Start) {
          setPage(Page::Place);
        }
      }
      break;

    // Our OWN fleet, handed back by the server after a disconnection. The
    // board is rebuilt from the packets that built it the first time, so
    // there is no second description of a match anywhere to fall out of step.
    case PacketType::MyFleet:
      if (p.src == PEER_SERVER) {
        _fleet.place(p.a, Coord{placementCol(p.b), placementRow(p.b)},
                     placementVertical(p.b));
        // Coming back to a fleet already on the board means placement is
        // behind us.
        if (_fleet.allPlaced() && _page == Page::Place) {
          _page = Page::Waiting;
        }
      }
      break;

    // ---- the board handed back after a restart ----
    //
    // Accepted from the server only. These say what the board LOOKS LIKE,
    // where a shot and a result say what happened, so nothing is answered and
    // nothing is counted: applying a mark must not fire, and taking damage
    // must not send a result back to a player who is not waiting for one.
    case PacketType::Mark:
      if (p.src == PEER_SERVER && p.a < COLS * ROWS) {
        const Coord c{static_cast<int8_t>(p.a % COLS), static_cast<int8_t>(p.a / COLS)};
        const ShotResult r = static_cast<ShotResult>(p.b & 3);
        _tracking[c.row][c.col] =
            (r == ShotResult::Miss) ? CellState::Miss : CellState::Hit;
        // A sunk ship of theirs is one we know about, so the roster fills in
        // as it would have during the match itself -- and a hit that did not
        // sink anything still marks their hull, so the board revealed at the
        // end is the same board whether or not anyone restarted mid-game.
        if (r == ShotResult::Sunk) _enemy.sinkByType(static_cast<ShipType>(p.b >> 2));
        else if (r == ShotResult::Hit) _enemy.registerHit(c);
        _everLinked = true;
        _practice = false;
      }
      break;

    // A cell of our own water they have shot at: b says whether it hit.
    case PacketType::Damage:
      if (p.src == PEER_SERVER && p.a < COLS * ROWS) {
        const Coord c{static_cast<int8_t>(p.a % COLS), static_cast<int8_t>(p.a / COLS)};
        if (p.b != 0) {
          _fleet.registerHit(c);
          _incoming[c.row][c.col] = CellState::Hit;
        } else {
          _incoming[c.row][c.col] = CellState::Miss;
        }
        _everLinked = true;
        _practice = false;
      }
      break;

    case PacketType::Turn:
      if (p.src == PEER_SERVER) {
        _myTurn = p.a != 0;
        _inFlight = false;  // nothing of ours is outstanding after a restart
      }
      break;

    // The server marching us to a screen. Accepted from the server only: a
    // player being able to move the other player's screen would be a way to
    // cheat, not a feature.
    case PacketType::Page:
      if (p.src == PEER_SERVER && p.a <= static_cast<uint8_t>(Page::Waiting)) {
        setPage(static_cast<Page>(p.a));
      }
      break;

    default: break;  // reset/bye are the backend's business
  }
}

// The opponent fired at us. Resolve it against our OWN fleet and answer with
// exactly one result -- we are the only authority on our own water. Then the
// turn is ours, whatever the outcome was.
void Game::onShot(const NetPacket &p) {
  if (!inMatch(_page)) return;
  const Coord c{p.a, p.b};

  // A cell already shot at is answered as it was answered the first time
  // rather than counted twice: a duplicate is a resend, not a second shot.
  int idx = _fleet.shipAt(c);
  const int k = idx >= 0 ? cellIndexOf(_fleet.ship(idx), c) : -1;
  const bool already = k >= 0 && (_fleet.ship(idx).hits & (1u << k));
  ShotResult r;
  if (already) {
    r = _fleet.ship(idx).sunk() ? ShotResult::Sunk : ShotResult::Hit;
  } else {
    r = _fleet.registerHit(c);
    idx = _fleet.shipAt(c);
  }

  // Their shot, marked on our own water. A hit shows as damage to the hull
  // it struck; a miss would leave no trace at all, and then there is no way
  // to see where the other player has already been.
  _incoming[c.row][c.col] =
      (r == ShotResult::Miss) ? CellState::Miss : CellState::Hit;

  const uint8_t ship = (r == ShotResult::Miss || idx < 0) ? 0 : static_cast<uint8_t>(_fleet.ship(idx).type);
  send(PacketType::Result, static_cast<uint8_t>(r), ship);

  // Show the defender what just happened to them. The rulebook has the owner
  // of a ship announce that it has been sunk, so being the one struck is
  // exactly when you are meant to know about it -- and a resend must not
  // raise the announcement twice.
  if (!already) {
    if (r != ShotResult::Miss) {
      _hitShip = static_cast<ShipType>(ship);
      _sunkWasMine = true;  // it was our ship that was struck
    }
    if (_fleet.allSunk()) {
      // Seen, and then the match is over. Losing the last ship is worth being
      // told about before the result page says so.
      _won = false;
      _pendingOver = true;
    }
    _pendingResult = static_cast<int8_t>(r);
  }

  if (_fleet.allSunk()) return;  // the turn does not pass; the match is done
  _myTurn = true;
}

// The answer to our own shot: it lands on the tracking grid and ends our
// turn. A hit does not grant another shot -- "after a hit or a miss, your
// turn is over".
void Game::onResult(const NetPacket &p) {
  if (!_inFlight) return;
  _inFlight = false;

  const ShotResult r = static_cast<ShotResult>(p.a);
  _tracking[_shotAt.row][_shotAt.col] =
      (r == ShotResult::Miss) ? CellState::Miss : CellState::Hit;
  if (r != ShotResult::Miss) {
    _hitShip = static_cast<ShipType>(p.b);
    _sunkWasMine = false;  // it was one of theirs
  }
  if (r == ShotResult::Sunk) _enemySunk++;

  // Record the damage against their layout too, so the ship list on the top
  // panel fills in as their vessels go down. Harmless when the layout was
  // never sent -- an unplaced fleet has nothing to hit.
  if (r != ShotResult::Miss) _enemy.registerHit(_shotAt);

  _myTurn = false;
  if (_enemySunk >= SHIP_COUNT) {
    // The sinking shot is announced before the match is called, on both
    // sides, rather than jumping straight to the verdict.
    _won = true;
    _pendingOver = true;
  }
  // A packet arrives between ticks and carries no clock with it, so the
  // overlay is armed here and started by the next tick, which is the first
  // time a nowMs is in hand. Timing never comes from anywhere else.
  _pendingResult = static_cast<int8_t>(r);
}

void Game::showResult(ShotResult r, uint32_t nowMs) {
  _page = (r == ShotResult::Miss) ? Page::Miss
                                  : (r == ShotResult::Sunk ? Page::Sunk : Page::Hit);
  _resultUntil = nowMs + OVERLAY_MS;
  _press.reset();
}

void Game::endMatch(bool won) {
  _won = won;
  _page = Page::Over;
  // The verdict's clock starts on the next tick. Left over from an earlier
  // match it is already expired, and the banner never shows.
  _overSince = 0;
  _inFlight = false;
  _press.reset();
}

// Take the shot under the crosshair. A header position is not a cell, so it
// is not a legal shot; a cell already resolved is not one either, which is
// the rulebook's own rule that the same shot is never called twice.
void Game::fire(uint32_t nowMs) {
  if (_aim.row == HEADER || _aim.col == HEADER) return;
  if (_tracking[_aim.row][_aim.col] != CellState::Empty) return;
  if (!_myTurn || _inFlight) return;

  _shotAt = _aim;

  if (_practice) {
    // No opponent: resolve against our own fleet so the board is playable on
    // its own. The turn never passes, because there is nobody to pass it to.
    const ShotResult r = _fleet.registerHit(_aim);
    _tracking[_aim.row][_aim.col] =
        (r == ShotResult::Miss) ? CellState::Miss : CellState::Hit;
    if (r != ShotResult::Miss) {
      const int i = _fleet.shipAt(_aim);
      if (i >= 0) _hitShip = _fleet.ship(i).type;
    }
    if (r == ShotResult::Sunk) _enemySunk++;
    // Solo play shoots at its own fleet, so there is no other side to name;
    // the announcement reads as though the ships belonged to the opponent,
    // which is the fiction the whole practice path runs on.
    _sunkWasMine = false;
    if (_fleet.allSunk()) { endMatch(true); return; }
    showResult(r, nowMs);
    return;
  }

  // A real opponent owns the answer, so all that happens now is that the shot
  // goes out. The result lands whenever the backend delivers it.
  _inFlight = true;
  send(PacketType::Shot, static_cast<uint8_t>(_aim.col), static_cast<uint8_t>(_aim.row));
  _press.reset();
}

// The grid you are attacking, on the top panel: dots so the crosshair is the
// only unbroken line, your marks so far, and the fleet you are hunting.
void Game::renderTargetPanel() {
  drawPanelFrame(_top);
  drawDotGrid(_top);
  drawEnemyRoster();
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++) drawCell(_top, Coord{c, r}, _tracking[r][c]);

  // Order matters: the strip inverts first so the crosshair's lines are not
  // flipped with it, and the corner square goes on top of both.
  drawHeaderSelection(_top, _aim);
  drawCrosshair(_top, _aim);
  drawHeaderCorner(_top, _aim);
  drawAimReadout();
}

void Game::renderMatch() {
  renderTargetPanel();
  renderOwnPanel();
}

// Your own side: the fleet as shapes, the damage on it, and how much of it is
// still afloat.
void Game::drawFleetBoard(gfx::Screen &s, const Fleet &f,
                          const CellState marks[ROWS][COLS]) {
  drawPanelFrame(s);
  drawGrid(s, true);

  for (int i = 0; i < SHIP_COUNT; i++) {
    const Ship &sh = f.ship(i);
    if (!sh.placed) continue;
    drawShip(s, sh.origin, sh.length(), sh.vertical);
    // A sunk ship is struck through along its axis so it reads as gone at a
    // glance, without having to count the holes in it.
    if (sh.sunk()) drawStrike(s, sh.origin, sh.length(), sh.vertical);
  }

  // The shots that found water. Under the hulls and the damage, since it is
  // the quietest thing on the board -- but it is what tells you which squares
  // have already been spent.
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++)
      if (marks[r][c] == CellState::Miss)
        drawIncomingMiss(s, Coord{static_cast<int8_t>(c), static_cast<int8_t>(r)});

  // Damage last, so a hit stays visible against the hull it is in.
  for (int i = 0; i < SHIP_COUNT; i++) {
    const Ship &sh = f.ship(i);
    if (!sh.placed) continue;
    for (int k = 0; k < sh.length(); k++)
      if (sh.hits & (1u << k)) drawOwnHit(s, sh.cell(k));
  }
}

// Your own side: the fleet as shapes, the damage on it, and how much of it is
// still afloat. The marks are THEIR shots at us -- our own tracking grid has
// nothing to say about our water.
void Game::renderOwnPanel() {
  drawFleetBoard(_bot, _fleet, _incoming);
  drawGutterCount(_bot, GUTTER_L_LEFT, "SHIPS", "LEFT", _fleet.remaining(), SHIP_COUNT);
}

// The opposing fleet, listed in the right gutter as outlines so it never
// competes with the marks or the crosshair inside the grid. A hull fills in
// once that ship is sunk, which makes the roster the tally the rulebook asks
// for rather than just decoration.
void Game::drawEnemyRoster() {
  constexpr int ROW1_H = 5 * CELL - 1;
  constexpr int ROW2_H = 3 * CELL - 1;

  const int inner = GUTTER_R_RIGHT - GUTTER_R_LEFT + 1;
  const int row1X = GUTTER_R_LEFT + (inner - (HULL_W + PITCH)) / 2;
  const int row2X = GUTTER_R_LEFT + (inner - (HULL_W + 2 * PITCH)) / 2;
  const int y0 = (gfx::H - (ROW1_H + ROSTER_GAP + ROW2_H)) / 2;
  const int y1 = y0 + ROW1_H + ROSTER_GAP;

  // Which ships are out there comes from the opponent's own report of their
  // fleet once it has arrived. Until then -- solo play, or before the
  // exchange -- fall back on the rulebook's five, which is what any opponent
  // must have anyway.
  // Lengths come from the ship index either way, so the only thing the fleet
  // decides here is which hulls are drawn as sunk. In practice there is no
  // enemy and showing our own is the intended fiction; in a real match a
  // single dropped Fleet packet must not turn the enemy roster into a mirror
  // of our own losses, telling the player their opponent has lost ships they
  // have not.
  const bool known = enemyFleetKnown();
  const Fleet &roster = known ? _enemy : _fleet;
  const bool showSunk = known || _practice;

  struct Slot { int x, y, h, ship; };
  const Slot slots[SHIP_COUNT] = {
      {row1X, y0, ROW1_H, 0},
      {row1X + PITCH, y0, 4 * CELL - 1, 1},
      {row2X, y1, ROW2_H, 2},
      {row2X + PITCH, y1, ROW2_H, 3},
      {row2X + 2 * PITCH, y1, 2 * CELL - 1, 4},
  };
  for (const Slot &s : slots) {
    const int len = roster.ship(s.ship).length();
    const bool sunk = showSunk && roster.ship(s.ship).sunk();
    drawShipAt(_top, s.x, s.y, HULL_W, s.h, len, !sunk);
  }
}

// The aim readout, centred in the gutter left of the grid's row letters. The
// coordinate is what you read while moving, so it is set at twice the size of
// its label. An axis off the board reads X, so the starting corner is XX.
void Game::drawAimReadout() {
  constexpr int BAND = LEFT_RULE - 1;
  constexpr int NUDGE = 2;

  _top.text(NUDGE + (BAND - gfx::Screen::textWidth("AIM")) / 2, 18, "AIM");

  const char label[3] = {
      _aim.row == HEADER ? 'X' : static_cast<char>('A' + _aim.row),
      _aim.col == HEADER ? 'X' : static_cast<char>('0' + _aim.col), '\0'};
  _top.textScaled(NUDGE + (BAND - gfx::Screen::textScaledWidth(label, 2)) / 2, 30, label, 2);
  drawTurnReadout();
}

// Whose turn it is, under the aim readout in the same gutter. Two tiny words
// rather than one, because the gutter is 33px wide and a word per line is
// what fits at this size.
void Game::drawTurnReadout() {
  constexpr int BAND = LEFT_RULE - 1;
  constexpr int NUDGE = 2;
  const char *l1 = _practice ? "SOLO" : (_myTurn ? "YOUR" : "THEIR");
  const char *l2 = _practice ? "PLAY" : "TURN";
  _top.textTiny(NUDGE + (BAND - gfx::Screen::textTinyWidth(l1)) / 2, 46, l1);
  _top.textTiny(NUDGE + (BAND - gfx::Screen::textTinyWidth(l2)) / 2, 54, l2);
}

// The match is decided. The tracking grid stays up top -- the board you won
// or lost on is the thing worth looking at -- and the bottom panel says which
// it was, in the same frame the shot results use.
void Game::renderOver(uint32_t nowMs) {
  // Their board, revealed. The match is over, so the one thing that was worth
  // keeping secret no longer is -- and where their ships actually were is the
  // first thing either player wants to know. Drawn in the same language as
  // your own board (hulls with the gaps between them, a strike through what
  // sank, dots where a shot found water) rather than as the dotted tracking
  // grid, because it is a fleet now and not a guess.
  drawFleetBoard(_top, _enemy, _tracking);
  drawGutterCount(_top, GUTTER_L_LEFT, "THEIR", "FLEET", _enemy.remaining(), SHIP_COUNT);

  // The bottom panel says who won, and then becomes your own board, so the
  // two layouts sit one above the other and can be compared. The banner has
  // its moment first because a board is not an answer: WIN or LOSE is.
  const bool banner = _overSince == 0 || nowMs - _overSince < VERDICT_MS;
  if (banner) {
    constexpr int MARGIN = 2;
    constexpr int BORDER = LABEL_H - 1;
    const int fw = gfx::W - 2 * MARGIN;
    const int fh = gfx::H - 2 * MARGIN;
    _bot.fillRect(MARGIN, MARGIN, fw, fh, true);
    _bot.fillRect(MARGIN + BORDER, MARGIN + BORDER, fw - 2 * BORDER, fh - 2 * BORDER, false);

    const char *word = _won ? "WIN" : "LOSE";
    constexpr int SCALE = 3;
    _bot.textBubble(gfx::centerScaledX(word, SCALE), 15, word, SCALE);

    // Whose win it is, by name. "WIN" says the same thing on both panels; the
    // name is what makes the verdict a fact about the match rather than about
    // the screen it is drawn on.
    const char *winner = _won ? _myName : _theirName;
    char line[LOBBY_NAME_CHARS + 6];
    if (winner[0] != '\0') {
      int n = 0;
      for (const char *c = winner; *c && n < LOBBY_NAME_CHARS; c++) line[n++] = *c;
      for (const char *c = " WINS"; *c; c++) line[n++] = *c;
      line[n] = '\0';
    } else {
      const char *fallback = _won ? "FLEET SUNK" : "ALL HANDS LOST";
      int n = 0;
      for (const char *c = fallback; *c; c++) line[n++] = *c;
      line[n] = '\0';
    }
    _bot.text(gfx::centerTextX(line), 45, line);
    return;
  }

  // Both fleets, one panel each. No timer on this: somebody working out where
  // the ships were is not on the clock, so it stays until they press.
  drawFleetBoard(_bot, _fleet, _incoming);
  drawGutterCount(_bot, GUTTER_L_LEFT, "YOUR", "FLEET", _fleet.remaining(), SHIP_COUNT);
  _bot.textTiny(gfx::centerTinyIn("PRESS", GUTTER_R_LEFT, GUTTER_R_RIGHT), 24, "PRESS");
  _bot.textTiny(gfx::centerTinyIn("TO", GUTTER_R_LEFT, GUTTER_R_RIGHT), 32, "TO");
  _bot.textTiny(gfx::centerTinyIn("LEAVE", GUTTER_R_LEFT, GUTTER_R_RIGHT), 40, "LEAVE");
}

}  // namespace game
