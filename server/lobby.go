package main

import (
	"errors"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
	"unicode/utf8"
)

// Who is connected, independent of who is playing.
//
// A seat is a role in a match and there are two of them; a player is a person
// and there may be many. Conflating the two is what made a lobby impossible:
// the third person to open the page was turned away rather than shown a list.
//
// Identity is a UUID the client makes once and keeps. The name is a label and
// may collide freely -- two people called "nirvek" are two players, and the
// relay never has to guess which. That is also what makes reconnecting
// possible: a returning player is recognised by something that outlives their
// connection, which a seat does not.

const lobbyStale = 45 * time.Second

// Five numbered rooms, fixed. A player who does not want to wait for someone
// to choose them back picks a room instead, and the second person into that
// room is their opponent -- so five matches can be running at once and a
// packet can be checked against the room its sender is in.
const RoomCount = 5

type Player struct {
	ID    string    `json:"id"`   // UUID, from the client
	Name  string    `json:"name"` // a label; duplicates are fine
	Team  string    `json:"team"` // "red" or "black"
	Seen  time.Time `json:"-"`
	Match string    `json:"match,omitempty"` // the match they are in, if any
	// Who this player has chosen as their opponent. A match starts when two
	// players have chosen each other -- agreement rather than an offer and an
	// acceptance, so there is no half-answered invitation to get out of step.
	Invited string `json:"invited,omitempty"`
	// Whether they have taken their seat in the match they are in. A match
	// outlives the connection that was playing it -- that is what makes
	// reconnecting possible -- but a player who has just restarted is put
	// back on the lobby list, not into the game: they walk back in by
	// choosing the lobby their match is in, which is what sets this.
	//
	// Without it, reloading the page dropped the player straight back into a
	// match they had not asked to return to, on a page they never chose.
	Attached bool `json:"-"`
	// Which numbered room they are sitting in, 1..RoomCount, or 0 for none.
	// Kept while their match runs: it is what says whether two players are
	// allowed to be talking to each other at all.
	Room int `json:"room,omitempty"`
}

type Lobby struct {
	mu      sync.Mutex
	players map[string]*Player
	matches map[string]*Match
	nextID  int
	// Called when a match is cleared away, with the room it was played in.
	// The lobby knows WHO was playing; the packet log and the seats belong to
	// the table, so wiping those is the caller's to do -- and it has to
	// happen, or the next pair to sit down in that room would poll a log full
	// of somebody else's fleet, shots and results.
	onEnd func(room int)
	// Bumped whenever anything a lobby list shows changes: who is in a room,
	// who is playing, who has gone. A client polling every second passes the
	// version it last saw and is answered "nothing changed" -- which is the
	// answer almost every time, and costs a status line instead of a list.
	version int
	// Rooms whose match was reaped, waiting for their table to be wiped. A
	// slice rather than a call from inside reapLocked: reaping happens with
	// the lock held, and the table has a lock of its own.
	ended []int
}

type Match struct {
	ID      string    `json:"id"`
	A       string    `json:"a"` // player UUID in seat a
	B       string    `json:"b"` // player UUID in seat b
	Started time.Time `json:"started"`
	Room    int       `json:"room,omitempty"` // the numbered room it came from, if any
	// The transport tokens, handed out when the match starts rather than when
	// a page loads. Not sent to the lobby at large -- each player is told only
	// their own.
	TokenA string `json:"-"`
	TokenB string `json:"-"`
}

// Attached reports whether this player has taken their seat in the match they
// are in -- by starting it, or by choosing its lobby again after a restart. A
// match they have not walked back into is one they are not playing yet.
func (l *Lobby) Attached(id string) bool {
	l.mu.Lock()
	defer l.mu.Unlock()
	p, ok := l.players[id]
	return ok && p.Attached && p.Match != ""
}

// SeatOf is which end of a match a player is on and the token they talk with,
// read under the lobby's lock.
//
// Match.Seat reads two fields that startMatch writes, so reading them without
// the lock is a data race however harmless it looks -- and a client that read
// a half-written token would be told to talk with a seat that does not exist.
func (l *Lobby) SeatOf(id string) (seat, token string) {
	l.mu.Lock()
	defer l.mu.Unlock()
	p, ok := l.players[id]
	if !ok || p.Match == "" {
		return "", ""
	}
	m, ok := l.matches[p.Match]
	if !ok {
		return "", ""
	}
	return m.Seat(id)
}

// SetToken records the token a seat talks with. Called as a match starts, and
// again for a player who has reconnected and needs a fresh one.
func (l *Lobby) SetToken(matchID, seat, token string) {
	l.mu.Lock()
	defer l.mu.Unlock()
	m, ok := l.matches[matchID]
	if !ok {
		return
	}
	if seat == "a" {
		m.TokenA = token
	} else {
		m.TokenB = token
	}
}

// Seat is which end of the match a player is on, and Token is how they talk.
// Call it through Lobby.SeatOf rather than directly: the token fields are
// written under the lobby's lock.
func (m *Match) Seat(playerID string) (string, string) {
	if m.A == playerID {
		return "a", m.TokenA
	}
	if m.B == playerID {
		return "b", m.TokenB
	}
	return "", ""
}

func NewLobby() *Lobby {
	return &Lobby{players: map[string]*Player{}, matches: map[string]*Match{}, version: 1}
}

// Version is what a lobby list looked like when it was last different. A
// client that has seen this version has seen the current list.
func (l *Lobby) Version() int {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.version
}

// OnMatchEnd registers what to do with the table when a match is cleared.
func (l *Lobby) OnMatchEnd(f func(room int)) { l.onEnd = f }

// Join records a player, or refreshes one that is already known. A returning
// UUID keeps whatever it had -- its team, and the match it was in -- which is
// the whole of what makes reconnecting work.
func (l *Lobby) Join(id, name string) *Player {
	l.mu.Lock()
	defer l.flushEnded() // runs after the unlock below
	defer l.mu.Unlock()
	l.reapLocked()

	p, known := l.players[id]
	if !known {
		p = &Player{ID: id, Team: l.freeTeamLocked()}
		l.players[id] = p
		l.version++
	}
	if name := cleanName(name); name != "" {
		if p.Name != name {
			l.version++
		}
		p.Name = name
	}
	// A new connection has not taken its seat yet, whatever it was doing
	// before. The lobby list is where everyone starts.
	p.Attached = false
	p.Seen = time.Now()
	return p
}

// A name is a label chosen by the player, and it is written verbatim into a
// text protocol that a board parses with sscanf: lines separated by newlines,
// names from counts by spaces, and the two players in a room by a pipe. So a
// name may not contain any of those, or a player could type a line of
// protocol -- "x\nmatched b black <token>" -- and hand a board a seat and a
// token of their choosing.
//
// Cut to what a panel can show, too: nothing longer is ever drawn, and a
// kilobyte of name has no business travelling to a microcontroller.
func cleanName(name string) string {
	// Capped in BYTES, not runes, and cut on a rune boundary. The panels draw
	// bytes and the board reads a line into a fixed buffer, so sixteen runes
	// of something like emoji is sixty-four bytes -- four times what either
	// end has room for, and a line that did not fit is a line the board could
	// not count its way past.
	out := make([]byte, 0, LOBBY_NAME_MAX)
	for _, r := range name {
		if r < 0x20 || r == 0x7f || r == '|' {
			continue // control characters, newlines, and the field separator
		}
		if len(out)+utf8.RuneLen(r) > LOBBY_NAME_MAX {
			break
		}
		out = utf8.AppendRune(out, r)
	}
	return strings.TrimSpace(string(out))
}

// What a panel line holds, in bytes: game::LOBBY_NAME_CHARS in
// src/game/game_config.h. The relay truncates as well as the core so that
// what the log, the dashboard and the panels say about a player is one
// string, not three -- and so that a room line fits the buffer a board reads
// it into.
const LOBBY_NAME_MAX = 16

// Teams alternate, so two players who arrive together are never the same
// colour -- the panels show the team rather than the name, and two reds would
// make the sunk announcement ambiguous.
func (l *Lobby) freeTeamLocked() string {
	reds := 0
	for _, p := range l.players {
		if p.Team == "red" {
			reds++
		}
	}
	if reds*2 <= len(l.players) {
		return "red"
	}
	return "black"
}

// TouchSeat marks the player sitting in one seat of one room as still here.
//
// Playing a match is not, by itself, something the lobby can see: a player in
// a game talks to their table -- /v1/send and /v1/recv -- and never asks for
// the lobby list again. Their Seen time therefore stood still while they
// played, and the reaper dropped both of them mid-match: forty-five seconds
// into a game the room emptied, the match was cleared and two people watching
// their own boards were told their seats no longer existed.
func (l *Lobby) TouchSeat(room int, seat string) {
	l.mu.Lock()
	defer l.mu.Unlock()
	for _, m := range l.matches {
		if m.Room != room {
			continue
		}
		id := m.A
		if seat == "b" {
			id = m.B
		}
		if p, ok := l.players[id]; ok {
			p.Seen = time.Now()
		}
		return
	}
}

func (l *Lobby) Touch(id string) {
	l.mu.Lock()
	defer l.mu.Unlock()
	if p, ok := l.players[id]; ok {
		p.Seen = time.Now()
	}
}

// Others is the list one player sees: everyone else who is connected and not
// already in a match.
//
// The caller is left out HERE rather than by the client. A client that forgot
// to filter would let someone invite themselves, and the relay would then be
// pairing a player with themselves with no rule left to catch it.
func (l *Lobby) Others(id string) []Player {
	l.mu.Lock()
	defer l.flushEnded() // runs after the unlock below
	defer l.mu.Unlock()
	l.reapLocked()

	out := []Player{}
	for _, p := range l.players {
		if p.ID == id || p.Match != "" {
			continue
		}
		out = append(out, *p)
	}
	// Stable order, so a list that is polled repeatedly does not shuffle under
	// a cursor that is pointing at one of its lines.
	sort.Slice(out, func(i, j int) bool { return out[i].ID < out[j].ID })
	return out
}

// Invite records a choice and starts a match if it is mutual.
func (l *Lobby) Invite(from, to string) *Match {
	l.mu.Lock()
	defer l.flushEnded() // runs after the unlock below
	defer l.mu.Unlock()
	// Reap first, like every other public method here. Without it this could
	// read a match whose players went stale minutes ago and refuse a pairing
	// that the very next Others() call would have allowed -- with nothing
	// either player could do to make progress.
	l.reapLocked()

	a, ok := l.players[from]
	if !ok || from == to {
		return nil // inviting yourself is not a thing that can happen
	}
	b, ok := l.players[to]
	if !ok || a.Match != "" || b.Match != "" {
		return nil
	}
	a.Invited = to
	if b.Invited != from {
		return nil // not agreed yet; wait for them to choose us back
	}

	return l.pairLocked(a, b, 0)
}

// Seats by UUID order, not by who happened to click second. Seat A shoots
// first, so deciding it on click order would hand out the first move at
// random -- and a player who reconnects must land in the seat they left,
// which only holds if the assignment can be worked out again from the two
// identities alone.
func (l *Lobby) pairLocked(a, b *Player, room int) *Match {
	first, second := a, b
	if second.ID < first.ID {
		first, second = second, first
	}
	l.nextID++
	m := &Match{
		ID: matchName(l.nextID), A: first.ID, B: second.ID,
		Started: time.Now(), Room: room,
	}
	l.matches[m.ID] = m
	a.Match, b.Match = m.ID, m.ID
	a.Attached, b.Attached = true, true
	l.version++
	a.Invited, b.Invited = "", ""
	// A match made by choosing each other is played at the base table, not in
	// a numbered room, so whatever room either of them was sitting in is given
	// up: they are not waiting there any more, and a row that keeps showing
	// them is a row nobody else can have.
	a.Room, b.Room = room, room
	return m
}

// ---- the numbered rooms -------------------------------------------------

// Room is one line of the room list: who is sitting in it, and whether it can
// take anybody else.
type Room struct {
	N       int      `json:"n"`
	Players []string `json:"players"`         // names, in seat order once paired
	IDs     []string `json:"ids"`             // the UUIDs behind those names
	Match   string   `json:"match,omitempty"` // the match running in it, if any
	// Set for the caller's own room: the lobby they are waiting in, or the
	// one holding the match they walked away from. It is the row they may
	// press even when it reads 2/2 -- the seat in it is theirs.
	Yours bool `json:"yours,omitempty"`
}

func (r Room) Full() bool { return len(r.Players) >= 2 }

// Rooms is the whole list, always RoomCount long whether or not anyone is in
// them: the page draws five rows and a room that emptied must still be there
// to be joined again.
//
// It is the list as ONE player sees it -- their own room is marked, because
// that is the row they may press even when it reads 2/2: the seat in it is
// theirs, whether they are waiting in it or walking back into a match.
func (l *Lobby) Rooms(id string) []Room {
	l.mu.Lock()
	defer l.flushEnded() // runs after the unlock below
	defer l.mu.Unlock()
	l.reapLocked()
	out := l.roomsLocked()
	if p, ok := l.players[id]; ok && p.Room >= 1 && p.Room <= RoomCount {
		out[p.Room-1].Yours = true
	}
	return out
}

func (l *Lobby) roomsLocked() []Room {
	out := make([]Room, RoomCount)
	for i := range out {
		out[i] = Room{N: i + 1, Players: []string{}, IDs: []string{}}
	}
	occupants := make([][]*Player, RoomCount+1)
	for _, p := range l.players {
		if p.Room >= 1 && p.Room <= RoomCount {
			occupants[p.Room] = append(occupants[p.Room], p)
		}
	}
	for n := 1; n <= RoomCount; n++ {
		in := occupants[n]
		// Seat order, so "<a> vs <b>" names the player who shoots first
		// first -- and reads the same on every client rather than following
		// whatever order a map happened to hand back.
		sort.Slice(in, func(i, j int) bool { return in[i].ID < in[j].ID })
		r := &out[n-1]
		for _, p := range in {
			name := p.Name
			if name == "" {
				name = "player"
			}
			r.Players = append(r.Players, name)
			r.IDs = append(r.IDs, p.ID)
			if p.Match != "" {
				r.Match = p.Match
			}
		}
	}
	return out
}

// JoinRoom sits a player down in a numbered room, and starts the match when
// they are the second one in. Rejoining the room you are already in is not an
// error: a page that reloads should find its seat, not a refusal.
//
// A full room is refused HERE rather than by the page graying its button: the
// button is a courtesy, the check is the rule -- two clients pressing Join on
// the same room in the same instant is exactly the case a disabled button
// cannot cover.
func (l *Lobby) JoinRoom(id string, n int) (*Match, error) {
	l.mu.Lock()
	defer l.flushEnded() // runs after the unlock below
	defer l.mu.Unlock()
	l.reapLocked()

	if n < 1 || n > RoomCount {
		return nil, errNoSuchRoom
	}
	p, ok := l.players[id]
	if !ok {
		return nil, errUnknownPlayer
	}
	// Their own match, walked back into. This is what reconnecting IS now:
	// the player picks the lobby they were playing in, and only then are they
	// put back in it. A match in some OTHER room is not a reason to hand them
	// this one.
	if p.Match != "" {
		m := l.matches[p.Match]
		if m == nil || m.Room != n {
			return nil, errElsewhere
		}
		// Walking back in: this is the choice that puts them in the game.
		p.Attached = true
		return m, nil
	}

	// Everyone else sitting in this room counts towards it being full --
	// including the two playing a match in it, who are not available to pair
	// with. Counting only the free ones is what let a third player join a
	// room with a game running and pair with someone already in a match,
	// which reassigned that player's match and left their opponent polling a
	// table nobody would answer.
	var free []*Player
	occupied := 0
	for _, q := range l.players {
		if q.ID == id || q.Room != n {
			continue
		}
		occupied++
		if q.Match == "" {
			free = append(free, q)
		}
	}
	if occupied >= 2 {
		return nil, errRoomFull
	}
	// Leaving whichever room they were in happens by simply setting the new
	// one: a player is in one room or none.
	p.Room = n
	p.Invited = ""
	l.version++
	if len(free) == 0 {
		return nil, nil // waiting for someone to join them
	}
	return l.pairLocked(p, free[0], n), nil
}

// LeaveRoom puts a player back in the list of rooms.
//
// Leaving a match ENDS it rather than leaving half of one behind. A game with
// one player in it is not a game, and the room has to come back empty: the
// alternative is a room that says "1/2" for ever, holding a match nobody is
// playing, with the loser's fleet still in the log.
func (l *Lobby) LeaveRoom(id string) {
	l.mu.Lock()
	p, ok := l.players[id]
	if !ok {
		l.mu.Unlock()
		return
	}
	if p.Match == "" {
		p.Room = 0
		l.version++
		l.mu.Unlock()
		return
	}
	match := p.Match
	l.mu.Unlock()
	// The player who is left behind keeps the lobby. Their opponent walked
	// out; they did not, and putting them back on the lobby list as well
	// would take a room off someone who is still standing in it. The row goes
	// back to 1/2 with their name on it, and the next person to join that
	// lobby plays them.
	l.endMatch(match, id)
}

// EndMatch clears a match away: both players are put back in the list of
// rooms, and the room's packet log and seats are wiped by whoever registered
// OnMatchEnd. Everything the match consisted of -- the fleets, the shots, the
// hits and misses -- lives in that log, so this is what makes the next game
// in that room a new game rather than a continuation of the last one.
//
// Safe to call twice: the second call finds no match and does nothing.
func (l *Lobby) EndMatch(matchID string) { l.endMatch(matchID, "") }

// leaving is the player who walked out, if anyone did: everyone ELSE in the
// match keeps the room, because they are still in it.
func (l *Lobby) endMatch(matchID, leaving string) {
	l.mu.Lock()
	m, ok := l.matches[matchID]
	if !ok {
		l.mu.Unlock()
		return
	}
	room := m.Room
	delete(l.matches, matchID)
	l.version++
	for _, id := range []string{m.A, m.B} {
		p, ok := l.players[id]
		if !ok {
			continue
		}
		p.Match = ""
		p.Invited = ""
		if leaving == "" || id == leaving {
			p.Room = 0
		}
	}
	l.mu.Unlock()

	// Outside the lock: the table has a lock of its own, and holding both at
	// once in one order here and the other order elsewhere is how a deadlock
	// is built.
	if l.onEnd != nil {
		l.onEnd(room)
	}
}

var (
	errNoSuchRoom    = errors.New("room: there are only " + strconv.Itoa(RoomCount) + " rooms")
	errRoomFull      = errors.New("room: that room already has two players")
	errUnknownPlayer = errors.New("room: join the lobby first")
	errElsewhere     = errors.New("room: your match is in another lobby")
)

// A short, unique name for a match. Short because it is read in log columns
// beside everything else; unique because ids that wrap around start naming
// two matches at once, and MatchOf would then hand a player the other one's
// seats and tokens. strconv, not a two-character alphabet with 260 values in
// it -- a relay left running reaches 260 matches.
func matchName(n int) string { return "m" + strconv.Itoa(n) }

// MatchInRoom is the match being played in a numbered room, if any. The relay
// knows a table by its room number rather than by who is sitting at it, so
// this is how a decision made in the packet log finds the match it decided.
func (l *Lobby) MatchInRoom(room int) *Match {
	l.mu.Lock()
	defer l.mu.Unlock()
	for _, m := range l.matches {
		if m.Room == room {
			return m
		}
	}
	return nil
}

// Opponent is the name of the other player in this player's match, or "".
//
// A board learns it here rather than from the room list: its poll returns as
// soon as it is told it has been matched, so it may never read a room row
// showing the two of them -- and the waiting page has a name-shaped hole in
// it that says who is being waited for.
func (l *Lobby) Opponent(id string) string {
	l.mu.Lock()
	defer l.mu.Unlock()
	p, ok := l.players[id]
	if !ok || p.Match == "" {
		return ""
	}
	m, ok := l.matches[p.Match]
	if !ok {
		return ""
	}
	other := m.A
	if id == m.A {
		other = m.B
	}
	if q, ok := l.players[other]; ok {
		return displayName(q.Name)
	}
	return ""
}

func (l *Lobby) MatchOf(id string) *Match {
	l.mu.Lock()
	defer l.mu.Unlock()
	if p, ok := l.players[id]; ok && p.Match != "" {
		return l.matches[p.Match]
	}
	return nil
}

func (l *Lobby) Player(id string) (Player, bool) {
	l.mu.Lock()
	defer l.mu.Unlock()
	if p, ok := l.players[id]; ok {
		return *p, true
	}
	return Player{}, false
}

func (l *Lobby) All() []Player {
	l.mu.Lock()
	defer l.flushEnded() // runs after the unlock below
	defer l.mu.Unlock()
	l.reapLocked()
	out := []Player{}
	for _, p := range l.players {
		out = append(out, *p)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].ID < out[j].ID })
	return out
}

// Players who have gone quiet are dropped from the LOBBY. A match they were
// in is kept while ANYONE is still there -- that is exactly the state a
// reconnect needs to find, and a player who returns with the same UUID rejoins
// the match they left.
//
// When the last player of a match has gone, the match goes too, and its room
// comes back empty. Keeping it was what left rooms showing a game that nobody
// was playing: the players were long gone, but a match with a stale player in
// it kept them both in the table, so the room could never be joined again.
func (l *Lobby) reapLocked() {
	now := time.Now()
	gone := map[string]bool{}
	for id, p := range l.players {
		if now.Sub(p.Seen) <= lobbyStale {
			continue
		}
		if p.Match != "" {
			gone[p.Match] = true
			continue // dropped below, but only if nobody is left in the match
		}
		delete(l.players, id)
		l.version++
	}

	for matchID := range gone {
		m, ok := l.matches[matchID]
		if !ok {
			continue
		}
		alive := false
		for _, id := range []string{m.A, m.B} {
			if p, ok := l.players[id]; ok && now.Sub(p.Seen) <= lobbyStale {
				alive = true
			}
		}
		if alive {
			continue
		}
		delete(l.matches, matchID)
		delete(l.players, m.A)
		delete(l.players, m.B)
		l.version++
		l.ended = append(l.ended, m.Room)
	}
}

// flushEnded wipes the tables of matches the reaper cleared away. Called
// after the lobby's own lock is released -- a table has a lock of its own,
// and taking two locks in one order here and the other order elsewhere is how
// a deadlock is built.
//
// Registered as the FIRST defer in each method that reaps, so it runs last,
// after the unlock.
func (l *Lobby) flushEnded() {
	l.mu.Lock()
	rooms := l.ended
	l.ended = nil
	l.mu.Unlock()
	if l.onEnd == nil {
		return
	}
	for _, room := range rooms {
		l.onEnd(room)
	}
}
