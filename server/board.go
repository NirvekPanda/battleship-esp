package main

import "sync"

// The board as the relay understands it: five ships a side, and every shot
// that has been answered.
//
// The packet log already holds all of this, and a returning player used to be
// rebuilt by replaying it from the start. That works, but it makes reconnect
// a function of history rather than of state: the log grows for the whole
// match, a duplicate shot appears in it twice, and nothing in it says what
// the board looks like NOW without walking the lot. A lobby that keeps the
// current board answers "what am I looking at?" directly, which is what a
// player who has just restarted is asking.
//
// It holds no rules. Whether a shot was a hit is decided by the player whose
// ships they are, and this only records the answer they gave.
type Board struct {
	mu sync.Mutex
	// Per seat: their own five ships, and the shots THEY fired with the
	// answers they were given. One player's shots are the other player's
	// damage, which is why both displays can be drawn from these two lists.
	fleet map[PeerID]map[int]Ship
	shots map[PeerID]map[int]Shot
	ready map[PeerID]bool
	// The cell each player is waiting for an answer to. A result names no
	// cell -- it answers "the shot you just fired" -- so the relay has to
	// remember which one that was, exactly as the players do.
	pending  map[PeerID]int
	answered map[PeerID]int
}

// Ship is one placement: which of the five, where its bow is, and which way
// it lies. The same three facts the frame carries.
type Ship struct {
	Ship     int  `json:"ship"` // 0..4, index into the fleet
	Col      int  `json:"col"`
	Row      int  `json:"row"`
	Vertical bool `json:"vertical"`
}

// Shot is one answered shot: where, and what came back.
type Shot struct {
	Col    int    `json:"col"`
	Row    int    `json:"row"`
	Result uint8  `json:"result"` // ResMiss / ResHit / ResSunk
	Ship   int    `json:"ship"`   // which ship, when it hit
	Name   string `json:"name"`   // "A5", so a reader needs no board geometry
}

func NewBoard() *Board {
	return &Board{
		fleet:    map[PeerID]map[int]Ship{},
		shots:    map[PeerID]map[int]Shot{},
		ready:    map[PeerID]bool{},
		pending:  map[PeerID]int{},
		answered: map[PeerID]int{},
	}
}

// Note folds one carried packet into the board. Called for every packet the
// table accepts, so the board is never a separate account of the match that
// could disagree with the log -- it is the log, added up.
//
// Keyed by ship index and by cell rather than appended: a fleet packet resent
// after a reconnect, or a duplicate shot answered twice (which the core does
// deliberately), must not become a second ship or a second hit.
func (b *Board) Note(p Packet) {
	b.mu.Lock()
	defer b.mu.Unlock()

	switch p.T {
	case TFleet, TMyFleet:
		col, row, vertical := UnpackPlacement(p.B)
		src := p.Src
		if p.T == TMyFleet {
			src = p.Dst // the server handing a player their own fleet back
		}
		if b.fleet[src] == nil {
			b.fleet[src] = map[int]Ship{}
		}
		b.fleet[src][int(p.A)] = Ship{Ship: int(p.A), Col: col, Row: row, Vertical: vertical}

	case TReady:
		b.ready[p.Src] = true

	case TResult:
		// A result answers "the shot you just fired", naming no cell, so the
		// relay remembers which cell that was exactly as the players do. An
		// answer with nothing outstanding is a duplicate and is dropped:
		// counting it would put a mark on a cell nobody shot at.
		shooter := p.Dst
		cell, waiting := b.pending[shooter]
		if !waiting {
			return
		}
		delete(b.pending, shooter)
		s, ok := b.shots[shooter][cell]
		if !ok {
			return
		}
		s.Result = p.A
		s.Ship = int(p.B)
		b.shots[shooter][cell] = s
		b.answered[shooter]++

	case TShot:
		if b.shots[p.Src] == nil {
			b.shots[p.Src] = map[int]Shot{}
		}
		cell := int(p.B)*BoardSize + int(p.A)
		if _, seen := b.shots[p.Src][cell]; seen {
			return // a re-sent shot is the same shot
		}
		b.shots[p.Src][cell] = Shot{
			Col: int(p.A), Row: int(p.B), Result: resultPending,
			Name: CellName(p.A, p.B),
		}
		b.pending[p.Src] = cell
	}
}

// A shot that has been fired and not yet answered. Outside the protocol's
// three results, so it can never be mistaken for one.
const resultPending uint8 = 255

// Side is one player's half of the board.
type Side struct {
	Seat  string `json:"seat"`
	Name  string `json:"name,omitempty"`
	Ready bool   `json:"ready"`
	Fleet []Ship `json:"fleet"` // where their ships are
	Shots []Shot `json:"shots"` // what they have fired, and the answers
}

// Snapshot is the whole board, both sides, as it stands. Both sides, because
// a panel draws two: the grid you are attacking is your own shots, and the
// board underneath is theirs.
type Snapshot struct {
	Room int  `json:"room"`
	A    Side `json:"a"`
	B    Side `json:"b"`
}

func (b *Board) Snapshot(room int) Snapshot {
	b.mu.Lock()
	defer b.mu.Unlock()
	return Snapshot{Room: room, A: b.sideLocked(PeerA, "a"), B: b.sideLocked(PeerB, "b")}
}

func (b *Board) sideLocked(id PeerID, seat string) Side {
	s := Side{Seat: seat, Ready: b.ready[id], Fleet: []Ship{}, Shots: []Shot{}}
	// Ordered by ship index and by cell, so two calls describing the same
	// board return the same bytes -- a map's own order does not.
	for i := 0; i < ShipCount; i++ {
		if ship, ok := b.fleet[id][i]; ok {
			s.Fleet = append(s.Fleet, ship)
		}
	}
	for cell := 0; cell < BoardSize*BoardSize; cell++ {
		if shot, ok := b.shots[id][cell]; ok {
			s.Shots = append(s.Shots, shot)
		}
	}
	return s
}

// Frames is the same board as packets, for a client with no JSON parser --
// which is every board. They are the packets that would have built this
// position, in the order the core expects them: your own fleet, then theirs,
// then each shot with the answer it was given.
//
// Built from the CURRENT board rather than replayed from the log, so a
// reconnect costs one exchange whatever the match has been through, and a
// shot that was answered twice is sent once.
func (b *Board) Frames(seat PeerID) []Packet {
	b.mu.Lock()
	defer b.mu.Unlock()

	other := PeerB
	if seat == PeerB {
		other = PeerA
	}

	// A match that has not started has no board to hand back, and saying so
	// is the difference between "walk back into your game" and "start one".
	// Answering with a lone `turn` packet -- true of every fresh match, since
	// somebody always has the move -- told both clients they had just
	// reconnected to something, so neither went on to lay out a fleet and the
	// game never began.
	if len(b.fleet[seat]) == 0 && len(b.fleet[other]) == 0 &&
		len(b.shots[seat]) == 0 && len(b.shots[other]) == 0 {
		return []Packet{}
	}

	out := []Packet{}

	// Their own ships, which is the one thing the log can never give them
	// back: a player is never sent their own packets.
	for i := 0; i < ShipCount; i++ {
		if s, ok := b.fleet[seat][i]; ok {
			out = append(out, Packet{V: Version, Src: PeerServer, Dst: seat,
				T: TMyFleet, A: uint8(s.Ship), B: PackPlacement(s.Col, s.Row, s.Vertical)})
		}
	}
	// The enemy roster: which vessels they are hunting. Their POSITIONS are
	// carried too, exactly as they are during a live match -- the core keeps
	// them to name a sinking and never draws them on the tracking grid.
	for i := 0; i < ShipCount; i++ {
		if s, ok := b.fleet[other][i]; ok {
			out = append(out, Packet{V: Version, Src: other, Dst: seat,
				T: TFleet, A: uint8(s.Ship), B: PackPlacement(s.Col, s.Row, s.Vertical)})
		}
	}
	if b.ready[other] {
		out = append(out, Packet{V: Version, Src: other, Dst: seat, T: TReady})
	}
	// Their own shots come back as MARKS on named cells, not as the results
	// they originally were: a result means "the shot you just fired", and a
	// player who has restarted fired nothing. The enemy's hits come back as
	// DAMAGE for the same reason -- replaying them as shots would have the
	// core dutifully answer every one of them all over again.
	for cell := 0; cell < BoardSize*BoardSize; cell++ {
		if s, ok := b.shots[seat][cell]; ok && s.Result != resultPending {
			out = append(out, Packet{V: Version, Src: PeerServer, Dst: seat,
				T: TMark, A: uint8(cell), B: s.Result | uint8(s.Ship)<<2})
		}
		// Every answered shot of theirs, hit or miss: the hits are damage to
		// our hulls, and the misses are the marks that say which squares they
		// have already spent a turn on. Leaving the misses out left a
		// returning player unable to see where the other player had been.
		if s, ok := b.shots[other][cell]; ok && s.Result != resultPending {
			hit := uint8(0)
			if s.Result != ResMiss {
				hit = 1
			}
			out = append(out, Packet{V: Version, Src: PeerServer, Dst: seat,
				T: TDamage, A: uint8(cell), B: hit})
		}
	}

	// Whose move it is. Seat A shoots first and the turn passes on every
	// answered shot, so it follows from the two counts -- the relay is
	// reading the traffic, not holding a rule.
	mine := b.answered[seat] <= b.answered[other]
	if seat == PeerB {
		mine = b.answered[seat] < b.answered[other]
	}
	turn := uint8(0)
	if mine {
		turn = 1
	}
	out = append(out, Packet{V: Version, Src: PeerServer, Dst: seat, T: TTurn, A: turn})

	// And the screen to be on. Both fleets down means the match is running,
	// so a returning player lands on the board rather than on placement.
	if b.ready[seat] && b.ready[other] {
		out = append(out, Packet{V: Version, Src: PeerServer, Dst: seat,
			T: TPage, A: uint8(pageMatch)})
	}
	return out
}

// game::Page::Match, which is what a returning player is put back on. Named
// here rather than written as 4: the numbering is an interface shared with
// the core and web/main.ts.
const pageMatch = 4
