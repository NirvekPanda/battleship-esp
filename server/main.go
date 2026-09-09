// Command relay is a local match server for battleship-esp.
//
// It does two jobs at once, which is why it exists at all: it serves the WASM
// page, and it routes packets between whoever is playing -- the browser, the
// laptop, and an ESP32 on the same network. Serving the page from the same
// origin means the browser can talk to the relay with no CORS dance and no
// second port to remember.
//
// It holds no game rules. The rules live in src/game/, compiled into both the
// firmware and the WASM build; this process only moves validated packets
// between peers and remembers what it has seen.
package main

import (
	"context"
	"crypto/subtle"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"syscall"
	"time"
)

const (
	// A long poll parks for this long before answering with nothing, which
	// keeps a connection well inside any proxy or browser idle timeout.
	pollWait = 25 * time.Second
	// WriteTimeout must outlast a parked poll or the server would cut off its
	// own long polls. This is the one timeout that cannot just be "short".
	writeTimeout = pollWait + 10*time.Second
)

func main() {
	// Bound to every interface by default. A relay nobody else can reach is a
	// relay with one player on it: the boards are on the WiFi, phones are on
	// the WiFi, and every one of them was answered with a connection reset
	// until somebody remembered ADDR=0.0.0.0. Pass -addr localhost:8080 to put
	// it back on loopback.
	addr := flag.String("addr", "0.0.0.0:8080",
		"address to listen on; 0.0.0.0 lets anyone on the WiFi reach it, localhost keeps it to this machine")
	webDir := flag.String("web", "web", "directory holding index.html and the WASM build")
	dashDir := flag.String("dashboard", "dashboard", "directory holding the dashboard")
	dashAddr := flag.String("dashboard-addr", "0.0.0.0:8081",
		"where to serve the dashboard; empty to serve it only through nginx")
	quiet := flag.Bool("quiet", false, "do not print each packet as it passes")
	adminToken := flag.String("admin-token", "",
		"token required by the control endpoints; generated and printed if unset")
	espNow := flag.Bool("esp-now", false,
		"the two boards carry gameplay directly over ESP-NOW; the relay then serves the page and "+
			"observes, and refuses to also relay shots (not implemented on the firmware yet)")
	flag.Parse()

	// The control endpoints are the ones that can ruin a match in progress:
	// wipe its history, evict both players, march their screens elsewhere.
	// They were open to anyone who could reach the port, which was defensible
	// on loopback and is not once a tunnel is in front -- and being plain
	// POSTs with no custom header, they need no CORS preflight either, so any
	// page in any tab could fire them at the public hostname.
	if *adminToken == "" {
		*adminToken = newToken()
		log.Printf("admin  token %s", *adminToken)
		log.Printf("       paste it into the dashboard; set -admin-token to fix it")
	}
	isAdmin := func(r *http.Request) bool {
		return subtle.ConstantTimeCompare(
			[]byte(r.Header.Get("X-Admin-Token")), []byte(*adminToken)) == 1
	}

	requireAdmin := func(next http.HandlerFunc) http.HandlerFunc {
		return func(w http.ResponseWriter, r *http.Request) {
			// Compared with constant time: these are short-lived local tokens
			// rather than passwords, but a comparison that returns early is a
			// habit worth not having.
			if !isAdmin(r) {
				httpError(w, http.StatusUnauthorized,
					errors.New("admin: send X-Admin-Token; the relay printed it at startup"))
				return
			}
			next(w, r)
		}
	}

	hub := NewHub(*espNow)
	seats := NewSeats()
	// One Hub and one Seats per numbered room, so five matches can be in
	// flight at once; the pair above stays as the base table for the CLI, the
	// dashboard and a board that plays without rooms.
	tables := NewTables(hub, seats, *espNow)
	lobby := NewLobby()
	// A match that is over -- decided, abandoned, or simply left by both
	// players -- takes its packet log and its seats with it. Everything the
	// match consisted of lives in that log, so without this the next pair in
	// the room would poll their way through the last game's fleets and shots.
	//
	// But not this instant. A match is decided by the packet that sinks the
	// last ship, and the relay notices while carrying it -- before the player
	// who fired it has polled for the answer. Wiping the table there took the
	// deciding result away with it, so the winner sat on the board waiting for
	// a reply that no longer existed and never saw their own WIN. The lobby is
	// freed at once; the TABLE lingers long enough for both sides to collect
	// the end of the game and read the verdict.
	lobby.OnMatchEnd(func(room int) {
		label := tables.Room(room).Label()
		logs.Columns(label, "lobby", "server",
			"match over -- the lobby is open again; clearing the board shortly")
		time.AfterFunc(matchLinger, func() {
			// Unless somebody has started a new game in it meanwhile, which
			// brings its own board with it.
			if lobby.MatchInRoom(room) != nil {
				return
			}
			tables.Reset(room)
			logs.Columns(label, "lobby", "server", "board cleared")
		})
	})
	mux := http.NewServeMux()

	// A seat is claimed here and held with the token this returns. Before
	// this existed a client simply asserted which player it was, so nothing
	// stopped two browsers both calling themselves A -- or one of them
	// answering its own shots as though it were the opponent.
	mux.HandleFunc("POST /v1/join", func(w http.ResponseWriter, r *http.Request) {
		var body struct {
			Name string `json:"name"`
		}
		_ = json.NewDecoder(http.MaxBytesReader(w, r.Body, 1024)).Decode(&body)

		id, token, reclaimed, err := seats.Join(body.Name)
		if err != nil {
			// Say who is in the way, and how long they have been quiet: a
			// stale holder is reclaimed after 90s, but a live one never is,
			// and those need different actions from whoever is reading this.
			logs.Columns("main", "-", body.Name,
				fmt.Sprintf("REFUSED: %v -- held by %s", err, seats.Holders()))
			httpError(w, http.StatusConflict, err)
			return
		}
		if reclaimed {
			logs.Columns("main", "-", "server", "reclaimed a seat whose holder had gone quiet")
		}
		logs.Columns("main", hub.Phase(id), body.Name, "joined as seat "+peerName(id))
		// A board has no JSON parser worth the flash. "fmt=text" answers with
		// "<peer> <token>" and a newline, which sscanf reads in one line.
		if r.URL.Query().Get("fmt") == "text" {
			w.Header().Set("Content-Type", "text/plain")
			fmt.Fprintf(w, "%d %s\n", id, token)
			return
		}
		writeJSON(w, map[string]any{"peer": id, "name": peerName(id), "token": token})
	})

	mux.HandleFunc("POST /v1/leave", func(w http.ResponseWriter, r *http.Request) {
		// Also accepted in the query string: a page being closed can only say
		// goodbye with sendBeacon, which cannot set a header.
		token := r.Header.Get("X-Peer-Token")
		if token == "" {
			token = r.URL.Query().Get("token")
		}
		if tb, id, name, err := tables.Resolve(token); err == nil {
			logs.Columns(tb.Label(), tb.Hub.Phase(id), name, "left seat "+peerName(id))
			tables.SeatsOf(tb).Leave(token)
		}
		writeJSON(w, map[string]any{"ok": true})
	})

	// Go 1.22+ patterns carry the method, so a wrong verb is a 405 from the
	// mux rather than something each handler has to check.
	mux.HandleFunc("POST /v1/send", func(w http.ResponseWriter, r *http.Request) {
		// The token says which table this packet belongs on, which is what
		// keeps five matches from writing into one log: a player can only
		// ever reach the room their seat was issued in.
		tb, who, name, err := tables.Resolve(r.Header.Get("X-Peer-Token"))
		if err != nil {
			httpError(w, http.StatusUnauthorized, err)
			return
		}

		// Two encodings of the same packet: JSON from the browser, and the
		// fixed 8-byte wire form from a board, which is the same layout
		// ESP-NOW will carry. Sharing one decoder path would mean one of them
		// pretending to be the other.
		// Two encodings of the same packet: JSON from the browser, and the
		// fixed 8-byte wire form from a board -- the same layout ESP-NOW will
		// carry. A board may post several frames back to back in one request,
		// because handing over a fleet is six packets at once and six TCP
		// connections to say it would be six times the cost for no reason.
		var batch []Packet
		if strings.HasPrefix(r.Header.Get("Content-Type"), "application/octet-stream") {
			raw, err := io.ReadAll(http.MaxBytesReader(w, r.Body, WireSize*64))
			if err != nil {
				httpError(w, http.StatusBadRequest, err)
				return
			}
			if len(raw) == 0 || len(raw)%WireSize != 0 {
				httpError(w, http.StatusBadRequest,
					fmt.Errorf("%w: body must be a whole number of %d-byte frames, got %d",
						ErrShort, WireSize, len(raw)))
				return
			}
			for off := 0; off < len(raw); off += WireSize {
				p, err := UnmarshalWire(raw[off : off+WireSize])
				if err != nil {
					httpError(w, http.StatusBadRequest, err)
					return
				}
				batch = append(batch, p)
			}
		} else {
			// One packet, or several: the browser drains whatever the core
			// produced in a frame, and laying out a fleet produces six at
			// once. Sending them one request at a time let the connection
			// pool decide their order, and the fleet packets have to arrive
			// before the ready that follows them.
			raw, err := io.ReadAll(http.MaxBytesReader(w, r.Body, 8192))
			if err != nil {
				httpError(w, http.StatusBadRequest, err)
				return
			}
			trimmed := strings.TrimSpace(string(raw))
			if strings.HasPrefix(trimmed, "[") {
				if err := json.Unmarshal([]byte(trimmed), &batch); err != nil {
					httpError(w, http.StatusBadRequest, err)
					return
				}
				if len(batch) == 0 {
					httpError(w, http.StatusBadRequest,
						errors.New("send: an empty batch says nothing"))
					return
				}
			} else {
				var p Packet
				if err := json.Unmarshal([]byte(trimmed), &p); err != nil {
					httpError(w, http.StatusBadRequest, err)
					return
				}
				batch = append(batch, p)
			}
		}
		// The seat decides who you are, not the packet. A client that claims
		// to be the other player is refused rather than quietly corrected,
		// because a mismatch means the two ends disagree about who is who and
		// silently rewriting it would hide that.
		//
		// Checked for every frame before any of them is accepted, so a batch
		// is all-or-nothing: half a fleet landing is worse than none.
		for _, p := range batch {
			if p.Src != who {
				httpError(w, http.StatusForbidden, ErrWrongPeer)
				return
			}
			if err := p.Validate(); err != nil {
				httpError(w, http.StatusBadRequest, err)
				return
			}
		}

		// Still here. A player in a match talks to their table and never asks
		// for the lobby list again, so without this their lobby record went
		// stale while they played and the reaper cleared the game out from
		// under them.
		lobby.TouchSeat(tb.Room, peerName(who))

		// The board is folded in as the packets are carried, so it is the log
		// added up rather than a second account of the match that could
		// disagree with it.
		board := tables.BoardOf(tb)
		for _, p := range batch {
			if err := tb.Hub.Send(p); err != nil {
				// A refusal in direct-link mode is a rule, not a malformed
				// request, so it gets its own status: the client is not wrong,
				// it is talking to a server configured to stay out of the way.
				if errors.Is(err, ErrDirectLink) {
					httpError(w, http.StatusConflict, err)
					return
				}
				httpError(w, http.StatusBadRequest, err)
				return
			}
			board.Note(p)
			if !*quiet {
				// Columns: which screen, who, what. The name the client gave
				// itself rather than its seat letter -- "esp-red" says more
				// than "a" when two boards look alike.
				_ = name
				kind, cell, detail := p.Parts()
				logs.Packet(tb.Label(), tb.Hub.Phase(p.Src), tables.SeatsOf(tb).Name(p.Src), tables.SeatsOf(tb).Name(p.Dst),
					kind, cell, detail, int(p.Seq))
			}
		}
		// Decided? Then the room is done with. Checked here, after the packets
		// that could have decided it have been carried, so the last result
		// still reaches the loser before the seats are taken away.
		// The table is known by its room, not by who is sitting at it: the seat
		// name is a label and two players may share one.
		if tb.Room != 0 && tb.Hub.Finished() {
			if m := lobby.MatchInRoom(tb.Room); m != nil {
				lobby.EndMatch(m.ID)
			}
		}
		writeJSON(w, map[string]any{"ok": true})
	})

	mux.HandleFunc("GET /v1/recv", func(w http.ResponseWriter, r *http.Request) {
		// The poller is identified by its token too, so a client can only
		// ever read its own mail.
		tb, peer, _, err := tables.Resolve(r.Header.Get("X-Peer-Token"))
		if err != nil {
			httpError(w, http.StatusUnauthorized, err)
			return
		}
		lobby.TouchSeat(tb.Room, peerName(peer))
		since, _ := strconv.Atoi(r.URL.Query().Get("since"))

		// A browser can park on a long poll; a board cannot, because its main
		// loop also has to draw and read buttons. "wait" lets the caller say
		// how long it can afford to block, and 0 means answer at once.
		wait := pollWait
		if v := r.URL.Query().Get("wait"); v != "" {
			if ms, err := strconv.Atoi(v); err == nil {
				wait = time.Duration(ms) * time.Millisecond
				if wait > pollWait {
					wait = pollWait
				}
			}
		}

		// The request context cancels when the client hangs up, so a browser
		// that navigates away frees the parked poll immediately.
		packets, next := tb.Hub.Recv(r.Context(), peer, since, wait)
		if packets == nil {
			packets = []Packet{}
		}

		// The board reads packets as the same fixed 8-byte frames it sends,
		// so it needs no parser at all: the count is the length over eight.
		if r.URL.Query().Get("fmt") == "bin" {
			w.Header().Set("Content-Type", "application/octet-stream")
			w.Header().Set("X-Next", strconv.Itoa(next))
			for _, p := range packets {
				b := p.MarshalWire()
				_, _ = w.Write(b[:])
			}
			return
		}
		writeJSON(w, map[string]any{"packets": packets, "next": next})
	})

	mux.HandleFunc("GET /v1/status", func(w http.ResponseWriter, r *http.Request) {
		st := hub.Status()
		// Every table, not just the base one: with five rooms the base table
		// is usually the empty one, and a status that only described it would
		// report an idle relay with five games running on it.
		rooms := []map[string]any{}
		packets := 0
		for _, tb := range tables.All() {
			s := tb.Hub.Status()
			packets += s.Packets
			if tb.Room == 0 {
				continue
			}
			rooms = append(rooms, map[string]any{
				"room": tb.Room, "packets": s.Packets, "seats": tables.SeatsOf(tb).List(),
			})
		}
		writeJSON(w, map[string]any{
			"version": st.Version, "directLink": st.DirectLink,
			"packets": packets, "seats": seats.List(), "rooms": rooms,
		})
	})

	// The end-of-match reset. Clearing the log invalidates every client
	// cursor at once, which is precisely what a reset means.
	mux.HandleFunc("POST /v1/reset", requireAdmin(func(w http.ResponseWriter, r *http.Request) {
		logs.Columns("main", "-", "server", "match history cleared")
		for _, tb := range tables.All() {
			tb.Hub.Reset()
		}
		writeJSON(w, map[string]any{"ok": true})
	}))

	// Start the match over without breaking the link.
	//
	// The seats are deliberately left alone: both players stay connected, so
	// they land on the title screen and the handshake that follows finds the
	// other side already there and completes on the next poll. Clearing the
	// seats would work too, and would cost each of them a rejoin before they
	// could even begin -- which is the difference between starting again and
	// setting up again.
	//
	// Order matters: the history is cleared first, then the page packet is
	// sent, or the instruction to go to the title would be swept away with
	// everything else.
	mux.HandleFunc("POST /v1/restart", requireAdmin(func(w http.ResponseWriter, r *http.Request) {
		for _, tb := range tables.All() {
			tb.Hub.Reset()
		}
		p := Packet{V: Version, Src: PeerServer, Dst: PeerAll, T: TPage, A: 0} // start
		if err := sendEverywhere(tables, p); err != nil {
			httpError(w, http.StatusInternalServerError, err)
			return
		}
		kind, cell, detail := p.Parts()
		// A restart is every table at once, so it is a main-lobby event: the
		// alternative is the same line printed six times.
		logs.Packet("main", "start", "server", "all", kind, cell, detail, int(p.Seq))
		logs.Columns("main", "start", "server", "hard restart -- seats kept")
		writeJSON(w, map[string]any{"ok": true, "seats": len(seats.List())})
	}))

	// Free one named seat. Used when a board is reflashed under a new name and
	// its old seat would otherwise sit there until the idle timeout.
	mux.HandleFunc("POST /v1/seats/drop", requireAdmin(func(w http.ResponseWriter, r *http.Request) {
		name := r.URL.Query().Get("name")
		if name == "" {
			httpError(w, http.StatusBadRequest, errors.New("drop: name is required"))
			return
		}
		id, found := seats.Drop(name)
		if found {
			logs.Columns("main", "-", name, "seat "+peerName(id)+" dropped")
		}
		writeJSON(w, map[string]any{"dropped": found, "name": name})
	}))

	mux.HandleFunc("POST /v1/seats/clear", requireAdmin(func(w http.ResponseWriter, r *http.Request) {
		n := seats.Clear()
		logs.Columns("main", "-", "server",
			fmt.Sprintf("cleared %d seat(s); everyone rejoins on their next try", n))
		writeJSON(w, map[string]any{"cleared": n})
	}))

	// ---- dashboard ----

	// What the terminal is showing, so the dashboard can show the same thing.
	mux.HandleFunc("GET /v1/log", func(w http.ResponseWriter, r *http.Request) {
		since, _ := strconv.Atoi(r.URL.Query().Get("since"))
		lines, next := logs.Since(since)
		// A fleet line names the cell a ship sits on, and positions are the
		// only secret in this game. The log is the dashboard's whole point, so
		// it stays open -- but those two fields are held back from a reader
		// who has not proved they are the operator. Everything else about the
		// line, including that a fleet packet passed, is still there.
		if !isAdmin(r) {
			for i := range lines {
				switch lines[i].Kind {
				case "fleet", "myfleet":
					lines[i].Cell = "--"
					lines[i].Detail = "a ship, placed"
					lines[i].Text = ""
				}
			}
		}
		writeJSON(w, map[string]any{"lines": lines, "next": next})
	})

	// What the relay can tell about the match from the packets it has carried.
	// The table with a game on it, which is almost never the base one now:
	// matches are played in the numbered rooms, and a match panel describing
	// only room 0 showed an empty board while five games were being played.
	mux.HandleFunc("GET /v1/match", func(w http.ResponseWriter, r *http.Request) {
		// A room can be asked for by name, which is what the dashboard does
		// once an operator has picked one: five games at once means "the
		// match" is a question with five answers, and the operator chooses.
		tb := liveTable(tables)
		if v := r.URL.Query().Get("room"); v != "" {
			n, err := strconv.Atoi(v)
			if err != nil {
				httpError(w, http.StatusBadRequest, errors.New("match: ?room= must be a number"))
				return
			}
			tb = tables.Room(n)
		}
		m := tb.Hub.Match()
		writeJSON(w, map[string]any{
			"ready": m.Ready, "fleet": m.Fleet, "shots": m.Shots,
			"phase": m.Phase, "bothReady": tb.Hub.BothReady(),
			"seats": tables.SeatsOf(tb).List(), "room": tb.Room, "lobby": tb.Label(),
		})
	})

	// Send both players to a screen. Broadcast from the server rather than
	// from a player, and the firmware only obeys it from the server: one
	// player moving the other's screen would be a way to cheat.
	mux.HandleFunc("POST /v1/force-page", requireAdmin(func(w http.ResponseWriter, r *http.Request) {
		page, err := strconv.Atoi(r.URL.Query().Get("page"))
		if err != nil || page < 0 || page > 255 {
			httpError(w, http.StatusBadRequest, errors.New("force-page: page is required"))
			return
		}
		p := Packet{V: Version, Src: PeerServer, Dst: PeerAll, T: TPage, A: uint8(page)}
		if err := p.Validate(); err != nil {
			httpError(w, http.StatusBadRequest, err)
			return
		}
		// ONE lobby, named by the dashboard -- the one it is showing. Sending
		// to every table at once was worse than useless with five games
		// running: the guard below was evaluated against whichever table
		// happened to have both fleets down, and every other lobby's players,
		// including a pair still laying out ships, were marched to a match
		// screen with nothing on the board.
		target := liveTable(tables)
		if v := r.URL.Query().Get("room"); v != "" {
			n, err := strconv.Atoi(v)
			if err != nil {
				httpError(w, http.StatusBadRequest,
					errors.New("force-page: ?room= must be a number"))
				return
			}
			target = tables.Room(n)
		}

		// Refusing to send a lobby to a board with no ships on it is the one
		// piece of judgement here, and it belongs on the server: the dashboard
		// greys the button out, but a greyed button is a hint, not a rule.
		if pageNames[page] == "match" && !target.Hub.BothReady() {
			httpError(w, http.StatusConflict,
				errors.New("force-page: both fleets must be placed before the match screen"))
			return
		}
		if err := target.Hub.Send(p); err != nil {
			httpError(w, http.StatusBadRequest, err)
			return
		}
		// Logged as the packet it is, not as a remark about one. A page packet
		// is carried like any other, so it belongs in the same columns --
		// otherwise the one kind of traffic the operator caused is the one
		// kind they cannot see in the table.
		kind, cell, detail := p.Parts()
		logs.Packet(target.Label(), pageNames[page], "server", "all", kind, cell, detail, int(p.Seq))
		writeJSON(w, map[string]any{"ok": true, "page": pageNames[page]})
	}))

	// The whole match as a spreadsheet, timestamps included, for looking at
	// afterwards.
	//
	// Admin only: every fleet row in it carries a ship's position, and
	// positions are the only secret in this game. /v1/reconnect is behind a
	// seat token for exactly that reason, and a CSV of the same thing must
	// not be the way around it -- least of all now the relay listens on the
	// whole WiFi by default.
	mux.HandleFunc("GET /v1/packets.csv", requireAdmin(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/csv")
		w.Header().Set("Content-Disposition",
			fmt.Sprintf("attachment; filename=battleship-%s.csv",
				time.Now().Format("20060102-150405")))
		// The table with the game on it, or one named by ?room=. The base hub
		// is the CLI and ESP-NOW path; a CSV of it after a match played in a
		// lobby was a header row and nothing else.
		csvTable := liveTable(tables)
		if v := r.URL.Query().Get("room"); v != "" {
			if n, err := strconv.Atoi(v); err == nil {
				csvTable = tables.Room(n)
			}
		}
		if err := csvTable.Hub.CSV(w); err != nil {
			log.Printf("csv    %v", err)
		}
	}))

	// ---- lobby ----
	//
	// Identity is the UUID the client sends in X-Player-Id, not the seat it
	// happens to hold. Names may collide; UUIDs may not.
	playerID := func(r *http.Request) string { return r.Header.Get("X-Player-Id") }

	mux.HandleFunc("POST /v1/lobby/join", func(w http.ResponseWriter, r *http.Request) {
		id := playerID(r)
		if id == "" {
			httpError(w, http.StatusBadRequest, errors.New("lobby: send X-Player-Id"))
			return
		}
		var body struct {
			Name string `json:"name"`
		}
		_ = json.NewDecoder(http.MaxBytesReader(w, r.Body, 1024)).Decode(&body)

		p := lobby.Join(id, body.Name)

		// A returning player is NOT put back into their match here. Their
		// match is still theirs -- it is waiting for them, and the room they
		// were playing in says so -- but they rejoin it by choosing that
		// lobby from the list, the same way they chose it the first time.
		// Dropping someone straight back into a game they may not have meant
		// to return to, on a page they never asked for, is what "reconnect"
		// used to mean here; now it is a room you can walk back into.
		m := lobby.MatchOf(id)
		logs.Columns("main", "lobby", p.Name, "joined the lobby as "+p.Team)
		writeJSON(w, map[string]any{
			"player": p, "resume": m != nil, "match": m,
			"room": p.Room,
		})
	})

	// The board as it stands, for a player who has just walked back into
	// their lobby.
	//
	// Not a replay of the log: a result means "the shot you just fired", and
	// somebody who has restarted fired nothing, so the board is handed over
	// as marks on named cells (see TMark / TDamage / TTurn in packet.go).
	// One exchange, whatever the match has been through.
	//
	// Authenticated by the SEAT TOKEN, not by the player id in a header. The
	// id of every player in the lobby is public -- the list is how you choose
	// an opponent -- so a header-only check let anyone ask for anyone else's
	// ship positions, which are the only secret in the game.
	mux.HandleFunc("GET /v1/reconnect", func(w http.ResponseWriter, r *http.Request) {
		tb, peer, name, err := tables.Resolve(r.Header.Get("X-Peer-Token"))
		if err != nil {
			httpError(w, http.StatusUnauthorized, err)
			return
		}
		board := tables.BoardOf(tb)
		frames := board.Frames(peer)

		// The board reads packets as the same fixed 8-byte frames it sends,
		// so it needs no parser at all.
		// Where to poll from next. Everything in the log up to here is already
		// accounted for in the board just handed over, so a client that polled
		// from where it left off would replay the whole match on top of it.
		next := tb.Hub.Status().Packets
		if r.URL.Query().Get("fmt") == "bin" {
			w.Header().Set("Content-Type", "application/octet-stream")
			w.Header().Set("X-Next", strconv.Itoa(next))
			for _, p := range frames {
				b := p.MarshalWire()
				_, _ = w.Write(b[:])
			}
			logs.Columns(tb.Label(), "resume", name,
				fmt.Sprintf("reconnected: %d frames of board sent", len(frames)))
			return
		}
		logs.Columns(tb.Label(), "resume", name,
			fmt.Sprintf("reconnected: %d frames of board sent", len(frames)))
		writeJSON(w, map[string]any{
			"room": tb.Room, "seat": peerName(peer), "next": next,
			"packets": frames, "board": board.Snapshot(tb.Room),
		})
	})

	mux.HandleFunc("GET /v1/lobby", func(w http.ResponseWriter, r *http.Request) {
		id := playerID(r)
		lobby.Touch(id)
		// Others(), never All(): the caller is removed by the relay so that a
		// client cannot offer itself as its own opponent.
		others := lobby.Others(id)

		// A board has no JSON parser worth the flash, so it gets lines it can
		// read with sscanf: slot, "1" if that player has already chosen the
		// caller, then the name to the end of the line. The name is last
		// because it may contain spaces, and the team is not sent at all --
		// red and black are an ESP32 label, carried in the board's own name.
		// The slot is an index into this same list, which is sorted by UUID
		// and therefore does not shuffle under a cursor pointing at a row.
		if r.URL.Query().Get("fmt") == "text" {
			w.Header().Set("Content-Type", "text/plain")
			if m := lobby.MatchOf(id); m != nil && lobby.Attached(id) {
				seat, token := lobby.SeatOf(id)
				p, _ := lobby.Player(id)
				// A board is told the same thing on the same terms: no token,
				// not yet matched -- it polls again in a second.
				if token != "" {
					// The opponent's name last, so it may contain spaces. The
					// board's poll returns here and never reads a room row, so
					// this is the only place it can learn who it is playing.
					fmt.Fprintf(w, "matched %s %s %s %s\n", seat, p.Team, token,
						lobby.Opponent(id))
					return
				}
			}
			for i, p := range others {
				chose := 0
				if p.Invited == id {
					chose = 1
				}
				fmt.Fprintf(w, "%d %d %s\n", i, chose, displayName(p.Name))
			}
			return
		}

		// The browser draws the same list through the same core, so it is told
		// the same things: a slot to invite with, and who has already chosen
		// it. Invited itself is not sent -- whom someone else has picked is
		// nobody's business but the person they picked.
		listed := make([]map[string]any, 0, len(others))
		for i, p := range others {
			listed = append(listed, map[string]any{
				"id":        p.ID,
				"name":      displayName(p.Name),
				"team":      p.Team,
				"slot":      i,
				"chose_you": p.Invited == id,
			})
		}

		out := map[string]any{"players": listed, "you": id}
		// A player who has been paired learns it here: their seat, their team,
		// and the token they talk with. Told only to them -- the lobby at
		// large has no business knowing anyone's token.
		// Only once the tokens exist. Pairing and handing out seats are two
		// steps, and a poll landing between them would tell the client it is
		// in a match and give it an empty token to talk with -- which the
		// relay then refuses, stranding the page on a board it cannot play.
		// Only for a player who has taken their seat. A match survives the
		// connection that was playing it, but a client that has just
		// restarted belongs on the lobby list until it chooses the lobby its
		// match is in -- otherwise reloading a page drops the player straight
		// back into a game they never asked to return to.
		if m := lobby.MatchOf(id); m != nil && lobby.Attached(id) {
			seat, token := lobby.SeatOf(id)
			p, _ := lobby.Player(id)
			if token == "" {
				writeJSON(w, out)
				return
			}
			out["matched"] = true
			out["match"] = m.ID
			out["seat"] = seat
			out["team"] = p.Team
			out["token"] = token
			out["opponent"] = lobby.Opponent(id)
		}
		writeJSON(w, out)
	})

	// ---- the numbered rooms ----
	// Five of them, always listed whether or not anyone is in one. A player
	// who would rather not wait to be chosen picks a room, and the second
	// person into it is their opponent -- so there is no waiting on agreement
	// and five matches can be running at once.
	mux.HandleFunc("GET /v1/rooms", func(w http.ResponseWriter, r *http.Request) {
		id := playerID(r)
		lobby.Touch(id)

		// Nothing changed since the version the caller last saw? Then say so
		// in a status line and stop. A lobby list is polled once a second by
		// every client in it and changes perhaps twice a minute, so this is
		// the answer almost every time -- and on a board it is the difference
		// between parsing seven lines and reading one.
		version := lobby.Version()
		w.Header().Set("X-Lobby-Version", strconv.Itoa(version))
		if seen := r.URL.Query().Get("since"); seen != "" {
			if n, err := strconv.Atoi(seen); err == nil && n == version &&
				lobby.MatchOf(id) == nil {
				w.WriteHeader(http.StatusNoContent)
				return
			}
		}
		rooms := lobby.Rooms(id)
		you := 0
		if p, ok := lobby.Player(id); ok {
			you = p.Room
		}
		// The board reads lines, not JSON: room, how many are in it, then the
		// names separated by a pipe -- a separator a name cannot contain,
		// where a space can. This is also where a board learns it has been
		// paired, so picking a room and starting a match are one poll rather
		// than two endpoints a board has to keep in step.
		if r.URL.Query().Get("fmt") == "text" {
			w.Header().Set("Content-Type", "text/plain")
			if m := lobby.MatchOf(id); m != nil && lobby.Attached(id) {
				seat, token := lobby.SeatOf(id)
				p, _ := lobby.Player(id)
				// A board is told the same thing on the same terms: no token,
				// not yet matched -- it polls again in a second.
				if token != "" {
					// The opponent's name last, so it may contain spaces. The
					// board's poll returns here and never reads a room row, so
					// this is the only place it can learn who it is playing.
					fmt.Fprintf(w, "matched %s %s %s %s\n", seat, p.Team, token,
						lobby.Opponent(id))
					return
				}
			}
			fmt.Fprintf(w, "you %d\n", you)
			for _, rm := range rooms {
				yours := 0
				if rm.Yours {
					yours = 1
				}
				fmt.Fprintf(w, "%d %d %d %s\n", rm.N, len(rm.Players), yours,
					strings.Join(rm.Players, "|"))
			}
			return
		}
		out := map[string]any{"rooms": rooms, "you": you, "count": RoomCount}
		// Being paired is reported here too, so a client watching the rooms
		// has one poll rather than two: picking a lobby and the match
		// starting in it are one conversation.
		// Only once the tokens exist. Pairing and handing out seats are two
		// steps, and a poll landing between them would tell the client it is
		// in a match and give it an empty token to talk with -- which the
		// relay then refuses, stranding the page on a board it cannot play.
		// Only for a player who has taken their seat. A match survives the
		// connection that was playing it, but a client that has just
		// restarted belongs on the lobby list until it chooses the lobby its
		// match is in -- otherwise reloading a page drops the player straight
		// back into a game they never asked to return to.
		if m := lobby.MatchOf(id); m != nil && lobby.Attached(id) {
			seat, token := lobby.SeatOf(id)
			p, _ := lobby.Player(id)
			if token == "" {
				writeJSON(w, out)
				return
			}
			out["matched"] = true
			out["match"] = m.ID
			out["seat"] = seat
			out["team"] = p.Team
			out["token"] = token
			out["opponent"] = lobby.Opponent(id)
		}
		writeJSON(w, out)
	})

	mux.HandleFunc("POST /v1/rooms/join", func(w http.ResponseWriter, r *http.Request) {
		id := playerID(r)
		if id == "" {
			httpError(w, http.StatusBadRequest, errors.New("room: send X-Player-Id"))
			return
		}
		n, err := strconv.Atoi(r.URL.Query().Get("n"))
		if err != nil {
			httpError(w, http.StatusBadRequest, errors.New("room: ?n= which room"))
			return
		}
		m, err := lobby.JoinRoom(id, n)
		if err != nil {
			// A full room is a refusal, not a fault: the page redraws and the
			// row it was told about is now shown as taken.
			httpError(w, http.StatusConflict, err)
			return
		}
		p, _ := lobby.Player(id)
		if m == nil {
			logs.Columns("main", "lobby", p.Name,
				fmt.Sprintf("sat down in lobby %d, waiting for an opponent", n))
			writeJSON(w, map[string]any{"room": n, "paired": false})
			return
		}
		// Seats and tokens are handed out here, exactly as a mutual invitation
		// does it -- a room is another way to agree on an opponent, not another
		// kind of match.
		if _, tok := lobby.SeatOf(id); tok == "" {
			startMatch(tables, tables.Room(m.Room), lobby, m)
		}
		seat, token := lobby.SeatOf(id)
		writeJSON(w, map[string]any{
			"room": n, "paired": true, "match": m.ID, "seat": seat, "token": token,
		})
	})

	// Standing up again, which is what a held centre asks for. The room is
	// freed on the relay rather than only on the client, so the other player's
	// next poll shows an empty row instead of someone who has gone.
	mux.HandleFunc("POST /v1/rooms/leave", func(w http.ResponseWriter, r *http.Request) {
		id := playerID(r)
		p, known := lobby.Player(id)
		was := p.Room
		lobby.LeaveRoom(id)
		if known && was != 0 {
			logs.Columns("main", "lobby", p.Name,
				fmt.Sprintf("left lobby %d -- back to the list", was))
		}
		writeJSON(w, map[string]any{"room": 0})
	})

	mux.HandleFunc("POST /v1/lobby/invite", func(w http.ResponseWriter, r *http.Request) {
		id := playerID(r)
		to := r.URL.Query().Get("to")
		// A board invites by slot, since it holds a five-character name and a
		// row number rather than a 36-character UUID for everyone present.
		if slot := r.URL.Query().Get("slot"); slot != "" {
			n, err := strconv.Atoi(slot)
			others := lobby.Others(id)
			if err != nil || n < 0 || n >= len(others) {
				httpError(w, http.StatusBadRequest, errors.New("invite: no such slot"))
				return
			}
			to = others[n].ID
		}
		if id == "" || to == "" {
			httpError(w, http.StatusBadRequest, errors.New("invite: need X-Player-Id and ?to="))
			return
		}
		if id == to {
			httpError(w, http.StatusBadRequest, errors.New("invite: you cannot play yourself"))
			return
		}
		m := lobby.Invite(id, to)
		if m == nil {
			// Recorded, waiting for them to choose us back. Not an error: half
			// of a mutual choice is the normal state for a moment.
			writeJSON(w, map[string]any{"paired": false})
			return
		}
		startMatch(tables, tables.Room(m.Room), lobby, m)
		writeJSON(w, map[string]any{"paired": true, "match": m})
	})

	mux.HandleFunc("GET /healthz", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/plain")
		_, _ = w.Write([]byte("ok\n"))
	})

	// Everything else is the page itself. Registered last and on the bare
	// root so the /v1 routes win: a more specific pattern always beats "/".
	mux.Handle("GET /", noCache(http.FileServer(http.Dir(*webDir))))

	// The dashboard is a second front door onto the same handlers. nginx can
	// serve it instead (dashboard/nginx.conf does exactly that, proxying /v1
	// here), but serving it directly means the dashboard works before nginx is
	// installed, and with no CORS or proxy in the way either.
	dashMux := http.NewServeMux()
	dashMux.Handle("/v1/", mux)
	dashMux.Handle("/healthz", mux)
	dashMux.Handle("/", noCache(http.FileServer(http.Dir(*dashDir))))

	srv := &http.Server{
		Addr:              *addr,
		Handler:           mux,
		ReadHeaderTimeout: 5 * time.Second,
		WriteTimeout:      writeTimeout,
		IdleTimeout:       60 * time.Second,
	}

	var dash *http.Server
	if *dashAddr != "" {
		dash = &http.Server{
			Addr:              *dashAddr,
			Handler:           dashMux,
			ReadHeaderTimeout: 5 * time.Second,
			WriteTimeout:      writeTimeout,
			IdleTimeout:       60 * time.Second,
		}
		go func() {
			log.Printf("dash  on http://%s  (%s)", *dashAddr, *dashDir)
			if err := dash.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
				log.Printf("dashboard: %v", err)
			}
		}()
	}

	go func() {
		log.Printf("relay on http://%s  (web=%s, esp-now=%v)", *addr, *webDir, *espNow)
		warnIfLoopback(*addr)
		announceLAN(*addr)
		if *espNow {
			log.Printf("esp-now: shots and results will be REFUSED; the firmware side is a future change")
		}
		if err := srv.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			log.Fatalf("listen: %v", err)
		}
	}()

	// Shut down on the first Ctrl-C rather than dropping parked polls on the
	// floor, so a restart during development does not leave clients hanging.
	stop := make(chan os.Signal, 1)
	signal.Notify(stop, os.Interrupt, syscall.SIGTERM)
	<-stop
	log.Print("shutting down")
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if dash != nil {
		_ = dash.Shutdown(ctx)
	}
	_ = srv.Shutdown(ctx)
}

// startMatch turns a mutual choice into a running game: seats and tokens for
// the two players, then a Start packet to each telling it which end it is on.
//
// Both boards are told by the SAME authority at the same moment, which is what
// makes them agree about who is seat A -- and therefore about who shoots
// first. Two boards working it out separately is how they would disagree.
// sendEverywhere puts one server packet on every table. A broadcast from the
// dashboard means "everyone", and everyone is spread across the base table
// and the five rooms.
//
// A table that refuses it -- direct-link mode -- is reported, but the others
// are still sent to: one room configured out of the way should not silence
// the whole relay.
func sendEverywhere(tables *Tables, p Packet) error {
	var firstErr error
	for _, tb := range tables.All() {
		if err := tb.Hub.Send(p); err != nil && firstErr == nil {
			firstErr = err
		}
	}
	return firstErr
}

// liveTable is the table with a game on it: a room where both players have
// placed, else a room with any traffic, else the base table. The dashboard
// shows one match, and with five rooms the base table is usually the empty
// one -- so "the match" has to mean the one being played.
func liveTable(tables *Tables) *Table {
	var busy *Table
	for _, tb := range tables.All() {
		if tb.Hub.BothReady() {
			return tb
		}
		if busy == nil && tb.Room != 0 && tb.Hub.Status().Packets > 0 {
			busy = tb
		}
	}
	if busy != nil {
		return busy
	}
	return tables.Base()
}

// How long a finished match's table outlives the match. Longer than the
// verdict the panels hold (game::VERDICT_MS, 6s), so both players can collect
// the last packets and read who won before their seats stop existing.
const matchLinger = 9 * time.Second

// A name is a label and may be empty; the panels need something to draw.
func displayName(name string) string {
	if name == "" {
		return "player"
	}
	return name
}

func startMatch(tables *Tables, tb *Table, lobby *Lobby, m *Match) {
	// A new game starts on a clean table, whatever is still on this one.
	//
	// The last match's table lingers for a few seconds so both players can
	// read the verdict, and the lobby is free during those seconds -- so a
	// new pair can sit down before the old board has been cleared away.
	// Clearing it HERE is what makes that harmless: a game always begins with
	// an empty log, empty seats and an empty board, and the lingering timer
	// then finds nothing left to do.
	tables.Reset(tb.Room)
	seats := tables.SeatsOf(tb)
	a, _ := lobby.Player(m.A)
	b, _ := lobby.Player(m.B)
	lobby.SetToken(m.ID, "a", seats.Assign(PeerA, a.Name))
	lobby.SetToken(m.ID, "b", seats.Assign(PeerB, b.Name))

	teamBit := func(t string) uint8 {
		if t == "black" {
			return 1
		}
		return 0
	}
	for _, side := range []struct {
		dst  PeerID
		seat uint8
		team string
	}{{PeerA, 0, a.Team}, {PeerB, 1, b.Team}} {
		p := Packet{V: Version, Src: PeerServer, Dst: side.dst, T: TStart,
			A: side.seat, B: teamBit(side.team)}
		if err := tb.Hub.Send(p); err != nil {
			log.Printf("start  %v", err)
		}
	}
	logs.Columns(tb.Label(), "lobby", "server", "match "+m.ID+": "+a.Name+" ("+a.Team+
		") vs "+b.Name+" ("+b.Team+")")
}

func writeJSON(w http.ResponseWriter, v any) {
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(v)
}

func httpError(w http.ResponseWriter, code int, err error) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(map[string]string{"error": err.Error()})
}

// A relay bound to loopback is reachable from this machine and from nowhere
// else. That is the right default -- it keeps the relay off the network until
// it is wanted there -- but it is also invisible from the board's end: the
// kernel answers a connection on the LAN interface with a reset, which an
// ESP32 reports only as "errno 104, Connection reset by peer". Say it here,
// where the cause is known, rather than leaving it to be worked out from the
// far end.
// looksLikeLoopback reports whether an address reaches this machine only.
//
// An empty host is every interface -- ":8080" is the ordinary way to write
// that -- so it is the opposite of loopback rather than a special case of it.
// Reading it as loopback made the relay print "no ESP32 or phone can reach
// this" about an address every ESP32 and phone could reach, which is the
// wrong end of the very mistake this warning exists to catch.
func looksLikeLoopback(addr string) bool {
	host, _, err := net.SplitHostPort(addr)
	if err != nil || host == "" {
		return false
	}
	return host == "localhost" || net.ParseIP(host).IsLoopback()
}

func warnIfLoopback(addr string) {
	_, port, err := net.SplitHostPort(addr)
	if err != nil {
		return
	}
	if !looksLikeLoopback(addr) {
		return
	}
	log.Printf("note:  bound to loopback -- no ESP32 or phone can reach this")
	log.Printf("       for the WiFi:   make -C server run ADDR=0.0.0.0:%s", port)
	if ips := lanAddrs(); len(ips) > 0 {
		log.Printf("       and set SERVER_IP in include/config.h to one of: %s",
			strings.Join(ips, ", "))
	}
}

// Where everyone else on the WiFi should point their browser. Printed at
// startup because it is the one thing a phone in the room needs and the one
// thing the machine running the relay never has to look up.
func announceLAN(addr string) {
	_, port, err := net.SplitHostPort(addr)
	if err != nil {
		return
	}
	for _, ip := range lanAddrs() {
		log.Printf("       on the WiFi:    http://%s:%s", ip, port)
	}
}

// The addresses a board on the same network could actually use.
func lanAddrs() []string {
	var out []string
	addrs, err := net.InterfaceAddrs()
	if err != nil {
		return out
	}
	for _, a := range addrs {
		n, ok := a.(*net.IPNet)
		if !ok || n.IP.IsLoopback() || n.IP.To4() == nil {
			continue
		}
		out = append(out, n.IP.String())
	}
	return out
}

// The WASM build is rebuilt constantly during development, and a cached
// sim.wasm against a fresh main.js is a confusing way to lose an afternoon.
func noCache(h http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Cache-Control", "no-store")
		h.ServeHTTP(w, r)
	})
}
