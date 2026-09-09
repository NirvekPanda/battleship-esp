#include "game.h"

// The "place your ships" page.
//
// Focus is implicit and needs no control of its own: with no ship in hand the
// picker on the top panel has it, and with one in hand the grid does. That is
// what lets four directions and one button drive the whole page.
namespace game {

namespace {
// The picker: five hulls in a row across the bottom half of the top panel,
// with the selected ship's name above them.
constexpr int HULL_H = CELL - 1;      // 4, one cell deep
constexpr int PICK_GAP = 8;
constexpr int PICK_Y = 44;            // bottom half, clear of the name
constexpr int NAME_Y = 12;
constexpr int UNDERLINE_Y = PICK_Y + HULL_H + 3;

int hullW(int len) { return len * CELL - 1; }

// Total width of the row, so it can be centred as one block.
int rowWidth(const Fleet &f) {
  int w = (SHIP_COUNT - 1) * PICK_GAP;
  for (int i = 0; i < SHIP_COUNT; i++) w += hullW(f.ship(i).length());
  return w;
}

int hullX(const Fleet &f, int index) {
  int x = (gfx::W - rowWidth(f)) / 2;
  for (int i = 0; i < index; i++) x += hullW(f.ship(i).length()) + PICK_GAP;
  return x;
}

// The next ship still to place, wrapping, so putting one down moves the
// picker to something useful rather than leaving it on a finished ship.
// Keep the whole ship on the board, not just its origin.
//
// The origin is the bow, and the rest of the hull runs right or down from it,
// so the last legal origin depends on both length and orientation: a
// five-cell ship laid across the board cannot start further right than column
// 5. Clamping the origin alone -- to the last column, as a bare cursor would
// -- lets four cells of carrier hang past the right wall, where they are not
// merely illegal but are drawn over the rule and the gutter beside the grid.
void clampOrigin(Coord &c, int len, bool vertical) {
  const int maxCol = vertical ? COLS - 1 : COLS - len;
  const int maxRow = vertical ? ROWS - len : ROWS - 1;
  if (c.col > maxCol) c.col = maxCol;
  if (c.row > maxRow) c.row = maxRow;
  if (c.col < 0) c.col = 0;
  if (c.row < 0) c.row = 0;
}

int nextUnplaced(const Fleet &f, int from) {
  for (int k = 1; k <= SHIP_COUNT; k++) {
    const int i = (from + k) % SHIP_COUNT;
    if (!f.ship(i).placed) return i;
  }
  return from;
}
}  // namespace

void Game::updatePlace(const Input &in, uint32_t nowMs) {
  const PressDetector::Event ev = _press.update(in.center, nowMs);

  if (_active < 0) {
    // ---- picker has focus ----
    if ((in.left || in.right) && nowMs - _lastStep >= STEP_MS) {
      _lastStep = nowMs;
      _hover = (_hover + (in.right ? 1 : SHIP_COUNT - 1)) % SHIP_COUNT;
    }

    // The picker takes a ship on the press itself: choosing one is not a
    // gesture, and the double window belongs to the grid where the button has
    // two meanings.
    if (_press.edge()) {
      // Taking a placed ship back off the board is how it gets moved: it
      // stops occupying its cells, so it cannot collide with itself, and the
      // cursor picks up where that ship already was.
      _active = _hover;
      const Ship &s = _fleet.ship(_active);
      if (s.placed) {
        _place = s.origin;
        _vertical = s.vertical;
        _fleet.pickUp(_active);
      } else {
        _place = Coord{0, 0};
        _vertical = true;
      }
      clampOrigin(_place, _fleet.ship(_active).length(), _vertical);
      _press.reset();  // this press must not start a double on the grid
    }
    return;
  }

  // ---- a ship is in hand, the grid has focus ----
  const int len = _fleet.ship(_active).length();

  stepCursor(in, nowMs, _place, 0);

  // Centre alone turns the ship; centre twice quickly puts it down. The
  // double is the gesture that is recognised the moment it happens, so it
  // goes to the action that cannot be undone -- a turn that arrives a window
  // late is simply turned again. The directions do nothing but move the
  // cursor.
  if (ev == PressDetector::Event::Single) {
    _vertical = !_vertical;
  }

  // After moving or turning, pull the ship back onto the board. Turning is
  // what makes this necessary as well as moving: a ship lying legally along
  // the last row is off the bottom the moment it stands up.
  clampOrigin(_place, len, _vertical);

  if (ev == PressDetector::Event::Double) {
    if (_fleet.place(_active, _place, _vertical)) {
      _hover = nextUnplaced(_fleet, _active);
      _active = -1;
      _press.reset();
    }
    // A refusal can now only be an overlap: the clamp above means the ship is
    // always wholly on the board. The cross in the right gutter has been
    // saying so the whole time the cursor sat there, so it is never a
    // surprise.
  }

  // Every ship down and nothing in hand: the fleet is set.
  if (_active < 0 && _fleet.allPlaced()) {
    // Whether there is anyone to wait for is decided here, by the link. A
    // board with no link has no opponent who could ever arrive, so it goes
    // straight on and plays solo rather than blocking on a player who does
    // not exist.
    _practice = !_everLinked;  // solo only if a link was NEVER seen
    if (_practice) {
      _myTurn = true;
      _page = Page::Starting;
      _waveUntil = 0;
    } else {
      // Hand over the whole layout, one ship per packet, then say the fleet
      // is down. Order matters: Ready is the last thing sent, so a peer that
      // sees Ready knows every Fleet packet is already behind it.
      //
      // They need the layout to know which vessels they are hunting -- the
      // ship list on their top panel is drawn from it. Their POSITIONS are
      // never drawn on the tracking grid; finding those is the game.
      for (int i = 0; i < SHIP_COUNT; i++) {
        const Ship &s = _fleet.ship(i);
        send(PacketType::Fleet, static_cast<uint8_t>(i),
             packPlacement(s.origin.col, s.origin.row, s.vertical));
      }
      send(PacketType::Ready, 0, 0);
      _page = Page::Waiting;
    }
    _press.reset();
  }
}

void Game::renderPlace() {
  // ---- top: the picker ----
  // A hull is outlined until its ship is spoken for and filled once it is,
  // so the row doubles as the progress indicator: five filled hulls means a
  // fleet ready to fight.
  for (int i = 0; i < SHIP_COUNT; i++) {
    const Ship &s = _fleet.ship(i);
    const bool solid = s.placed || i == _active;
    drawShipAt(_top, hullX(_fleet, i), PICK_Y, hullW(s.length()), HULL_H,
               s.length(), !solid);
  }

  // Which ship the name belongs to: the one in hand, or the one hovered.
  const int named = _active >= 0 ? _active : _hover;
  const char *name = shipName(_fleet.ship(named).type);
  const int scale = gfx::Screen::textScaledWidth(name, 2) <= gfx::W - 4 ? 2 : 1;
  _top.textScaled((gfx::W - gfx::Screen::textScaledWidth(name, scale)) / 2, NAME_Y,
                  name, scale);

  // The picker's own cursor: a rule under the hull the name refers to.
  const int ux = hullX(_fleet, named);
  _top.hLine(ux, UNDERLINE_Y, hullW(_fleet.ship(named).length()), true);

  // ---- bottom: the board ----
  drawPanelFrame(_bot);
  drawDotGrid(_bot);
  for (int i = 0; i < SHIP_COUNT; i++) {
    const Ship &s = _fleet.ship(i);
    if (s.placed) drawShip(_bot, s.origin, s.length(), s.vertical);
  }

  drawGutterCount(_bot, GUTTER_L_LEFT, "PLACED", "SHIPS", _fleet.placedCount(), SHIP_COUNT);

  if (_active >= 0) {
    const Ship &s = _fleet.ship(_active);
    const bool legal = _fleet.legalAt(_active, _place, _vertical);

    // The ship under the cursor is drawn where it would land: solid when it
    // would be legal there, an outline when it would not, so the board itself
    // says what the gutter mark says.
    if (legal) {
      drawShip(_bot, _place, s.length(), _vertical);
    } else {
      const int x = cellX(_place.col) + 1, y = cellY(_place.row) + 1;
      const int w = _vertical ? CELL - 1 : s.length() * CELL - 1;
      const int h = _vertical ? s.length() * CELL - 1 : CELL - 1;
      drawShipAt(_bot, x, y, w, h, s.length(), true);
    }

    // Live legality, updated on every cursor step rather than only when the
    // place is attempted.
    constexpr int MARK = 13;
    const int mx = GUTTER_R_LEFT + (GUTTER_R_RIGHT - GUTTER_R_LEFT + 1 - MARK) / 2;
    if (legal) gfx::drawTick(_bot, mx, 25, MARK, true);
    else       gfx::drawCross(_bot, mx, 25, MARK, true);
  }
}

}  // namespace game
