package main

import (
	"strconv"
	"sync"
)

// One table per numbered room.
//
// A packet log and two seats used to be the whole server, which meant the
// whole server was one match: seat A was seat A everywhere, so a second pair
// of players had nowhere to sit. A Table is that original pair -- a Hub and
// its Seats -- and there is now one of them per room, so five matches run
// side by side and a token resolves to the table it was issued for.
//
// Room 0 is the base table, kept for everything that came before rooms: the
// dashboard, the CLI, the ESP-NOW path, and a match made by two players
// choosing each other in the open lobby.
type Table struct {
	Room  int
	Hub   *Hub
	Seats *Seats
	// The board as it stands, kept alongside the log the whole match is in.
	// The log says what happened; this says what it looks like now, which is
	// the question a player who has just restarted is asking.
	Board *Board
}

// Label is what the CLI and the dashboard call this table: the main lobby,
// where players arrive and pick a room, or one of the five games.
func (t *Table) Label() string {
	if t.Room == 0 {
		return "main"
	}
	return "lobby " + strconv.Itoa(t.Room)
}

type Tables struct {
	mu     sync.Mutex
	direct bool
	base   *Table
	rooms  map[int]*Table
}

func NewTables(base *Hub, seats *Seats, direct bool) *Tables {
	return &Tables{
		direct: direct,
		base:   &Table{Room: 0, Hub: base, Seats: seats, Board: NewBoard()},
		rooms:  map[int]*Table{},
	}
}

func (t *Tables) Base() *Table { return t.base }

// Room hands back the table for a room, making it the first time it is asked
// for. Room 0 -- and any number outside the five -- is the base table.
func (t *Tables) Room(n int) *Table {
	if n < 1 || n > RoomCount {
		return t.base
	}
	t.mu.Lock()
	defer t.mu.Unlock()
	if tb, ok := t.rooms[n]; ok {
		return tb
	}
	tb := &Table{Room: n, Hub: NewHub(t.direct), Seats: NewSeats(), Board: NewBoard()}
	t.rooms[n] = tb
	return tb
}

// Resolve finds the table a token belongs to. The base is tried first, since
// that is where a board playing without rooms sits; a token is unique across
// all of them, so the order only decides how quickly the answer is found.
func (t *Tables) Resolve(token string) (*Table, PeerID, string, error) {
	for _, tb := range t.All() {
		if id, name, err := t.seatsOf(tb).Resolve(token); err == nil {
			return tb, id, name, nil
		}
	}
	return nil, 0, "", ErrBadToken
}

// seatsOf reads the seats field under the lock, since Reset replaces it.
func (t *Tables) seatsOf(tb *Table) *Seats {
	t.mu.Lock()
	defer t.mu.Unlock()
	return tb.Seats
}

// SeatsOf is seatsOf for callers outside this file: every read of the field
// goes through the lock, because Reset swaps it.
func (t *Tables) SeatsOf(tb *Table) *Seats { return t.seatsOf(tb) }

// BoardOf, likewise: Reset gives a room a fresh board with the fresh seats.
func (t *Tables) BoardOf(tb *Table) *Board {
	t.mu.Lock()
	defer t.mu.Unlock()
	return tb.Board
}

// each is a snapshot of the room tables, so a caller can walk them without
// holding the lock across whatever it does to each one.
func (t *Tables) each() []*Table {
	t.mu.Lock()
	defer t.mu.Unlock()
	out := make([]*Table, 0, len(t.rooms))
	for _, tb := range t.rooms {
		out = append(out, tb)
	}
	return out
}

// Reset wipes a room's table: a fresh packet log and fresh seats, so the next
// pair to sit down there start a new game rather than polling their way
// through somebody else's fleet, shots and results.
//
// The seats are replaced rather than cleared, which also invalidates the old
// tokens -- a client still holding one gets a 401 and rejoins the lobby,
// which is exactly what it should do when the match it was in is over.
func (t *Tables) Reset(n int) {
	// Numbered rooms only. The base table is not a room: it is where a board
	// pointed at a bare relay sits, and where the CLI and the ESP-NOW path
	// live. Wiping it because some match that was never played in a room
	// ended would evict clients that have nothing to do with that match.
	if n < 1 || n > RoomCount {
		return
	}
	// Under the lock, and Resolve takes it too: the seats are REPLACED rather
	// than emptied, and a pointer swapped while another request goroutine is
	// reading it is a data race however harmless the old value looks.
	t.mu.Lock()
	defer t.mu.Unlock()
	tb, ok := t.rooms[n]
	if !ok {
		return
	}
	tb.Hub.Reset()
	tb.Seats = NewSeats()
	tb.Board = NewBoard()
}

// All is every table including the base, for the things that act on the whole
// server: a reset clears every match, not whichever one was first.
func (t *Tables) All() []*Table { return append([]*Table{t.base}, t.each()...) }
