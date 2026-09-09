package main

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"
)

func shot(col, row uint8) Packet {
	return Packet{V: Version, Seq: 1, Src: PeerA, Dst: PeerB, T: TShot, A: col, B: row}
}

func TestValidate(t *testing.T) {
	tests := []struct {
		name string
		p    Packet
		want error
	}{
		{"a legal shot", shot(0, 0), nil},
		{"the far corner", shot(9, 9), nil},
		{"off the board by one column", shot(10, 0), ErrArg},
		{"off the board by one row", shot(0, 10), ErrArg},
		{"wrong version", Packet{V: 99, T: TShot, Src: PeerA}, ErrVersion},
		{"unknown type", Packet{V: Version, T: Type(200), Src: PeerA}, ErrType},
		{"broadcast as a source", Packet{V: Version, T: TReady, Src: PeerAll}, ErrSrc},
		{"result that is not miss/hit/sunk",
			Packet{V: Version, Src: PeerB, T: TResult, A: 7}, ErrArg},
		{"result naming a sixth ship",
			Packet{V: Version, Src: PeerB, T: TResult, A: ResHit, B: 5}, ErrArg},
		{"a miss that names a ship",
			Packet{V: Version, Src: PeerB, T: TResult, A: ResMiss, B: 2}, ErrArg},
		{"a sunk carrier", Packet{V: Version, Src: PeerB, T: TResult, A: ResSunk, B: 0}, nil},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			err := tc.p.Validate()
			if tc.want == nil && err != nil {
				t.Fatalf("want ok, got %v", err)
			}
			if tc.want != nil && !errors.Is(err, tc.want) {
				t.Fatalf("want %v, got %v", tc.want, err)
			}
		})
	}
}

// The binary form is what ESP-NOW will carry, so it has to survive the round
// trip exactly -- including the little-endian sequence number, which is the
// only multi-byte field and so the only one that can be laid out wrongly.
func TestWireRoundTrip(t *testing.T) {
	in := Packet{V: Version, Seq: 0x1234, Src: PeerA, Dst: PeerB, T: TResult, A: ResSunk, B: 4}
	b := in.MarshalWire()
	if len(b) != WireSize {
		t.Fatalf("wire form is %d bytes, want %d", len(b), WireSize)
	}
	if b[4] != 0x34 || b[5] != 0x12 {
		t.Fatalf("seq is not little-endian: %#x %#x", b[4], b[5])
	}
	out, err := UnmarshalWire(b[:])
	if err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	if out != in {
		t.Fatalf("round trip changed the packet:\n got %+v\nwant %+v", out, in)
	}
}

func TestWireRejectsWrongLength(t *testing.T) {
	if _, err := UnmarshalWire([]byte{1, 2, 3}); !errors.Is(err, ErrShort) {
		t.Fatalf("want ErrShort, got %v", err)
	}
}

func TestJSONRejectsBadPacket(t *testing.T) {
	if _, err := DecodeJSON([]byte(`{"v":1,"src":1,"t":3,"a":99,"b":0}`)); !errors.Is(err, ErrArg) {
		t.Fatalf("want ErrArg for a shot off the board, got %v", err)
	}
}

func TestHubRoutesToTheAddressee(t *testing.T) {
	h := NewHub(false)
	if err := h.Send(shot(3, 4)); err != nil { // A -> B
		t.Fatalf("send: %v", err)
	}

	ctx := context.Background()
	got, next := h.Recv(ctx, PeerB, 0, time.Second)
	if len(got) != 1 || got[0].A != 3 || got[0].B != 4 {
		t.Fatalf("B should have received the shot, got %v", got)
	}
	if next != 1 {
		t.Fatalf("cursor should advance to 1, got %d", next)
	}

	// The sender must not read its own packet back, or a client would act on
	// its own shot as though the opponent had fired it.
	if own, _ := h.Recv(ctx, PeerA, 0, 10*time.Millisecond); len(own) != 0 {
		t.Fatalf("A read back its own packet: %v", own)
	}
}

func TestHubBroadcastReachesEveryoneElse(t *testing.T) {
	h := NewHub(false)
	_ = h.Send(Packet{V: Version, Seq: 1, Src: PeerA, Dst: PeerAll, T: TReset})
	for _, peer := range []PeerID{PeerB, PeerServer} {
		got, _ := h.Recv(context.Background(), peer, 0, 100*time.Millisecond)
		if len(got) != 1 {
			t.Fatalf("peer %s missed the broadcast", peerName(peer))
		}
	}
}

// The flag that reserves this for the future ESP-NOW change: with it set the
// relay must refuse gameplay outright, so a half-migrated setup fails loudly
// instead of quietly running two copies of the match.
func TestDirectLinkRefusesGameplayButKeepsBookkeeping(t *testing.T) {
	h := NewHub(true)

	for _, p := range []Packet{
		shot(1, 1),
		{V: Version, Seq: 2, Src: PeerB, Dst: PeerA, T: TResult, A: ResHit, B: 1},
	} {
		if err := h.Send(p); !errors.Is(err, ErrDirectLink) {
			t.Fatalf("%s should be refused in direct-link mode, got %v", p.T, err)
		}
	}

	for _, p := range []Packet{
		{V: Version, Seq: 3, Src: PeerA, Dst: PeerAll, T: TJoin},
		{V: Version, Seq: 4, Src: PeerA, Dst: PeerAll, T: TReady},
		{V: Version, Seq: 5, Src: PeerA, Dst: PeerAll, T: TReset},
	} {
		if err := h.Send(p); err != nil {
			t.Fatalf("%s should still be accepted in direct-link mode, got %v", p.T, err)
		}
	}
	if s := h.Status(); !s.DirectLink || s.Packets != 3 {
		t.Fatalf("status should show direct link and 3 kept packets, got %+v", s)
	}
}

// A long poll must return as soon as a packet lands, not when it times out.
func TestRecvWakesOnSend(t *testing.T) {
	h := NewHub(false)
	go func() {
		time.Sleep(30 * time.Millisecond)
		_ = h.Send(shot(5, 5))
	}()

	start := time.Now()
	got, _ := h.Recv(context.Background(), PeerB, 0, 5*time.Second)
	if len(got) != 1 {
		t.Fatalf("poll returned %d packets", len(got))
	}
	if elapsed := time.Since(start); elapsed > time.Second {
		t.Fatalf("poll waited %v, so it timed out rather than being woken", elapsed)
	}
}

func TestRecvReturnsEmptyOnTimeout(t *testing.T) {
	h := NewHub(false)
	got, next := h.Recv(context.Background(), PeerB, 0, 20*time.Millisecond)
	if len(got) != 0 || next != 0 {
		t.Fatalf("want an empty answer at cursor 0, got %v / %d", got, next)
	}
}

func TestResetClearsHistory(t *testing.T) {
	h := NewHub(false)
	_ = h.Send(shot(1, 2))
	h.Reset()
	if s := h.Status(); s.Packets != 0 {
		t.Fatalf("reset left %d packets", s.Packets)
	}
}

func TestSeatsHandOutTwoSeatsThenRefuse(t *testing.T) {
	s := NewSeats()
	a, tokA, _, err := s.Join("browser")
	if err != nil || a != PeerA {
		t.Fatalf("first join should be seat A, got %v %v", a, err)
	}
	b, tokB, _, err := s.Join("esp32")
	if err != nil || b != PeerB {
		t.Fatalf("second join should be seat B, got %v %v", b, err)
	}
	if tokA == tokB {
		t.Fatal("two seats were given the same token")
	}
	if _, _, _, err := s.Join("gatecrasher"); !errors.Is(err, ErrNoSeats) {
		t.Fatalf("a third join should be refused, got %v", err)
	}

	// The name a client gave itself is what the terminal prints.
	if s.Name(PeerB) != "esp32" {
		t.Fatalf("seat B should be named esp32, got %q", s.Name(PeerB))
	}
}

func TestSeatsResolveAndLeave(t *testing.T) {
	s := NewSeats()
	id, tok, _, _ := s.Join("browser")

	got, name, err := s.Resolve(tok)
	if err != nil || got != id || name != "browser" {
		t.Fatalf("resolve gave %v %q %v", got, name, err)
	}
	if _, _, err := s.Resolve("not-a-token"); !errors.Is(err, ErrBadToken) {
		t.Fatalf("an unknown token must be rejected, got %v", err)
	}
	if _, _, err := s.Resolve(""); !errors.Is(err, ErrNeedsToken) {
		t.Fatalf("a missing token must say so, got %v", err)
	}

	// Leaving frees the seat for the next client.
	s.Leave(tok)
	if again, _, _, _ := s.Join("someone else"); again != PeerA {
		t.Fatalf("seat A should be free after leaving, got %v", again)
	}
}

func TestCellNameMatchesThePanel(t *testing.T) {
	// The panel letters rows down the side and numbers columns across, and
	// the aim readout puts the letter first: col 5, row 0 reads A5.
	for _, tc := range []struct {
		col, row uint8
		want     string
	}{{0, 0, "A0"}, {5, 0, "A5"}, {9, 9, "J9"}, {3, 2, "C3"}} {
		if got := CellName(tc.col, tc.row); got != tc.want {
			t.Fatalf("CellName(%d,%d) = %q, want %q", tc.col, tc.row, got, tc.want)
		}
	}
}

func TestDescribeReadsAsEnglish(t *testing.T) {
	for _, tc := range []struct {
		p    Packet
		want string
	}{
		{Packet{T: TShot, A: 5, B: 0}, "shot   A5"},
		{Packet{T: TResult, A: ResMiss}, "result MISS"},
		{Packet{T: TResult, A: ResSunk, B: 0}, "result SUNK CARRIER"},
		{Packet{T: TReady}, "ready  fleet placed"},
	} {
		if got := tc.p.Describe(); got != tc.want {
			t.Fatalf("Describe() = %q, want %q", got, tc.want)
		}
	}
}

func TestFleetPacketValidation(t *testing.T) {
	fleet := func(ship uint8, col, row int, vertical bool) Packet {
		b := uint8((col + row*BoardSize) * 2)
		if vertical {
			b++
		}
		return Packet{V: Version, Seq: 1, Src: PeerA, Dst: PeerB, T: TFleet, A: ship, B: b}
	}
	tests := []struct {
		name string
		p    Packet
		want error
	}{
		{"carrier down column 0", fleet(0, 0, 0, true), nil},
		{"carrier across from the last legal column", fleet(0, 5, 0, false), nil},
		{"destroyer in the far corner", fleet(4, 9, 8, true), nil},
		{"a carrier that runs off the right", fleet(0, 6, 0, false), ErrArg},
		{"a carrier that runs off the bottom", fleet(0, 0, 6, true), ErrArg},
		{"a sixth ship", fleet(5, 0, 0, true), ErrArg},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			err := tc.p.Validate()
			if tc.want == nil && err != nil {
				t.Fatalf("want ok, got %v", err)
			}
			if tc.want != nil && !errors.Is(err, tc.want) {
				t.Fatalf("want %v, got %v", tc.want, err)
			}
		})
	}
}

// The packing has to agree with game::packPlacement() in netplay.h, since the
// two ends of the wire are the two implementations of it.
func TestPlacementPackingRoundTrip(t *testing.T) {
	for row := 0; row < BoardSize; row++ {
		for col := 0; col < BoardSize; col++ {
			for _, vert := range []bool{false, true} {
				b := uint8((col + row*BoardSize) * 2)
				if vert {
					b++
				}
				gotCol, gotRow, gotVert := UnpackPlacement(b)
				if gotCol != col || gotRow != row || gotVert != vert {
					t.Fatalf("(%d,%d,%v) packed to %d and came back (%d,%d,%v)",
						col, row, vert, b, gotCol, gotRow, gotVert)
				}
			}
		}
	}
}

func TestFleetDescribe(t *testing.T) {
	p := Packet{T: TFleet, A: 2, B: uint8((3 + 4*BoardSize) * 2)}
	if got, want := p.Describe(), "fleet  CRUISER at E3 across, ship #3 of 5, 3 long"; got != want {
		t.Fatalf("Describe() = %q, want %q", got, want)
	}
}

// Every ship, so the number and the length cannot drift apart from the name
// they are printed beside: ship #N is the Nth name and the Nth length.
func TestFleetDescribeNumbersAndLengths(t *testing.T) {
	want := []string{
		"fleet  CARRIER at A0 across, ship #1 of 5, 5 long",
		"fleet  BATTLESHIP at A0 across, ship #2 of 5, 4 long",
		"fleet  CRUISER at A0 across, ship #3 of 5, 3 long",
		"fleet  SUBMARINE at A0 across, ship #4 of 5, 3 long",
		"fleet  DESTROYER at A0 across, ship #5 of 5, 2 long",
	}
	for i := range shipNames {
		p := Packet{T: TFleet, A: uint8(i), B: 0}
		if got := p.Describe(); got != want[i] {
			t.Errorf("ship %d: Describe() = %q, want %q", i, got, want[i])
		}
	}
}

// A vertical placement says so, and the server handing a player their own
// fleet back describes it exactly as the original packet did.
func TestFleetDescribeVerticalAndMyFleet(t *testing.T) {
	down := Packet{T: TFleet, A: 0, B: uint8((3+4*BoardSize)*2 + 1)}
	if got, want := down.Describe(), "fleet  CARRIER at E3 down, ship #1 of 5, 5 long"; got != want {
		t.Fatalf("Describe() = %q, want %q", got, want)
	}
	mine := Packet{T: TMyFleet, A: 0, B: uint8((3+4*BoardSize)*2 + 1)}
	if got, want := mine.Describe(), "myfleet CARRIER at E3 down, ship #1 of 5, 5 long"; got != want {
		t.Fatalf("Describe() = %q, want %q", got, want)
	}
}

// Reflashing the board, or reloading a tab, must not be locked out by the
// session it just replaced -- that seat is held for another 45 seconds and is
// held by the very client now asking for it.
func TestJoinTakesBackItsOwnSeat(t *testing.T) {
	s := NewSeats()
	id, tok, _, _ := s.Join("esp32")
	other, _, _, _ := s.Join("browser-1a2b")
	if id == other {
		t.Fatal("two clients were given the same seat")
	}

	again, tok2, _, err := s.Join("esp32")
	if err != nil {
		t.Fatalf("a client must be able to take back its own seat, got %v", err)
	}
	if again != id {
		t.Fatalf("it should get the same seat: had %v, got %v", id, again)
	}
	if tok2 == tok {
		t.Fatal("the token must be reissued, or the dead session keeps working")
	}
	if _, _, err := s.Resolve(tok); !errors.Is(err, ErrBadToken) {
		t.Fatalf("the old token must stop working, got %v", err)
	}

	// And a genuine third client is still turned away.
	if _, _, _, err := s.Join("someone-else"); !errors.Is(err, ErrNoSeats) {
		t.Fatalf("a third client should still be refused, got %v", err)
	}
}

func TestHoldersNamesWhoIsInTheWay(t *testing.T) {
	s := NewSeats()
	s.Join("esp32")
	s.Join("browser-9f")
	h := s.Holders()
	if !strings.Contains(h, "esp32") || !strings.Contains(h, "browser-9f") {
		t.Fatalf("Holders() should name both seats, got %q", h)
	}
}

// Renaming the board strands its old seat: the new name no longer matches, so
// the take-your-own-seat-back rule cannot reclaim it. Dropping by name frees
// exactly that one, leaving the other player alone -- which clearing both
// seats would not.
func TestDropFreesOneSeatByName(t *testing.T) {
	s := NewSeats()
	oldBoard, _, _, _ := s.Join("esp32")
	s.Join("browser-77aa")

	if _, _, _, err := s.Join("esp-red"); !errors.Is(err, ErrNoSeats) {
		t.Fatalf("the renamed board should be locked out first, got %v", err)
	}

	id, found := s.Drop("esp32")
	if !found || id != oldBoard {
		t.Fatalf("Drop should free the old board's seat, got %v/%v", id, found)
	}
	if _, _, ok := func() (PeerID, string, bool) {
		p, _, _, err := s.Join("esp-red")
		return p, "", err == nil
	}(); !ok {
		t.Fatal("the renamed board should get in once its old seat is freed")
	}

	// And the other player was left where they were.
	holders := s.Holders()
	if !strings.Contains(holders, "browser-77aa") {
		t.Fatalf("the other player should keep their seat, holders = %q", holders)
	}

	if _, found := s.Drop("nobody-by-that-name"); found {
		t.Fatal("dropping a name nobody holds should report nothing found")
	}
}

// ":8080" is every interface, which is the opposite of loopback rather than a
// special case of it. Reading the empty host as loopback made the relay warn
// that no board could reach an address every board could reach.
func TestLoopbackWarningReadsTheAddressRight(t *testing.T) {
	for _, tc := range []struct {
		addr string
		warn bool
	}{
		{"localhost:8080", true},
		{"127.0.0.1:8080", true},
		{":8080", false},
		{"0.0.0.0:8080", false},
		{"192.168.86.220:8080", false},
	} {
		if got := looksLikeLoopback(tc.addr); got != tc.warn {
			t.Fatalf("looksLikeLoopback(%q) = %v, want %v", tc.addr, got, tc.warn)
		}
	}
}
