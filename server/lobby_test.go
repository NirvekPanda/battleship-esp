package main

import (
	"strings"
	"testing"
	"time"
)

// The rule the whole lobby turns on: a match starts when two players have
// chosen EACH OTHER. One choice is not an offer waiting to be accepted, it is
// half of an agreement -- which is why there is no accept/decline state to get
// out of step.
func TestPairingIsMutual(t *testing.T) {
	l := NewLobby()
	l.Join("uuid-a", "nirvek")
	l.Join("uuid-b", "nirvek") // the same name on purpose: names may collide

	if m := l.Invite("uuid-a", "uuid-b"); m != nil {
		t.Fatal("one player choosing should not start a match")
	}
	m := l.Invite("uuid-b", "uuid-a")
	if m == nil {
		t.Fatal("choosing each other should start a match")
	}
	if m.A != "uuid-a" || m.B != "uuid-b" {
		t.Fatalf("seats went to the wrong players: %+v", m)
	}
	if got := l.MatchOf("uuid-a"); got == nil || got.ID != m.ID {
		t.Fatal("both players should be in the match")
	}
}

func TestCannotPlayYourself(t *testing.T) {
	l := NewLobby()
	l.Join("uuid-a", "solo")
	if m := l.Invite("uuid-a", "uuid-a"); m != nil {
		t.Fatal("a player must not be able to pair with themselves")
	}
	// And they are never offered the chance: the list they see excludes them.
	for _, p := range l.Others("uuid-a") {
		if p.ID == "uuid-a" {
			t.Fatal("a player appears in their own lobby list")
		}
	}
}

func TestDuplicateNamesAreDifferentPlayers(t *testing.T) {
	l := NewLobby()
	a := l.Join("uuid-1", "nirvek")
	b := l.Join("uuid-2", "nirvek")
	if a.ID == b.ID {
		t.Fatal("two UUIDs collapsed into one player")
	}
	if len(l.All()) != 2 {
		t.Fatalf("want two players, got %d", len(l.All()))
	}
	// Teams alternate, so the panels can tell them apart at all.
	if a.Team == b.Team {
		t.Fatalf("both players got team %q", a.Team)
	}
}

func TestRejoiningKeepsTheMatch(t *testing.T) {
	l := NewLobby()
	l.Join("uuid-a", "a")
	l.Join("uuid-b", "b")
	l.Invite("uuid-a", "uuid-b")
	m := l.Invite("uuid-b", "uuid-a")

	// The player drops and comes back with the same UUID.
	again := l.Join("uuid-a", "a")
	if again.Match != m.ID {
		t.Fatalf("a returning player lost their match: %q", again.Match)
	}
	// And a player in a match is not offered around the lobby.
	l.Join("uuid-c", "c")
	for _, p := range l.Others("uuid-c") {
		if p.ID == "uuid-a" || p.ID == "uuid-b" {
			t.Fatal("a player already in a match was offered as an opponent")
		}
	}
}

// A room is the other way to find an opponent: no waiting to be chosen, the
// second person through the door is who you play.
func TestRoomPairsTheSecondPlayerIn(t *testing.T) {
	l := NewLobby()
	l.Join("uuid-a", "ann")
	l.Join("uuid-b", "bo")

	if m, err := l.JoinRoom("uuid-a", 3); err != nil || m != nil {
		t.Fatalf("one player in a room is not a match: %v %v", m, err)
	}
	m, err := l.JoinRoom("uuid-b", 3)
	if err != nil || m == nil {
		t.Fatalf("the second player should start the match: %v", err)
	}
	if m.Room != 3 {
		t.Fatalf("match kept the wrong room: %d", m.Room)
	}
	// Seats by UUID order, so a reconnecting player lands where they left.
	if m.A != "uuid-a" || m.B != "uuid-b" {
		t.Fatalf("seats went to the wrong players: %+v", m)
	}
}

func TestRoomRefusesAThirdPlayer(t *testing.T) {
	l := NewLobby()
	for _, id := range []string{"a", "b", "c"} {
		l.Join(id, id)
	}
	_, _ = l.JoinRoom("a", 1)
	_, _ = l.JoinRoom("b", 1)
	if _, err := l.JoinRoom("c", 1); err == nil {
		t.Fatal("a full room took a third player")
	}
	// The refusal is the rule, not the greyed-out button: two clients pressing
	// Join at the same instant is exactly what the button cannot cover.
	if _, err := l.JoinRoom("c", RoomCount+1); err == nil {
		t.Fatal("a room outside the five was accepted")
	}
	if m, err := l.JoinRoom("c", 2); err != nil || m != nil {
		t.Fatalf("another room should still be open: %v %v", m, err)
	}
}

// Five rows, always, whoever is in them -- the page draws a fixed list and an
// empty room has to be there to be joined again.
func TestRoomsAlwaysListsAllFive(t *testing.T) {
	l := NewLobby()
	l.Join("a", "ann")
	_, _ = l.JoinRoom("a", 5)

	rooms := l.Rooms("")
	if len(rooms) != RoomCount {
		t.Fatalf("want %d rooms, got %d", RoomCount, len(rooms))
	}
	if rooms[0].N != 1 || len(rooms[0].Players) != 0 || rooms[0].Full() {
		t.Fatalf("room 1 should be empty and joinable: %+v", rooms[0])
	}
	if got := rooms[4].Players; len(got) != 1 || got[0] != "ann" {
		t.Fatalf("room 5 should hold ann: %+v", got)
	}
}

// Five matches at once is the point of the rooms, and each one needs its own
// packet log: seat A of room 2 must not read seat A of room 1's mail.
func TestTablesAreSeparatePerRoom(t *testing.T) {
	tables := NewTables(NewHub(false), NewSeats(), false)
	one, two := tables.Room(1), tables.Room(2)
	if one == two || one.Hub == two.Hub || one.Seats == two.Seats {
		t.Fatal("two rooms shared a table")
	}
	if tables.Room(1) != one {
		t.Fatal("the same room handed back two tables")
	}
	tok := one.Seats.Assign(PeerA, "ann")
	tb, id, _, err := tables.Resolve(tok)
	if err != nil || tb != one || id != PeerA {
		t.Fatalf("a token resolved to the wrong table: %v %v", tb, err)
	}
	if _, _, err := two.Seats.Resolve(tok); err == nil {
		t.Fatal("a token from room 1 was accepted in room 2")
	}
}

// Standing up again frees the room for whoever looks next. A player in a
// match keeps theirs: that is the room the match is being played in.
func TestLeaveRoomFreesItForEveryoneElse(t *testing.T) {
	l := NewLobby()
	l.Join("a", "ann")
	l.Join("b", "bo")
	if _, err := l.JoinRoom("a", 4); err != nil {
		t.Fatalf("join: %v", err)
	}
	l.LeaveRoom("a")
	if got := l.Rooms("")[3]; len(got.Players) != 0 {
		t.Fatalf("the room still shows a player who left: %+v", got)
	}
	// And it can be taken by somebody else, which is the whole point.
	if _, err := l.JoinRoom("b", 4); err != nil {
		t.Fatalf("a freed room refused the next player: %v", err)
	}

	// Leaving a MATCH ends it rather than leaving half a game behind: a game
	// with one player in it is not a game, and the room has to come back
	// empty for whoever looks next.
	ended := []int{}
	l.OnMatchEnd(func(room int) { ended = append(ended, room) })
	m, err := l.JoinRoom("a", 4)
	if err != nil || m == nil {
		t.Fatalf("the room should have paired the two players again: %v", err)
	}
	l.LeaveRoom("a")
	if p, _ := l.Player("a"); p.Match != "" || p.Room != 0 {
		t.Fatalf("the player who left is still in a match: %+v", p)
	}
	// The player left behind keeps the lobby -- they are still standing in it
	// -- but the match is over for both of them.
	if p, _ := l.Player("b"); p.Match != "" || p.Room != 4 {
		t.Fatalf("the player left behind lost their lobby: %+v", p)
	}
	if got := l.Rooms("")[3]; got.Full() || len(got.Players) != 1 {
		t.Fatalf("the room should hold the one player still in it: %+v", got)
	}
	if len(ended) != 1 || ended[0] != 4 {
		t.Fatalf("the table for room 4 was not wiped: %v", ended)
	}
	if l.MatchOf("b") != nil {
		t.Fatal("the match outlived both its players")
	}
}

// A match whose players have both gone quiet is cleared away, and its room
// comes back empty. Keeping it was what left rooms showing a game nobody was
// playing: the players were long gone, but a match with a stale player in it
// held them both, so the room could never be joined again.
func TestAMatchNobodyIsPlayingIsReaped(t *testing.T) {
	l := NewLobby()
	wiped := []int{}
	l.OnMatchEnd(func(room int) { wiped = append(wiped, room) })
	l.Join("a", "ann")
	l.Join("b", "bo")
	if _, err := l.JoinRoom("a", 2); err != nil {
		t.Fatalf("join: %v", err)
	}
	if m, err := l.JoinRoom("b", 2); err != nil || m == nil {
		t.Fatalf("the second player should start the match: %v", err)
	}

	// Both stop polling. Reaping happens on the next look at the lobby.
	l.mu.Lock()
	for _, p := range l.players {
		p.Seen = time.Now().Add(-2 * lobbyStale)
	}
	l.mu.Unlock()

	rooms := l.Rooms("")
	if len(rooms[1].Players) != 0 {
		t.Fatalf("a room still holds a match nobody is playing: %+v", rooms[1])
	}
	if len(wiped) != 1 || wiped[0] != 2 {
		t.Fatalf("the room's table was not wiped: %v", wiped)
	}

	// And one of them coming back finds a lobby, not a ghost of their match.
	p := l.Join("a", "ann")
	if p.Match != "" || p.Room != 0 {
		t.Fatalf("a returning player was put back into a dead match: %+v", p)
	}
}

// A decided match is over: five sunk results from one side is a fleet gone,
// and the relay counts them rather than being told, because two boards that
// have both stopped talking never would tell it.
func TestHubKnowsWhenAFleetIsGone(t *testing.T) {
	h := NewHub(false)
	send := func(src PeerID, res uint8) {
		if err := h.Send(Packet{V: Version, Src: src, Dst: PeerB, T: TResult, A: res}); err != nil {
			t.Fatalf("send: %v", err)
		}
	}
	sink := func(src PeerID, ship uint8) {
		if err := h.Send(Packet{V: Version, Src: src, Dst: PeerB, T: TResult,
			A: ResSunk, B: ship}); err != nil {
			t.Fatalf("send: %v", err)
		}
	}
	for i := uint8(0); i < ShipCount-1; i++ {
		sink(PeerA, i)
	}
	send(PeerA, ResHit)
	sink(PeerB, 0)
	if h.Finished() {
		t.Fatal("four sunk ships and a hit is not a finished match")
	}
	// The same ship announced sunk again is a resend, not a fifth ship: the
	// core deliberately re-answers a duplicate shot, and counting answers
	// rather than ships ended live matches.
	sink(PeerA, 0)
	sink(PeerA, 1)
	if h.Finished() {
		t.Fatal("the same ships re-announced is not a fleet gone")
	}
	sink(PeerA, ShipCount-1)
	if !h.Finished() {
		t.Fatal("five distinct sunk ships from one side is a fleet gone")
	}
}

// A name is a label the player chooses, and it is written verbatim into a
// text protocol a board parses with sscanf. A name that can contain the
// protocol's own separators is a name that can forge a line of it.
func TestNamesCannotForgeProtocolLines(t *testing.T) {
	l := NewLobby()
	p := l.Join("a", "x\nmatched b black deadbeefdeadbeef")
	if strings.ContainsAny(p.Name, "\n\r|") {
		t.Fatalf("a name kept a separator: %q", p.Name)
	}
	if len(p.Name) > LOBBY_NAME_MAX {
		t.Fatalf("a name longer than a panel line holds survived: %q", p.Name)
	}
	// Bytes, not runes: a board reads a room line into a fixed buffer, and
	// sixteen runes of emoji is four times what fits.
	if wide := l.Join("wide", strings.Repeat("\u4e2d", 16)); len(wide.Name) > LOBBY_NAME_MAX {
		t.Fatalf("a multi-byte name overran the cap: %q is %d bytes", wide.Name, len(wide.Name))
	}
	if q := l.Join("b", "nirvek|esp-red"); strings.Contains(q.Name, "|") {
		t.Fatalf("a name kept the field separator: %q", q.Name)
	}
}

// A room with a match running in it is full, whoever is free to pair. Before
// this, a third player joining such a room paired with someone already in a
// match -- reassigning their match and leaving their opponent polling a table
// nobody would answer.
func TestARoomNeverPairsSomeoneAlreadyPlaying(t *testing.T) {
	l := NewLobby()
	for _, id := range []string{"a", "b", "c"} {
		l.Join(id, id)
	}
	// a and b are paired in the open lobby, and a is also sitting in room 1.
	if _, err := l.JoinRoom("a", 1); err != nil {
		t.Fatalf("join: %v", err)
	}
	l.Invite("a", "b")
	if m := l.Invite("b", "a"); m == nil {
		t.Fatal("the two should have paired")
	}
	// Pairing in the open lobby gives up the room they were waiting in: they
	// are not waiting there any more, and a row that keeps showing them is a
	// row nobody else can have.
	if p, _ := l.Player("a"); p.Room != 0 {
		t.Fatalf("a player paired elsewhere still holds a room: %+v", p)
	}
	// So the room is free -- and c sitting down in it must not be paired with
	// somebody who is already in a match.
	m, err := l.JoinRoom("c", 1)
	if err != nil {
		t.Fatalf("an empty room refused a player: %v", err)
	}
	if m != nil {
		t.Fatal("c was paired with a player who is already in a match")
	}
	if p, _ := l.Player("a"); p.Match == "" {
		t.Fatal("the player in a match was pulled out of it")
	}
}

// One player walking out does not turn the other one out too. The lobby goes
// back to 1/2 with the name of whoever is still standing in it, and the next
// person to join that lobby plays them.
func TestLeavingLeavesTheOtherPlayerInTheLobby(t *testing.T) {
	l := NewLobby()
	l.Join("a", "ann")
	l.Join("b", "bo")
	if _, err := l.JoinRoom("a", 2); err != nil {
		t.Fatalf("join: %v", err)
	}
	if m, _ := l.JoinRoom("b", 2); m == nil {
		t.Fatal("the second player should have started the match")
	}
	l.LeaveRoom("a")

	if p, _ := l.Player("a"); p.Room != 0 || p.Match != "" {
		t.Fatalf("the player who left is still in the lobby: %+v", p)
	}
	p, _ := l.Player("b")
	if p.Room != 2 || p.Match != "" {
		t.Fatalf("the player left behind lost their lobby: %+v", p)
	}
	room := l.Rooms("")[1]
	if len(room.Players) != 1 || room.Players[0] != "bo" {
		t.Fatalf("the lobby should show the player still in it: %+v", room)
	}
	// And it can be joined again, which is the point of showing them.
	l.Join("c", "cy")
	if m, err := l.JoinRoom("c", 2); err != nil || m == nil {
		t.Fatalf("the next player should pair with the one left behind: %v", err)
	}
}

// Restarting puts a player on the lobby list, not back in their match. The
// match is still theirs and the lobby says so, but they walk back into it by
// choosing that lobby -- which is the whole of what "reconnect" means here.
func TestARestartedPlayerMustChooseTheirLobbyAgain(t *testing.T) {
	l := NewLobby()
	l.Join("a", "ann")
	l.Join("b", "bo")
	if _, err := l.JoinRoom("a", 3); err != nil {
		t.Fatalf("join: %v", err)
	}
	if m, _ := l.JoinRoom("b", 3); m == nil {
		t.Fatal("the two should have paired")
	}
	if !l.Attached("a") {
		t.Fatal("a player who just started a match is in it")
	}

	// The page reloads: same UUID, new connection.
	l.Join("a", "ann")
	if l.Attached("a") {
		t.Fatal("a returning player was dropped straight back into their match")
	}
	if m := l.MatchOf("a"); m == nil {
		t.Fatal("their match should still be waiting for them")
	}
	// And their lobby is marked, so there is a row to press.
	if got := l.Rooms("a")[2]; !got.Yours {
		t.Fatalf("the lobby holding their match is not marked as theirs: %+v", got)
	}
	// Choosing a different lobby is not a way back in.
	if _, err := l.JoinRoom("a", 4); err == nil {
		t.Fatal("a player walked into the wrong lobby and was let in")
	}
	if _, err := l.JoinRoom("a", 3); err != nil {
		t.Fatalf("choosing their own lobby should let them back in: %v", err)
	}
	if !l.Attached("a") {
		t.Fatal("choosing their lobby did not put them back in the match")
	}
}

// Ids that wrap around start naming two matches at once, and MatchOf then
// hands a player the other one's seats and tokens. A relay left running gets
// there: the old two-character name had 260 of them.
func TestMatchIdsDoNotRepeat(t *testing.T) {
	seen := map[string]bool{}
	for i := 1; i <= 600; i++ {
		name := matchName(i)
		if seen[name] {
			t.Fatalf("match id %q was handed out twice, at %d", name, i)
		}
		seen[name] = true
	}
}

// Ending a match that was never played in a numbered lobby must not wipe the
// base table: that is where a board pointed at a bare relay sits, and where
// the CLI and the ESP-NOW path live.
func TestEndingARoomlessMatchLeavesTheBaseTableAlone(t *testing.T) {
	tables := NewTables(NewHub(false), NewSeats(), false)
	token := tables.Base().Seats.Assign(PeerA, "a board")
	tables.Reset(0)
	if _, _, err := tables.Base().Seats.Resolve(token); err != nil {
		t.Fatal("resetting room 0 evicted the base table's client")
	}
	// A numbered room still resets, which is the whole point of the call.
	room := tables.Room(2)
	roomToken := tables.SeatsOf(room).Assign(PeerA, "a player")
	tables.Reset(2)
	if _, _, err := tables.SeatsOf(room).Resolve(roomToken); err == nil {
		t.Fatal("resetting a room left its old seats valid")
	}
}

// A match that has not started has no board to hand back. Answering with the
// one packet that is true of every fresh match -- somebody has the move --
// told both clients they had reconnected to a game in progress, so neither
// went on to lay out a fleet and the match never began.
func TestAFreshBoardHasNothingToHandBack(t *testing.T) {
	b := NewBoard()
	if got := b.Frames(PeerA); len(got) != 0 {
		t.Fatalf("a board nobody has played on offered %d frames: %+v", len(got), got)
	}

	// One ship placed is a board worth restoring.
	b.Note(Packet{V: Version, Src: PeerA, Dst: PeerB, T: TFleet, A: 0,
		B: PackPlacement(0, 0, false)})
	frames := b.Frames(PeerA)
	if len(frames) == 0 {
		t.Fatal("a board with a fleet on it handed back nothing")
	}
	// And it says whose move it is, which is what a returning player cannot
	// work out for themselves.
	turn := false
	for _, p := range frames {
		if p.T == TTurn {
			turn = true
		}
	}
	if !turn {
		t.Fatal("the board came back without saying whose move it is")
	}
}

// Playing is not something the lobby can see by itself: a player in a match
// talks to their table and never asks for the lobby list again. Their record
// went stale while they played, so the reaper cleared the game out from under
// them forty-five seconds in.
func TestPlayingKeepsAPlayerAlive(t *testing.T) {
	l := NewLobby()
	l.Join("a", "ann")
	l.Join("b", "bo")
	if _, err := l.JoinRoom("a", 1); err != nil {
		t.Fatalf("join: %v", err)
	}
	if m, _ := l.JoinRoom("b", 1); m == nil {
		t.Fatal("the two should have paired")
	}

	// Time passes; neither of them has asked for the lobby list since.
	l.mu.Lock()
	for _, p := range l.players {
		p.Seen = time.Now().Add(-2 * lobbyStale)
	}
	l.mu.Unlock()

	// But they are both playing, which is what the relay hears on their table.
	l.TouchSeat(1, "a")
	l.TouchSeat(1, "b")

	if rooms := l.Rooms(""); len(rooms[0].Players) != 2 {
		t.Fatalf("a match in progress was reaped: %+v", rooms[0])
	}
	if l.MatchOf("a") == nil || l.MatchOf("b") == nil {
		t.Fatal("the players lost the match they were playing")
	}
}

// A finished match's table lingers so both players can read the verdict, and
// the lobby is free during those seconds -- so a new pair can sit down before
// the old board is cleared. A game must therefore begin on a clean table
// rather than inherit whatever is still on it.
func TestANewMatchStartsOnACleanTable(t *testing.T) {
	tables := NewTables(NewHub(false), NewSeats(), false)
	room := tables.Room(2)

	// The previous game left a log and a board behind.
	if err := room.Hub.Send(Packet{V: Version, Src: PeerA, Dst: PeerB, T: TShot, A: 1, B: 1}); err != nil {
		t.Fatalf("send: %v", err)
	}
	tables.BoardOf(room).Note(Packet{V: Version, Src: PeerA, Dst: PeerB, T: TFleet,
		A: 0, B: PackPlacement(0, 0, false)})
	stale := tables.SeatsOf(room).Assign(PeerA, "whoever was here")

	l := NewLobby()
	l.OnMatchEnd(func(int) {})
	l.Join("a", "ann")
	l.Join("b", "bo")
	if _, err := l.JoinRoom("a", 2); err != nil {
		t.Fatalf("join: %v", err)
	}
	m, _ := l.JoinRoom("b", 2)
	if m == nil {
		t.Fatal("the two should have paired")
	}
	startMatch(tables, tables.Room(2), l, m)

	fresh := tables.Room(2)
	if got := fresh.Hub.Status().Packets; got != 2 { // the two start packets
		t.Fatalf("the new match inherited %d packets from the last one", got-2)
	}
	if len(tables.BoardOf(fresh).Frames(PeerA)) != 0 {
		t.Fatal("the new match inherited a board")
	}
	if _, _, err := tables.SeatsOf(fresh).Resolve(stale); err == nil {
		t.Fatal("a seat from the last match still works in the new one")
	}
}
