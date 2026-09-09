# The relay

A single Go program that does two things at once: it serves the WASM page, and
it routes packets between whoever is playing. Serving the page from the same
origin as the API is the point — the browser talks to the relay with no CORS
handling and no second port to remember.

    make -C server run          # http://localhost:8080 and the WiFi, page + routing
    make -C server run-quiet    # the same, without a line per packet
    make -C server run-espnow   # gameplay reserved for the direct board-to-board link
    make -C server test
    make -C server help

It holds **no game rules**. Those live in `src/game/`, compiled into both the
firmware and the WASM build. The relay moves validated packets, remembers what
it has seen, and adds them up into a position — everything it appears to know
about a match is a reading of the traffic rather than authority over it.

## The files

| File | What it is |
| --- | --- |
| `main.go` | flags, HTTP routes, and the startup banner |
| `packet.go` | the packet: fields, validation, JSON and 8-byte wire forms |
| `hub.go` | one match's append-only log, per-reader cursors, the long poll |
| `peers.go` | one match's two seats: tokens, reclaiming a quiet one |
| `board.go` | one match's position, added up from the packets it carried |
| `tables.go` | a Hub, Seats and Board per numbered lobby, so five run at once |
| `lobby.go` | players by UUID, the five lobbies, pairing, reaping |
| `logbuf.go` | the relay's own output, kept so the dashboard can show it |
| `PROTOCOL.md` | the protocol of record: packet, types, encodings, endpoints |

## How a game starts

1. `POST /v1/lobby/join` — a player announces themselves. Identity is a UUID
   the client makes and keeps; the name is a label and may collide.
2. `GET /v1/rooms` — the five numbered lobbies, who is in each, and which one
   is *yours*. Polled once a second; answered `204 No Content` when nothing
   has changed since the version the caller last saw.
3. `POST /v1/rooms/join?n=K` — sit down. The second player into a lobby is the
   opponent: the match starts there and then, seats are handed out, and a
   `start` packet tells each side which end it is on.
4. `POST /v1/send` / `GET /v1/recv` — the match itself, carried by the table
   for that lobby and nobody else's.

A player who restarts lands back on the lobby list, not in their game. Their
lobby is marked as theirs, and choosing it again is what puts them back in —
`GET /v1/reconnect` then hands the position over as marks that name their own
cell. See "Walking back into a match" in the root `README.md`.

## What each piece is for

**One table per lobby.** A packet log and two seats used to be the whole
server, which meant the whole server was one match. `tables.go` gives every
lobby a `Hub`, a `Seats` and a `Board` of its own, so a token resolves to the
table it was issued for and seat A of lobby 2 can never read seat A of lobby
1's mail. Table 0 is the base table, kept for the CLI, the dashboard and a
board pointed at a bare relay.

**The log is the match, the board is the position.** `hub.go` keeps every
packet with a timestamp, which is what a long poll reads and what the CSV
exports. `board.go` folds those same packets into where the ships are and
which cells have been answered — the question a returning player is actually
asking, and one the log can only answer by being walked from the start.

**A match ends and takes its lobby with it.** Decided (five distinct ships
sunk from one side), left by a player, or abandoned by both: the match is
cleared and the lobby's table is wiped — fresh log, fresh seats, fresh board.
That is what makes the next game in that lobby a new game rather than a
continuation of the last one, and replacing the seats invalidates the old
tokens, which is how both clients learn to go back to the lobby list.

**Identification, not security.** Seat tokens exist so two honest clients
cannot be mistaken for each other. The control routes — reset, restart,
force-page, seat eviction, the CSV — are behind an admin token the relay
prints at startup, because those are the ones that can ruin a match in
progress, and because the relay now listens on the whole WiFi by default.

## On the network

The relay binds every interface (`0.0.0.0:8080`) and prints the LAN addresses
to open. A relay nobody else can reach is a relay with one player on it: the
boards are on the WiFi and so are the phones. `make -C server run
ADDR=localhost:8080` puts it back on loopback.

Two addresses look plausible and are not: `127.0.0.1`, which to a board means
the board itself, and the subnet's `.255`, which is a broadcast and not a
host. `netBanner()` on the firmware checks for both at boot, because they fail
silently rather than with an error.

Three habits keep the polling cheap, and all three exist because breaking them
cost something visible:

- **Cursors, not sequence numbers.** `since` is a position in this table's
  log. Two senders reuse sequence numbers; the log's own order is the only one
  both ends can agree on.
- **Batches.** A fleet is six packets. Both the browser and the board post
  them in one request — six connections to say one thing is six times the cost
  and hands the ordering to a connection pool, and `ready` must arrive last.
- **A version on the lobby list.** It changes twice a minute and is asked for
  once a second per client; `?since=V` turns almost every one of those into a
  status line.

## Testing

    make -C server test     # go test ./...
    make -C server vet

`packet_test.go` covers the wire form and validation; `lobby_test.go` covers
pairing, the five lobbies, leaving, reaping, name sanitising and per-lobby
table isolation. The core's own suites live in `tools/` at the repo root and
run with `make test`.
