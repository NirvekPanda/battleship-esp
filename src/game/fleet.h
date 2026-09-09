#pragma once
#include <stdint.h>

#include "grid.h"

// The fleet and the rules over it: what may be placed where, what a shot
// hits, and when a ship is sunk. Deliberately separate from any drawing, so
// there is exactly one authority for legality and sinking and no page can
// disagree with another about them.
namespace game {

// The rulebook's five ships. Named because a hit report has to say which ship
// was struck ("Hit. Cruiser."), per Battleship_rules.pdf.
enum class ShipType : uint8_t { Carrier, Battleship, Cruiser, Submarine, Destroyer };
constexpr int SHIP_COUNT = 5;
constexpr int MAX_SHIP_LEN = 5;
constexpr int FLEET_CELLS = 17;  // 5 + 4 + 3 + 3 + 2

const char *shipName(ShipType t);
int shipLength(ShipType t);

// What a shot did. Sunk implies Hit: it is the last hit on a ship.
enum class ShotResult : uint8_t { Miss, Hit, Sunk };

// One vessel. `placed` distinguishes a ship sitting on the board from one
// still in hand -- picking a placed ship back up clears it, which is what lets
// a ship be moved without colliding with the copy of itself it just left.
struct Ship {
  ShipType type;
  Coord origin;
  bool vertical;
  bool placed;
  uint8_t hits;  // bitmask, bit i = cell i from the origin

  int length() const { return shipLength(type); }
  Coord cell(int i) const;
  bool occupies(Coord c) const;
  bool sunk() const;
};

class Fleet {
public:
  // All five ships, unplaced and undamaged, in rulebook order.
  void reset();

  Ship &ship(int i) { return _ships[i]; }
  const Ship &ship(int i) const { return _ships[i]; }

  // Would this ship sit legally at this origin? Bounds and overlap only --
  // the rulebook allows nothing else to make a placement illegal. The ship
  // being asked about is excluded from the overlap test, so re-placing a ship
  // does not collide with where it currently is.
  bool legalAt(int index, Coord origin, bool vertical) const;

  // Commit a placement. Returns false and changes nothing if it is not legal,
  // so a caller cannot place a ship somewhere the rules forbid.
  bool place(int index, Coord origin, bool vertical);

  // Take a placed ship back off the board so it can be moved.
  void pickUp(int index);

  // Which placed ship covers this cell, or -1.
  int shipAt(Coord c) const;

  // Resolve a shot. Only ever called on a cell not already shot at; the
  // caller owns that check, since it is the tracking grid that remembers.
  ShotResult registerHit(Coord c);

  // Mark a ship of this type as sunk outright, without shooting at it cell by
  // cell. Only the relay does this, handing a returning player back a board
  // whose sinkings are already facts -- the roster is drawn from them, and
  // rebuilding them from shots would need the enemy's positions, which a
  // player is not given.
  void sinkByType(ShipType t);

  int placedCount() const;
  bool allPlaced() const { return placedCount() == SHIP_COUNT; }
  int sunkCount() const;
  int remaining() const { return SHIP_COUNT - sunkCount(); }
  bool allSunk() const { return sunkCount() == SHIP_COUNT; }

private:
  Ship _ships[SHIP_COUNT];
};

}  // namespace game
