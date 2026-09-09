package main

import (
	"context"
	"encoding/csv"
	"errors"
	"io"
	"strconv"
	"sync"
	"time"
)

// ErrDirectLink is returned when the server is told the two players talk to
// each other directly, and something asks it to relay gameplay anyway.
var ErrDirectLink = errors.New("relay: gameplay is carried peer to peer over ESP-NOW; this server is observing only")

// Hub is the whole routing state: an append-only log of every packet, and a
// cursor per reader. A log rather than a queue per peer because a late
// joiner, a reconnecting browser and a serial console can then all read the
// same history at their own pace, which a consuming queue would not allow.
// A packet and when it passed through. The time is the reason for the type:
// a match is only worth graphing afterwards if the moves are stamped, and the
// packet itself has no room for a clock -- eight bytes is eight bytes.
type entry struct {
	At time.Time
	P  Packet
}

// A fleet is five ships, which is game::SHIP_COUNT in src/game/game_config.h.
// The relay needs the number only to know when a fleet is gone; it holds no
// other rule about them.
const ShipCount = 5

type Hub struct {
	mu sync.Mutex
	// changed is closed and replaced on every append. Closing a channel is
	// the one broadcast primitive that wakes every waiter at once, which is
	// what a long poll needs.
	changed chan struct{}
	log     []entry
	seen    map[PeerID]time.Time
	// Where each player has got to, inferred from what they have sent. The
	// relay holds no rules, so this is a reading of the traffic rather than
	// authority -- but it is what lets a log line say WHICH screen a move
	// happened on, which is most of what you want reading one back.
	phase map[PeerID]string
	// directLink mirrors the -esp-now flag. When set, the two boards carry
	// gameplay between themselves and this server must not also relay it:
	// two copies of every shot would desynchronise the match.
	directLink bool
}

func NewHub(directLink bool) *Hub {
	return &Hub{
		changed:    make(chan struct{}),
		seen:       make(map[PeerID]time.Time),
		phase:      make(map[PeerID]string),
		directLink: directLink,
	}
}

// carriesGameplay reports whether a packet is match traffic, as opposed to
// the bookkeeping that stays on the server even in direct-link mode.
func carriesGameplay(t Type) bool { return t == TShot || t == TResult }

// Send validates and appends. The direct-link rule is enforced here, in one
// place, rather than at each entry point.
func (h *Hub) Send(p Packet) error {
	if err := p.Validate(); err != nil {
		return err
	}
	if h.directLink && carriesGameplay(p.T) {
		return ErrDirectLink
	}

	h.mu.Lock()
	defer h.mu.Unlock()
	now := time.Now()
	h.log = append(h.log, entry{At: now, P: p})
	h.seen[p.Src] = now
	h.notePhase(p)

	close(h.changed)
	h.changed = make(chan struct{})
	return nil
}

// since is a cursor into the log, not a packet sequence number: a caller
// passes back the cursor it was given last time. Sequence numbers are the
// sender's own and two senders may reuse them, so they cannot order the log.
func (h *Hub) collect(peer PeerID, since int) ([]Packet, int) {
	h.mu.Lock()
	defer h.mu.Unlock()
	return h.collectLocked(peer, since)
}

// collectLocked is collect() with the lock already held, so a caller that
// needs the wakeup channel from the same critical section can have both.
func (h *Hub) collectLocked(peer PeerID, since int) ([]Packet, int) {
	if since < 0 || since > len(h.log) {
		since = len(h.log)
	}
	var out []Packet
	for _, e := range h.log[since:] {
		// A peer reads what is addressed to it or broadcast, but never its
		// own packets echoed back at it.
		if e.P.Src == peer {
			continue
		}
		if e.P.Dst == peer || e.P.Dst == PeerAll {
			out = append(out, e.P)
		}
	}
	return out, len(h.log)
}

// Recv returns everything waiting for peer after the cursor. If nothing is
// waiting it blocks until something arrives, the context is cancelled, or the
// deadline passes -- a long poll, so a browser sees a shot the moment it is
// fired instead of on its next timer.
func (h *Hub) Recv(ctx context.Context, peer PeerID, since int, wait time.Duration) ([]Packet, int) {
	deadline := time.NewTimer(wait)
	defer deadline.Stop()

	for {
		// Collect and take the wakeup channel under ONE lock. Doing them
		// separately leaves a window: a Send landing between them closes the
		// channel this waiter has not read yet, so it then parks on the fresh
		// one and sleeps through the very packet it was waiting for. The
		// browser polls with a 25s deadline, so that shows up as a shot or a
		// result that simply stops for half a minute.
		h.mu.Lock()
		out, next := h.collectLocked(peer, since)
		changed := h.changed
		h.mu.Unlock()

		if len(out) > 0 {
			return out, next
		}

		select {
		case <-changed:
			// Something landed; loop and collect it.
		case <-ctx.Done():
			return nil, next
		case <-deadline.C:
			// An empty answer is a real answer: it tells the client its
			// cursor is current and to poll again.
			return nil, next
		}
	}
}

// Reset drops the match history. The cursors clients hold are indexes into
// the log, so they are all invalidated at once -- which is the point: a reset
// is exactly "forget what happened and start again".
func (h *Hub) Reset() {
	h.mu.Lock()
	defer h.mu.Unlock()
	h.log = nil
	h.phase = make(map[PeerID]string)
	close(h.changed)
	h.changed = make(chan struct{})
}

// notePhase moves a player's screen on from what they just sent. Called with
// the lock held.
func (h *Hub) notePhase(p Packet) {
	switch p.T {
	case TJoin, TAck:
		// Only forward: a stray Join mid-match must not drag the display back
		// to the connecting screen.
		if h.phase[p.Src] == "" || h.phase[p.Src] == "start" {
			h.phase[p.Src] = "connect"
		}
	case TFleet, TReady:
		h.phase[p.Src] = "place"
	case TShot, TResult:
		h.phase[p.Src] = "match"
	case TPage:
		// The server moving everyone is not an inference, it is the answer,
		// so it overrides whatever was guessed -- for both players.
		if int(p.A) < len(pageNames) {
			h.phase[PeerA] = pageNames[p.A]
			h.phase[PeerB] = pageNames[p.A]
		}
	}
}

// Finished reports whether this match has been decided: one player's whole
// fleet is sunk.
//
// The relay holds no rules and is not being given any here -- it counts the
// results it has already carried. A fleet is SHIP_COUNT ships, and a ship is
// announced sunk exactly once, by the player whose ship it was; five of those
// from one side is that side's fleet gone. Counting is what lets the room be
// cleared without the clients having to agree to tell it so, which two boards
// that have both stopped talking never would.
func (h *Hub) Finished() bool {
	h.mu.Lock()
	defer h.mu.Unlock()
	// Distinct SHIPS, not sunk answers. A duplicate shot is deliberately
	// re-answered with the same result -- the core does that so a resend is
	// never counted twice -- so counting answers would let one ship shot at
	// five times end the match and wipe the room out from under a live game.
	sunk := map[PeerID]map[uint8]bool{}
	for _, e := range h.log {
		if e.P.T != TResult || e.P.A != ResSunk {
			continue
		}
		if sunk[e.P.Src] == nil {
			sunk[e.P.Src] = map[uint8]bool{}
		}
		sunk[e.P.Src][e.P.B] = true
		if len(sunk[e.P.Src]) >= ShipCount {
			return true
		}
	}
	return false
}

// Phase is where a player has got to, for the log and the dashboard.
func (h *Hub) Phase(id PeerID) string {
	h.mu.Lock()
	defer h.mu.Unlock()
	if p := h.phase[id]; p != "" {
		return p
	}
	return "start"
}

// CSV of everything that has passed through, for looking at a match after it
// is over. One row per packet, with the fields already decoded: a column of
// raw type numbers would need this table to read it anyway.
func (h *Hub) CSV(w io.Writer) error {
	h.mu.Lock()
	rows := make([]entry, len(h.log))
	copy(rows, h.log)
	h.mu.Unlock()

	c := csv.NewWriter(w)
	if err := c.Write([]string{
		"time", "elapsed_ms", "seq", "src", "dst", "type", "a", "b", "cell", "detail",
	}); err != nil {
		return err
	}
	var start time.Time
	for i, e := range rows {
		if i == 0 {
			start = e.At
		}
		cell := ""
		if e.P.T == TShot {
			cell = CellName(e.P.A, e.P.B)
		} else if e.P.T == TFleet {
			col, row, _ := UnpackPlacement(e.P.B)
			cell = CellName(uint8(col), uint8(row))
		}
		if err := c.Write([]string{
			e.At.Format(time.RFC3339Nano),
			strconv.FormatInt(e.At.Sub(start).Milliseconds(), 10),
			strconv.Itoa(int(e.P.Seq)),
			peerName(e.P.Src),
			peerName(e.P.Dst),
			e.P.T.String(),
			strconv.Itoa(int(e.P.A)),
			strconv.Itoa(int(e.P.B)),
			cell,
			e.P.Describe(),
		}); err != nil {
			return err
		}
	}
	c.Flush()
	return c.Error()
}

// MatchState is what the relay can tell about the game from the packets it
// has carried -- it holds no rules of its own, so this is observation rather
// than authority. It is enough to know whether jumping both players to the
// gameplay screen would land them on an empty board.
type MatchState struct {
	Ready map[string]bool   `json:"ready"` // peer -> its fleet is down
	Fleet map[string]int    `json:"fleet"` // peer -> ships reported so far
	Shots int               `json:"shots"`
	Phase map[string]string `json:"phase"` // peer -> which screen it is on
}

func (h *Hub) Match() MatchState {
	h.mu.Lock()
	defer h.mu.Unlock()
	m := MatchState{Ready: map[string]bool{}, Fleet: map[string]int{}, Phase: map[string]string{}}
	for id, ph := range h.phase {
		m.Phase[peerName(id)] = ph
	}
	for _, e := range h.log {
		switch e.P.T {
		case TReady:
			m.Ready[peerName(e.P.Src)] = true
		case TFleet:
			m.Fleet[peerName(e.P.Src)]++
		case TShot:
			m.Shots++
		}
	}
	return m
}

// BothReady is the question the dashboard actually asks: may the players be
// sent to the gameplay screen? Both fleets have to be down, or they arrive at
// a board with nothing on it.
func (h *Hub) BothReady() bool {
	m := h.Match()
	return m.Ready["a"] && m.Ready["b"]
}

type Status struct {
	Version    int              `json:"version"`
	DirectLink bool             `json:"directLink"`
	Packets    int              `json:"packets"`
	Peers      map[string]int64 `json:"peers"` // peer name -> unix seconds last seen
}

func (h *Hub) Status() Status {
	h.mu.Lock()
	defer h.mu.Unlock()
	peers := make(map[string]int64, len(h.seen))
	for id, t := range h.seen {
		peers[peerName(id)] = t.Unix()
	}
	return Status{Version: Version, DirectLink: h.directLink, Packets: len(h.log), Peers: peers}
}

func peerName(id PeerID) string {
	switch id {
	case PeerServer:
		return "server"
	case PeerA:
		return "a"
	case PeerB:
		return "b"
	case PeerAll:
		return "all"
	}
	return "peer"
}
