package main

import (
	"crypto/rand"
	"encoding/hex"
	"errors"
	"fmt"
	"sync"
	"time"
)

// Who is playing.
//
// Before this, a peer was whatever a client said it was: src in the packet
// and ?peer= on a poll were both self-declared, so any client could claim to
// be either player, or impersonate the other and answer its own shots. There
// are only ever two seats at this table, so handing them out explicitly is
// both simpler and correct.
//
// A seat is claimed with POST /v1/join and held with a token. This is
// identification, not security: the relay is a local development tool, and
// the token exists so two honest clients cannot be confused for each other,
// not to withstand an attacker who can already read your loopback traffic.

var (
	ErrNoSeats    = errors.New("join: both seats are taken")
	ErrBadToken   = errors.New("auth: unknown or expired token")
	ErrWrongPeer  = errors.New("auth: this token does not own that peer id")
	ErrNeedsToken = errors.New("auth: send X-Peer-Token, obtained from POST /v1/join")
)

// staleAfter releases a seat whose holder has gone quiet. A browser tab that
// is closed never says bye, and without this the second seat would be lost
// until the relay restarts.
// Long enough that a live client is never reclaimed underneath itself, short
// enough that a dead one does not lock a real player out for a minute and a
// half. The browser is the slowest to refresh: its poll parks for up to 25s,
// so its seat is touched at least that often. 45s leaves a wide margin above
// that while halving how long a crashed tab, or a test client that forgot to
// leave, holds a seat it is not using.
const staleAfter = 45 * time.Second

type seat struct {
	id    PeerID
	name  string // what the client called itself, for the CLI
	token string
	seen  time.Time
}

type Seats struct {
	mu    sync.Mutex
	byTok map[string]*seat
	byID  map[PeerID]*seat
}

func NewSeats() *Seats {
	return &Seats{byTok: map[string]*seat{}, byID: map[PeerID]*seat{}}
}

func newToken() string {
	var b [16]byte
	if _, err := rand.Read(b[:]); err != nil {
		// crypto/rand failing is not a condition this program can sensibly
		// continue through, and a predictable token would silently let two
		// clients collide.
		panic("join: no randomness available: " + err.Error())
	}
	return hex.EncodeToString(b[:])
}

// Join hands out the first free seat, preferring A. Returns the seat and
// whether an expired one was reclaimed to make room, which the CLI reports so
// a mysterious reconnection is visible rather than silent.
func (s *Seats) Join(name string) (PeerID, string, bool, error) {
	s.mu.Lock()
	defer s.mu.Unlock()

	reclaimed := false
	now := time.Now()
	for id, st := range s.byID {
		if now.Sub(st.seen) > staleAfter {
			delete(s.byID, id)
			delete(s.byTok, st.token)
			reclaimed = true
		}
	}

	if name == "" {
		name = "peer"
	}

	// A client asking for a seat it already holds is taking its own seat
	// back, not asking for a second one. This is what makes reflashing the
	// board work: the old session is still holding its seat and will be for
	// another ninety seconds, and the freshly booted board would otherwise
	// find the table full of itself. The old token is invalidated, so the
	// dead session cannot go on using it.
	//
	// Names must therefore be unique per client. The browser appends a random
	// suffix per tab for exactly this reason -- two tabs both called "browser"
	// would take the seat off each other for ever.
	for id, st := range s.byID {
		if st.name == name {
			delete(s.byTok, st.token)
			st.token = newToken()
			st.seen = now
			s.byTok[st.token] = st
			return id, st.token, reclaimed, nil
		}
	}

	for _, id := range []PeerID{PeerA, PeerB} {
		if _, taken := s.byID[id]; taken {
			continue
		}
		st := &seat{id: id, name: name, token: newToken(), seen: now}
		s.byID[id] = st
		s.byTok[st.token] = st
		return id, st.token, reclaimed, nil
	}
	return 0, "", reclaimed, ErrNoSeats
}

// Clear empties both seats. The blunt instrument, for when the table is held
// by clients that are gone but still polling -- a browser tab left open in
// another window is the usual one. Everyone still connected simply rejoins,
// since both clients retry, so this is disruptive rather than destructive.
func (s *Seats) Clear() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	n := len(s.byID)
	s.byID = map[PeerID]*seat{}
	s.byTok = map[string]*seat{}
	return n
}

// Drop frees the seat held by one named client, leaving the other alone.
// Reflashing a board under a new name strands its old seat: the old name no
// longer matches, so the take-your-own-seat-back rule cannot reclaim it, and
// it sits there until the idle timeout. Clearing both seats would work but
// would also throw out the player who did nothing wrong.
func (s *Seats) Drop(name string) (PeerID, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for id, st := range s.byID {
		if st.name == name {
			delete(s.byTok, st.token)
			delete(s.byID, id)
			return id, true
		}
	}
	return 0, false
}

// Holders is who is sitting down, for the message when someone is turned
// away. "Both seats are taken" is not actionable on its own; the names and
// how long they have been quiet are.
func (s *Seats) Holders() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := ""
	now := time.Now()
	for _, id := range []PeerID{PeerA, PeerB} {
		st, ok := s.byID[id]
		if !ok {
			continue
		}
		if out != "" {
			out += ", "
		}
		out += fmt.Sprintf("%s=%q (quiet %ds)", peerName(id), st.name,
			int(now.Sub(st.seen).Seconds()))
	}
	return out
}

// Assign hands a specific seat to a named client, replacing whoever held it.
//
// Used when a match starts rather than when a page loads. Claiming a seat on
// arrival is what limited the whole relay to two connected clients: the third
// person to open the page was turned away instead of being shown a lobby. A
// seat is a role in a match, and there is no match until two people have
// chosen each other.
func (s *Seats) Assign(id PeerID, name string) string {
	s.mu.Lock()
	defer s.mu.Unlock()
	if old, ok := s.byID[id]; ok {
		delete(s.byTok, old.token)
	}
	st := &seat{id: id, name: name, token: newToken(), seen: time.Now()}
	s.byID[id] = st
	s.byTok[st.token] = st
	return st.token
}

// Resolve turns a token into the peer that holds it, and refreshes the seat's
// last-seen time so an active player is never reclaimed underneath itself.
func (s *Seats) Resolve(token string) (PeerID, string, error) {
	if token == "" {
		return 0, "", ErrNeedsToken
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	st, ok := s.byTok[token]
	if !ok {
		return 0, "", ErrBadToken
	}
	st.seen = time.Now()
	return st.id, st.name, nil
}

// Leave frees a seat on a clean exit.
func (s *Seats) Leave(token string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if st, ok := s.byTok[token]; ok {
		delete(s.byTok, token)
		delete(s.byID, st.id)
	}
}

// Name is what the CLI prints for a peer: the client's own label where one is
// known, so a line reads "browser" rather than "1".
func (s *Seats) Name(id PeerID) string {
	s.mu.Lock()
	defer s.mu.Unlock()
	if st, ok := s.byID[id]; ok {
		return st.name
	}
	return peerName(id)
}

type SeatInfo struct {
	Peer     string `json:"peer"`
	Name     string `json:"name"`
	LastSeen int64  `json:"lastSeen"`
}

func (s *Seats) List() []SeatInfo {
	s.mu.Lock()
	defer s.mu.Unlock()
	var out []SeatInfo
	for _, id := range []PeerID{PeerA, PeerB} {
		if st, ok := s.byID[id]; ok {
			out = append(out, SeatInfo{Peer: peerName(id), Name: st.name, LastSeen: st.seen.Unix()})
		}
	}
	return out
}
