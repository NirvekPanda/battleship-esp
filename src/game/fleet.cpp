#include "fleet.h"

namespace game {

const char *shipName(ShipType t) {
  switch (t) {
    case ShipType::Carrier:    return "CARRIER";
    case ShipType::Battleship: return "BATTLESHIP";
    case ShipType::Cruiser:    return "CRUISER";
    case ShipType::Submarine:  return "SUBMARINE";
    case ShipType::Destroyer:  return "DESTROYER";
  }
  return "";
}

int shipLength(ShipType t) {
  switch (t) {
    case ShipType::Carrier:    return 5;
    case ShipType::Battleship: return 4;
    case ShipType::Cruiser:    return 3;
    case ShipType::Submarine:  return 3;
    case ShipType::Destroyer:  return 2;
  }
  return 0;
}

// Cell i counted from the origin, along whichever axis the ship lies on.
Coord Ship::cell(int i) const {
  return vertical ? Coord{origin.col, origin.row + i}
                  : Coord{origin.col + i, origin.row};
}

bool Ship::occupies(Coord c) const {
  if (!placed) return false;
  for (int i = 0; i < length(); i++) {
    const Coord k = cell(i);
    if (k.col == c.col && k.row == c.row) return true;
  }
  return false;
}

// Every cell hit. The mask has one bit per cell, so this is a full house of
// `length` bits rather than a count -- a cell cannot be hit twice, but the
// mask makes that structurally true instead of merely assumed.
bool Ship::sunk() const {
  if (!placed) return false;
  const uint8_t all = (uint8_t)((1u << length()) - 1u);
  return (hits & all) == all;
}

void Fleet::reset() {
  const ShipType order[SHIP_COUNT] = {ShipType::Carrier, ShipType::Battleship,
                                      ShipType::Cruiser, ShipType::Submarine,
                                      ShipType::Destroyer};
  for (int i = 0; i < SHIP_COUNT; i++)
    _ships[i] = Ship{order[i], Coord{0, 0}, true, false, 0};
}

bool Fleet::legalAt(int index, Coord origin, bool vertical) const {
  if (index < 0 || index >= SHIP_COUNT) return false;
  const int len = _ships[index].length();

  // Wholly on the board. The far end is what fails first, so it is checked
  // against the board's own bounds rather than a precomputed limit.
  if (origin.col < 0 || origin.row < 0) return false;
  const int lastCol = vertical ? origin.col : origin.col + len - 1;
  const int lastRow = vertical ? origin.row + len - 1 : origin.row;
  if (lastCol >= COLS || lastRow >= ROWS) return false;

  // No overlap with any OTHER placed ship. Skipping index is what makes
  // moving a ship work: it is still on the board while being re-placed, and
  // must not be found to collide with itself.
  for (int i = 0; i < len; i++) {
    const Coord c = vertical ? Coord{origin.col, origin.row + i}
                             : Coord{origin.col + i, origin.row};
    for (int s = 0; s < SHIP_COUNT; s++) {
      if (s == index) continue;
      if (_ships[s].occupies(c)) return false;
    }
  }
  return true;
}

bool Fleet::place(int index, Coord origin, bool vertical) {
  if (!legalAt(index, origin, vertical)) return false;
  _ships[index].origin = origin;
  _ships[index].vertical = vertical;
  _ships[index].placed = true;
  return true;
}

void Fleet::pickUp(int index) {
  if (index < 0 || index >= SHIP_COUNT) return;
  _ships[index].placed = false;
}

int Fleet::shipAt(Coord c) const {
  for (int i = 0; i < SHIP_COUNT; i++)
    if (_ships[i].occupies(c)) return i;
  return -1;
}

ShotResult Fleet::registerHit(Coord c) {
  const int i = shipAt(c);
  if (i < 0) return ShotResult::Miss;

  Ship &s = _ships[i];
  for (int k = 0; k < s.length(); k++) {
    const Coord cell = s.cell(k);
    if (cell.col == c.col && cell.row == c.row) {
      s.hits |= (uint8_t)(1u << k);
      break;
    }
  }
  return s.sunk() ? ShotResult::Sunk : ShotResult::Hit;
}

// Every cell struck at once. The ship's length is what says how many bits
// that is, so this cannot mark a ship as more sunk than it is.
void Fleet::sinkByType(ShipType t) {
  for (int i = 0; i < SHIP_COUNT; i++) {
    if (_ships[i].type != t) continue;
    const int n = _ships[i].length();
    _ships[i].hits = static_cast<uint8_t>((1u << n) - 1);
  }
}

int Fleet::placedCount() const {
  int n = 0;
  for (int i = 0; i < SHIP_COUNT; i++) if (_ships[i].placed) n++;
  return n;
}

int Fleet::sunkCount() const {
  int n = 0;
  for (int i = 0; i < SHIP_COUNT; i++) if (_ships[i].sunk()) n++;
  return n;
}

}  // namespace game
