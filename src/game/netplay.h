#pragma once
#include <stdint.h>

#include "fleet.h"

// The match protocol, as the portable core sees it: seven fields and nothing
// else. server/PROTOCOL.md is the document of record; this header is its C++
// face and deliberately knows nothing about how a packet travels. There are
// no sockets here, no JSON and no ESP-NOW -- a backend hands inbound packets
// to Game::receive() and drains outbound ones with Game::takeOutbound(), and
// whether that is HTTP long polling, a radio frame or a test's array is none
// of the core's business.
namespace game {

// Peer ids. Small integers rather than names because the same packet has to
// fit an ESP-NOW frame. A packet may be addressed to BROADCAST but may never
// claim to come from it.
constexpr uint8_t PEER_SERVER = 0;
constexpr uint8_t PEER_A = 1;
constexpr uint8_t PEER_B = 2;
constexpr uint8_t PEER_BROADCAST = 255;

constexpr uint8_t PROTOCOL_VERSION = 1;

enum class PacketType : uint8_t {
  Join = 1,
  Ready = 2,
  Shot = 3,
  Result = 4,
  Reset = 5,
  Bye = 6,
  // One ship of the sender's fleet, sent five times to convey the whole
  // layout. Five small packets rather than one big one because the frame is a
  // fixed eight bytes -- that is what lets an ESP-NOW payload be read with no
  // length prefix and no allocation -- and a five-ship layout does not fit in
  // the two argument bytes. The size of the frame is worth more than the
  // tidiness of a single message.
  Fleet = 7,
  // The other half of the handshake. Join says "I am here"; Ack says "and I
  // heard you". Both are needed before a match can start: a Join that was
  // sent proves nothing, since it may have been dropped or the relay may have
  // had nobody to give it to. Only an Ack proves the round trip closed.
  Ack = 8,
  // ---- putting a returning player back on their board ----
  //
  // A Result means "the shot you just fired was a hit", which only means
  // anything to a board that remembers firing it. A player who has restarted
  // remembers nothing, so the relay hands the board back as marks that name
  // their own cell instead of as the history that produced them.
  //
  //   Mark    a = cell (col + row*10), b = result | ship<<2  -- tracking grid
  //   Damage  a = cell, b = 1 for a hit, 0 for a miss       -- their shot
  //   Turn    a = 1 when the move is ours
  //
  // From the SERVER only, like Page: a player who could mark another player's
  // grid could tell them their shot hit when it did not.
  Mark = 12,
  Damage = 13,
  Turn = 14,
  // From the SERVER, not a player: put your screen on this page. The
  // dashboard uses it to march both boards to the same place at once.
  Page = 9,
  // The server restoring a returning player's OWN fleet. Distinct from Fleet,
  // which is the opponent telling you about theirs: the two go to different
  // boards, and one packet type that meant either would need a flag to say
  // which -- a flag that could be wrong.
  MyFleet = 10,
  // The match is on: a is your seat, b is your team. Sent by the server to
  // both players once they have chosen each other.
  Start = 11,
};

// A placement packed into one byte: the cell as col + row * 10 (0..99), then
// doubled with the orientation in the low bit. The largest value is 199, so a
// whole ship's position and heading fit in the packet's single spare byte.
inline uint8_t packPlacement(int col, int row, bool vertical) {
  return static_cast<uint8_t>((col + row * COLS) * 2 + (vertical ? 1 : 0));
}
inline int placementCol(uint8_t b) { return (b / 2) % COLS; }
inline int placementRow(uint8_t b) { return (b / 2) / COLS; }
inline bool placementVertical(uint8_t b) { return (b & 1) != 0; }

// The packet. A plain aggregate with no default member initializers, because
// the ESP toolchain compiles this as C++11 where those make brace
// initialization ill-formed -- the same reason Coord is written that way.
struct NetPacket {
  uint8_t v;
  uint16_t seq;
  uint8_t src;
  uint8_t dst;
  uint8_t t;  // PacketType
  uint8_t a;
  uint8_t b;

  PacketType type() const { return static_cast<PacketType>(t); }
};

// The protocol's result numbering IS ShotResult's and its ship index IS
// ShipType's, so both ends cast between them rather than translating.
// PROTOCOL.md says changing either enum changes the protocol; these assert
// it in the one place a change would otherwise slip through silently.
static_assert(static_cast<uint8_t>(ShotResult::Miss) == 0, "protocol: miss is 0");
static_assert(static_cast<uint8_t>(ShotResult::Hit) == 1, "protocol: hit is 1");
static_assert(static_cast<uint8_t>(ShotResult::Sunk) == 2, "protocol: sunk is 2");
static_assert(static_cast<uint8_t>(ShipType::Carrier) == 0, "protocol: ship 0 is the carrier");
static_assert(static_cast<uint8_t>(ShipType::Destroyer) == 4, "protocol: ship 4 is the destroyer");
static_assert(SHIP_COUNT == 5, "protocol: five ships, indices 0..4");

// The same validation the relay applies. A transport with no relay in the
// middle (ESP-NOW is the whole point of the binary encoding) has nothing to
// defend the core for it, so the core refuses exactly what the relay would.
inline bool packetValid(const NetPacket &p) {
  if (p.v != PROTOCOL_VERSION) return false;
  if (p.src == PEER_BROADCAST) return false;
  if (p.t < 1 || p.t > 14) return false;
  // The three that hand a returning player their board back. Checked here for
  // the same reason as everything else: ESP-NOW has no relay in the middle,
  // so the core cannot rely on one having looked.
  if (p.type() == PacketType::Mark) {
    if (p.a >= COLS * ROWS) return false;
    if ((p.b & 3) > static_cast<uint8_t>(ShotResult::Sunk)) return false;
    return (p.b >> 2) < SHIP_COUNT;
  }
  if (p.type() == PacketType::Damage) return p.a < COLS * ROWS && p.b <= 1;
  if (p.type() == PacketType::Turn) return p.a <= 1;
  if (p.type() == PacketType::Shot) return p.a < COLS && p.b < ROWS;
  if (p.type() == PacketType::Result) {
    if (p.a > static_cast<uint8_t>(ShotResult::Sunk)) return false;
    if (p.b >= SHIP_COUNT) return false;
    // A miss names no ship.
    return !(p.a == static_cast<uint8_t>(ShotResult::Miss) && p.b != 0);
  }
  if (p.type() == PacketType::MyFleet || p.type() == PacketType::Fleet) {
    // a is which ship, b is where it sits. The cell must be a real one, and
    // the ship must fit on the board from there -- the same rule Fleet
    // applies locally, so a bad layout cannot arrive over the wire either.
    if (p.a >= SHIP_COUNT) return false;
    if (p.b > (COLS * ROWS - 1) * 2 + 1) return false;
    const int len = shipLength(static_cast<ShipType>(p.a));
    const int col = placementCol(p.b), row = placementRow(p.b);
    if (placementVertical(p.b)) return row + len <= ROWS;
    return col + len <= COLS;
  }
  return true;
}

// How many packets the core will hold for a backend that has not drained the
// outbox yet. Big enough for the largest burst the core ever produces: the
// whole fleet handed over at the end of placement, one packet per ship, plus
// the Ready that follows it, plus room to spare. Sized against SHIP_COUNT
// rather than guessed, because send() drops silently when the outbox is full
// and a dropped Ready would leave both players waiting for each other for
// ever -- a deadlock with no error anywhere to explain it.
constexpr int OUTBOX_MAX = SHIP_COUNT + 4;

// ---- the 8-byte wire form ----
//
// The layout of record, matching server/PROTOCOL.md and Go's MarshalWire:
//
//     0    1    2     3     4       5       6    7
//     v    t    src   dst   seq lo  seq hi  a    b
//
// Written once, here, because every place that laid these bytes out by hand
// was a place that could get the order wrong -- and one of them did, sending
// the sequence number where the type belonged, which the relay could only
// report as "unknown type: 0". Seq is little-endian, as on the C3.
constexpr int WIRE_SIZE = 8;

inline void packWire(const NetPacket &p, uint8_t out[WIRE_SIZE]) {
  out[0] = p.v;
  out[1] = p.t;
  out[2] = p.src;
  out[3] = p.dst;
  out[4] = static_cast<uint8_t>(p.seq & 0xFF);
  out[5] = static_cast<uint8_t>(p.seq >> 8);
  out[6] = p.a;
  out[7] = p.b;
}

inline NetPacket unpackWire(const uint8_t in[WIRE_SIZE]) {
  NetPacket p{};
  p.v = in[0];
  p.t = in[1];
  p.src = in[2];
  p.dst = in[3];
  p.seq = static_cast<uint16_t>(in[4] | (in[5] << 8));
  p.a = in[6];
  p.b = in[7];
  return p;
}

}  // namespace game
