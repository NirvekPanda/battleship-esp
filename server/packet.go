package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"strings"
)

// Protocol version. Bump it when the meaning of an existing field changes;
// adding a new optional field does not need a bump, since both the JSON and
// the binary form carry fixed slots that older peers simply ignore.
const Version = 1

// PeerID is who a packet is from or for. Kept to a small integer rather than
// a string because the same packet has to fit in an 8-byte ESP-NOW frame,
// where there is no room for names.
type PeerID uint8

const (
	PeerServer PeerID = 0
	PeerA      PeerID = 1 // first player to join
	PeerB      PeerID = 2 // second player to join
	PeerAll    PeerID = 255
)

// Type is what a packet means. The set is deliberately small: these six cover
// a whole match, and anything not on this list is rejected rather than
// forwarded, so an unknown packet can never reach the game core.
type Type uint8

const (
	TJoin   Type = 1 // I am here, give me a peer id
	TReady  Type = 2 // my fleet is placed
	TShot   Type = 3 // I fire at A.col, A.row
	TResult Type = 4 // your shot at A.col, A.row was B (and hit ship B2)
	TReset  Type = 5 // abandon this match and start over
	TBye    Type = 6 // I am leaving
	// One ship of the sender's layout, sent five times to convey the whole
	// fleet. Five small packets rather than one big one because the frame is
	// a fixed eight bytes, which is what lets an ESP-NOW payload be read with
	// no length prefix and no allocation.
	TFleet Type = 7
	// The other half of the handshake: Join says "I am here", Ack says "and I
	// heard you". A Join that was sent proves nothing; only an Ack proves the
	// round trip closed.
	TAck Type = 8
	// Sent by the SERVER, not by a player: put your screen on this page. The
	// dashboard uses it to march both boards to the same place at once, which
	// is the only way to look at one screen on two devices that are otherwise
	// driven independently.
	TPage Type = 9
	// The server restoring a returning player's OWN fleet -- distinct from
	// TFleet, which is the opponent describing theirs. They go to different
	// boards, and one type meaning either would need a flag that could be
	// wrong.
	TMyFleet Type = 10
	// The match is on: a is your seat, b is your team.
	TStart Type = 11
	// ---- putting a returning player back on their board ----
	//
	// A result means "the shot you just fired was a hit", which only makes
	// sense to a client that remembers firing it. A player who has restarted
	// remembers nothing, so the board is handed to them as marks rather than
	// as the history that produced them: each one names its own cell.
	//
	// TMark  a = cell (col + row*10), b = result | ship<<2 -- your tracking grid
	// TDamage a = cell, b = 1 when it hit -- a shot of theirs at YOUR water
	// TTurn  a = 1 when it is your move
	TMark   Type = 12
	TDamage Type = 13
	TTurn   Type = 14
)

var typeNames = map[Type]string{
	TJoin: "join", TReady: "ready", TShot: "shot",
	TResult: "result", TReset: "reset", TBye: "bye", TFleet: "fleet", TAck: "ack", TPage: "page", TMyFleet: "myfleet", TStart: "start",
	TMark: "mark", TDamage: "damage", TTurn: "turn",
}

func (t Type) String() string {
	if n, ok := typeNames[t]; ok {
		return n
	}
	return fmt.Sprintf("type(%d)", uint8(t))
}

// Shot results, matching game::ShotResult in src/game/fleet.h exactly. The
// numbering is part of the protocol: the firmware casts between them.
const (
	ResMiss uint8 = 0
	ResHit  uint8 = 1
	ResSunk uint8 = 2
)

// Packet is the whole protocol. Every message is this one shape -- four
// routing fields and two bytes of argument -- so both ends have exactly one
// struct to parse and there is no per-type decoding.
//
// The two argument bytes are typed by Type:
//
//	shot    A = column 0..9,  B = row 0..9
//	result  A = ResMiss/ResHit/ResSunk, B = ship index 0..4 (0 when a miss)
//	join    A = protocol features (0 today), B = unused
//	ready   A, B unused
//	reset   A, B unused
//	bye     A, B unused
//
// Two argument bytes is not a guess at what might be needed: it is what the
// game actually says. A shot is a coordinate and a result is an outcome plus
// a ship, and nothing in the rulebook needs a third.
type Packet struct {
	V   uint8  `json:"v"`
	Seq uint16 `json:"seq"`
	Src PeerID `json:"src"`
	Dst PeerID `json:"dst"`
	T   Type   `json:"t"`
	A   uint8  `json:"a"`
	B   uint8  `json:"b"`
}

// WireSize is the fixed length of the binary form. Fixed, not variable, so an
// ESP-NOW frame needs no length prefix and no allocation to read.
const WireSize = 8

var (
	ErrVersion = errors.New("packet: wrong protocol version")
	ErrType    = errors.New("packet: unknown type")
	ErrArg     = errors.New("packet: argument out of range for type")
	ErrSrc     = errors.New("packet: a packet cannot come from the broadcast id")
	ErrShort   = errors.New("packet: wire form must be exactly 8 bytes")
)

// Validate rejects anything the game could not act on. Doing it here means
// neither the browser nor the firmware has to defend itself against a
// malformed peer: the relay refuses to forward a packet it cannot explain.
func (p Packet) Validate() error {
	if p.V != Version {
		return fmt.Errorf("%w: got %d, want %d", ErrVersion, p.V, Version)
	}
	if _, ok := typeNames[p.T]; !ok {
		return fmt.Errorf("%w: %d", ErrType, uint8(p.T))
	}
	if p.Src == PeerAll {
		return ErrSrc
	}
	switch p.T {
	case TShot:
		// The board is the rulebook's 10x10, same as game::COLS/ROWS.
		if p.A > 9 || p.B > 9 {
			return fmt.Errorf("%w: shot at %d,%d is off the board", ErrArg, p.A, p.B)
		}
	case TPage:
		if int(p.A) >= len(pageNames) {
			return fmt.Errorf("%w: page %d does not exist", ErrArg, p.A)
		}
	case TMark:
		// A cell is a single number here rather than a column and a row,
		// because the second argument byte is spent on the answer.
		if p.A >= BoardSize*BoardSize {
			return fmt.Errorf("%w: cell %d is off the board", ErrArg, p.A)
		}
		if p.B&3 > ResSunk {
			return fmt.Errorf("%w: result %d is not miss/hit/sunk", ErrArg, p.B&3)
		}
		if p.B>>2 > 4 {
			return fmt.Errorf("%w: ship index %d is not one of the five", ErrArg, p.B>>2)
		}
	case TDamage:
		if p.A >= BoardSize*BoardSize {
			return fmt.Errorf("%w: cell %d is off the board", ErrArg, p.A)
		}
		if p.B > 1 {
			return fmt.Errorf("%w: damage %d is not hit or miss", ErrArg, p.B)
		}
	case TTurn:
		if p.A > 1 {
			return fmt.Errorf("%w: turn %d is not yes or no", ErrArg, p.A)
		}
	case TStart:
		if p.A > 1 {
			return fmt.Errorf("%w: seat %d is not a or b", ErrArg, p.A)
		}
		if p.B > 1 {
			return fmt.Errorf("%w: team %d is not red or black", ErrArg, p.B)
		}
	// A fleet is a fleet whoever is describing it, so both types get the same
	// check. Listing them together says that outright, where a fallthrough
	// only implies it -- and an earlier attempt at this fell through into the
	// wrong case and left MyFleet with no validation at all.
	case TFleet, TMyFleet:
		// A is which ship; B packs the cell as col + row*10, doubled with the
		// orientation in the low bit. The ship must also fit on the board
		// from there -- the same rule the game applies locally.
		if p.A >= uint8(len(shipNames)) {
			return fmt.Errorf("%w: ship index %d is not one of the five", ErrArg, p.A)
		}
		if int(p.B) > (BoardCells-1)*2+1 {
			return fmt.Errorf("%w: placement %d is off the board", ErrArg, p.B)
		}
		col, row, vertical := UnpackPlacement(p.B)
		n := shipLens[p.A]
		if (vertical && row+n > BoardSize) || (!vertical && col+n > BoardSize) {
			return fmt.Errorf("%w: a %d-cell ship does not fit at %s", ErrArg, n,
				CellName(uint8(col), uint8(row)))
		}
	case TResult:
		if p.A > ResSunk {
			return fmt.Errorf("%w: result %d is not miss/hit/sunk", ErrArg, p.A)
		}
		if p.B > 4 {
			return fmt.Errorf("%w: ship index %d is not one of the five", ErrArg, p.B)
		}
		if p.A == ResMiss && p.B != 0 {
			return fmt.Errorf("%w: a miss names no ship", ErrArg)
		}
	}
	return nil
}

// MarshalWire is the binary form used off the wire where JSON is too heavy --
// principally ESP-NOW, whose payload this is sized for. Field order matches
// the struct so the two forms can be read against each other by eye.
func (p Packet) MarshalWire() [WireSize]byte {
	return [WireSize]byte{
		p.V,
		uint8(p.T),
		uint8(p.Src),
		uint8(p.Dst),
		uint8(p.Seq), uint8(p.Seq >> 8), // little-endian, as on the C3
		p.A,
		p.B,
	}
}

func UnmarshalWire(b []byte) (Packet, error) {
	if len(b) != WireSize {
		return Packet{}, fmt.Errorf("%w: got %d", ErrShort, len(b))
	}
	p := Packet{
		V:   b[0],
		T:   Type(b[1]),
		Src: PeerID(b[2]),
		Dst: PeerID(b[3]),
		Seq: uint16(b[4]) | uint16(b[5])<<8,
		A:   b[6],
		B:   b[7],
	}
	return p, p.Validate()
}

// DecodeJSON is the browser-facing form. It validates on the way in for the
// same reason the wire form does.
func DecodeJSON(data []byte) (Packet, error) {
	var p Packet
	if err := json.Unmarshal(data, &p); err != nil {
		return Packet{}, err
	}
	return p, p.Validate()
}

func (p Packet) String() string {
	return fmt.Sprintf("#%d %d->%d %s a=%d b=%d", p.Seq, p.Src, p.Dst, p.T, p.A, p.B)
}

// CellName is the label the players actually see: rows are lettered A..J down
// the side and columns numbered 0..9 across the top, and the aim readout puts
// the letter first. Printing shots the same way means a line in the terminal
// can be compared with the panel without translating anything.
func CellName(col, row uint8) string {
	if col > 9 || row > 9 {
		return fmt.Sprintf("?%d,%d", col, row)
	}
	return fmt.Sprintf("%c%d", 'A'+row, col)
}

// The board, and the fleet, as the rulebook has them; the same numbers as
// game::COLS/ROWS and game::shipLength().
const (
	BoardSize  = 10
	BoardCells = BoardSize * BoardSize
)

var shipLens = [...]int{5, 4, 3, 3, 2}

// PackPlacement is game::packPlacement(): a cell and a facing in one byte.
// The relay needs it to hand a returning player their own ships back.
func PackPlacement(col, row int, vertical bool) uint8 {
	b := uint8((row*BoardSize + col) * 2)
	if vertical {
		b |= 1
	}
	return b
}

// UnpackPlacement reverses game::packPlacement().
func UnpackPlacement(b uint8) (col, row int, vertical bool) {
	cell := int(b) / 2
	return cell % BoardSize, cell / BoardSize, b&1 != 0
}

// game::Page, in order. The numbering is the interface, mirrored here and in
// web/main.ts; see the comment on the enum in src/game/game.h.
var pageNames = [...]string{
	"start", "connecting", "place", "starting", "match",
	"hit", "miss", "sunk", "over", "waiting",
}

var resultNames = [...]string{"MISS", "HIT", "SUNK"}
var shipNames = [...]string{"CARRIER", "BATTLESHIP", "CRUISER", "SUBMARINE", "DESTROYER"}

// Describe renders a packet as one line of English for the terminal. The
// relay is often the only thing watching a match between two headless
// boards, so what it prints is the whole picture of the game.
// Parts splits a packet into the columns a table wants: what kind it is,
// which cell it concerns if any, and whatever is left to say. Describe()
// glues these back together for a single line of terminal output -- one
// source, so the two cannot disagree about what a packet means.
func (p Packet) Parts() (kind, cell, detail string) {
	kind = p.T.String()
	switch p.T {
	case TShot:
		return kind, CellName(p.A, p.B), ""
	case TFleet:
		col, row, vertical := UnpackPlacement(p.B)
		name := "ship?"
		if int(p.A) < len(shipNames) {
			name = shipNames[p.A]
		}
		facing := "across"
		if vertical {
			facing = "down"
		}
		return kind, CellName(uint8(col), uint8(row)), name + " " + facing
	case TResult:
		out := ""
		if int(p.A) < len(resultNames) {
			out = resultNames[p.A]
		}
		if p.A != ResMiss && int(p.B) < len(shipNames) {
			out += " " + shipNames[p.B]
		}
		return kind, "", out
	case TJoin:
		return kind, "", "I am here"
	case TAck:
		return kind, "", "and I heard you"
	case TReady:
		return kind, "", "fleet placed"
	case TReset:
		return kind, "", "start over"
	case TPage:
		name := "?"
		if int(p.A) < len(pageNames) {
			name = pageNames[p.A]
		}
		return kind, "", "go to " + name
	case TMyFleet:
		col, row, vertical := UnpackPlacement(p.B)
		name := "ship?"
		if int(p.A) < len(shipNames) {
			name = shipNames[p.A]
		}
		facing := "across"
		if vertical {
			facing = "down"
		}
		return kind, CellName(uint8(col), uint8(row)), "your " + name + " " + facing
	case TMark:
		out := ""
		if int(p.B&3) < len(resultNames) {
			out = resultNames[p.B&3]
		}
		if p.B&3 != ResMiss && int(p.B>>2) < len(shipNames) {
			out += " " + shipNames[p.B>>2]
		}
		return kind, CellName(p.A%BoardSize, p.A/BoardSize), "your grid: " + out
	case TDamage:
		if p.B == 1 {
			return kind, CellName(p.A%BoardSize, p.A/BoardSize), "a hit on your fleet"
		}
		return kind, CellName(p.A%BoardSize, p.A/BoardSize), "they shot and missed"
	case TTurn:
		if p.A == 1 {
			return kind, "", "it is your move"
		}
		return kind, "", "it is their move"
	case TStart:
		seat := "a"
		if p.A == 1 {
			seat = "b"
		}
		team := "red"
		if p.B == 1 {
			team = "black"
		}
		return kind, "", "you are seat " + seat + ", team " + team
	}
	return kind, "", ""
}

// Describe is Parts() glued back together, for the one column of the CSV that
// wants a sentence rather than fields.
//
// Built FROM Parts rather than written again beside it. The second switch had
// already drifted -- myfleet and start fell through it to a bare type name,
// so the CSV said "myfleet" where the terminal said "your CARRIER down" --
// and every type added later would have drifted the same way.
func (p Packet) Describe() string {
	kind, cell, detail := p.Parts()
	// The name is padded so a column of these lines up, which is the whole
	// reason this is one string instead of three.
	out := fmt.Sprintf("%-6s ", kind)
	switch {
	case cell != "" && detail != "":
		// "CRUISER at E3 across" reads as a sentence; "E3 CRUISER across"
		// reads as a log line that lost its columns.
		if i := strings.Index(detail, " "); i > 0 {
			return out + detail[:i] + " at " + cell + detail[i:]
		}
		return out + detail + " at " + cell
	case cell != "":
		return out + cell
	case detail != "":
		return out + detail
	}
	return strings.TrimSpace(out)
}
