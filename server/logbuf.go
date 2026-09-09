package main

import (
	"fmt"
	"log"
	"os"
	"sync"
	"time"
)

// The relay's own output, kept as well as printed.
//
// The dashboard shows what the terminal shows, which means the log has to be
// readable by something other than the terminal. Rather than have handlers
// report themselves twice -- once to the log and once to a feed, which is how
// the two drift apart -- this sits underneath log.Print and keeps what went
// past.
const logKeep = 500

// A line, and its parts when it has any.
//
// The terminal wants one aligned string; the dashboard wants columns it can
// lay out itself. Emitting both from one place is what keeps them saying the
// same thing -- a handler that reported to each separately is how they would
// drift.
type LogLine struct {
	At   time.Time `json:"at"`
	Text string    `json:"text"` // the whole line, as the terminal shows it

	// The same line split into columns. A packet fills all of them; a message
	// from the relay itself fills only From and Detail. Empty fields are left
	// out of the JSON so the dashboard can tell a packet from a remark.
	// Which lobby the line belongs to: "main" for the lobby everyone is in
	// before a game -- joining, picking a room, seats -- and "lobby N" for
	// one of the five games. Without it the five matches' packets arrive
	// interleaved in one stream with nothing to tell them apart.
	Scope  string `json:"scope,omitempty"`
	Page   string `json:"page,omitempty"`   // which screen it happened on
	Player string `json:"player,omitempty"` // who sent it
	To     string `json:"to,omitempty"`     // who it was for
	Kind   string `json:"kind,omitempty"`   // shot, result, fleet, ...
	Cell   string `json:"cell,omitempty"`   // A5, E3
	Detail string `json:"detail,omitempty"` // whatever is left to say
	Seq    int    `json:"seq,omitempty"`    // the sender's own counter
}

type LogBuf struct {
	mu    sync.Mutex
	lines []LogLine
	first int // index of lines[0] in the whole stream, for a stable cursor
}

var logs = &LogBuf{}

// Write makes this an io.Writer, so log.SetOutput hands us every line.
// Packet records one carried packet with every field kept apart, which is
// what lets the dashboard show, align and hide them individually.
func (b *LogBuf) Packet(scope, page, from, to, kind, cell, detail string, seq int) {
	text := fmt.Sprintf("%-7s %-8s %-13s -> %-13s %-7s %-4s %s",
		scope, page, from, to, kind, cell, detail)

	b.mu.Lock()
	b.lines = append(b.lines, LogLine{
		At: time.Now(), Text: text, Scope: scope, Page: page, Player: from, To: to,
		Kind: kind, Cell: cell, Detail: detail, Seq: seq,
	})
	b.trimLocked()
	b.mu.Unlock()

	fmt.Fprintf(os.Stderr, "%s %s\n", time.Now().Format("2006-01-02 15:04:05.000"), text)
}

// Columns is how a relay message is recorded: the parts kept separately for
// the dashboard, and printed aligned for the terminal.
func (b *LogBuf) Columns(scope, page, player, detail string) {
	text := fmt.Sprintf("%-7s %-8s %-12s %s", scope, page, player, detail)

	b.mu.Lock()
	b.lines = append(b.lines, LogLine{
		At: time.Now(), Text: text, Scope: scope, Page: page, Player: player,
		Detail: detail,
	})
	b.trimLocked()
	b.mu.Unlock()

	fmt.Fprintf(os.Stderr, "%s %s\n", time.Now().Format("2006-01-02 15:04:05.000"), text)
}

// stripLogStamp removes the "2026/09/07 15:56:16 " that log.Print puts on the
// front, leaving the sentence. The terminal keeps a stamp of its own, added
// where every other line gets one, so nothing is lost.
func stripLogStamp(text string) string {
	// "YYYY/MM/DD HH:MM:SS " is 20 characters, and nothing else in this
	// program starts with a digit and a slash in those places.
	const n = len("2006/01/02 15:04:05 ")
	if len(text) < n || text[4] != '/' || text[7] != '/' || text[13] != ':' {
		return text
	}
	return text[n:]
}

func (b *LogBuf) trimLocked() {
	if len(b.lines) > logKeep {
		drop := len(b.lines) - logKeep
		b.lines = b.lines[drop:]
		b.first += drop
	}
}

// Write makes this an io.Writer under log.Print, which is where the relay's
// own remarks arrive -- the startup banner, a warning, a Go error.
//
// They land in the SAME columns as everything else. They used to be stored as
// one undivided string, so the dashboard had a table whose rows sometimes had
// seven cells and sometimes one, and the startup lines, the lobby lines and
// the gameplay lines each read as a different thing. A remark is the relay
// talking in the main lobby about nothing in particular: scope "main", no
// page, "server" as the speaker, and the remark itself as the detail.
func (b *LogBuf) Write(p []byte) (int, error) {
	text := string(p)
	for len(text) > 0 && (text[len(text)-1] == '\n' || text[len(text)-1] == '\r') {
		text = text[:len(text)-1]
	}
	// log.Print stamps its own date and time on the front, and the dashboard
	// has a "when" column of its own; two timestamps on one row is one too
	// many.
	detail := stripLogStamp(text)

	b.mu.Lock()
	b.lines = append(b.lines, LogLine{
		At: time.Now(), Text: fmt.Sprintf("%-7s %-8s %-12s %s", "main", "-", "server", detail),
		Scope: "main", Page: "-", Player: "server", Detail: detail,
	})
	b.trimLocked()
	b.mu.Unlock()

	// Still goes to the terminal: the dashboard is an addition, not a
	// replacement, and a relay run without one must lose nothing.
	return os.Stderr.Write(p)
}

// Since returns the lines after a cursor, and the cursor to ask with next.
// The cursor counts the whole stream, not the buffer, so it stays meaningful
// when old lines are dropped -- a reader that falls behind is moved forward
// rather than being fed the wrong lines.
func (b *LogBuf) Since(cursor int) ([]LogLine, int) {
	b.mu.Lock()
	defer b.mu.Unlock()
	next := b.first + len(b.lines)
	if cursor < b.first {
		cursor = b.first
	}
	if cursor > next {
		cursor = next
	}
	out := make([]LogLine, next-cursor)
	copy(out, b.lines[cursor-b.first:])
	return out, next
}

func init() {
	log.SetOutput(logs)
}
