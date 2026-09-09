# Match protocol v1

One packet shape carries a whole match. It is defined once here and
implemented in `server/packet.go`; the browser and the firmware encode the
same seven fields.

The packet is deliberately **transport-independent**. Today it travels as JSON
over HTTP to the relay in `server/`. The plan is for the two boards to talk
directly over ESP-NOW, whose payload is small and fixed, so the same packet
also has an 8-byte binary form. Defining one packet with two encodings, rather
than one protocol per transport, is what stops the move to ESP-NOW from being
a rewrite.

## The packet

| Field | Type | Meaning |
| --- | --- | --- |
| `v` | uint8 | protocol version, `1` |
| `seq` | uint16 | the sender's own counter, for dedupe and ordering |
| `src` | uint8 | who sent it |
| `dst` | uint8 | who it is for, or `255` for everyone |
| `t` | uint8 | what it means |
| `a` | uint8 | first argument, typed by `t` |
| `b` | uint8 | second argument, typed by `t` |

Peers are small integers, not names, because the same packet has to fit an
ESP-NOW frame: `0` server, `1` player A, `2` player B, `255` broadcast. A
packet may be addressed to the broadcast id but may never claim to come from
it.

## Types and their arguments

| `t` | Name | `a` | `b` |
| --- | --- | --- | --- |
| 1 | `join` | feature bits (0 today) | - |
| 2 | `ready` | - | - |
| 3 | `shot` | column 0..9 | row 0..9 |
| 4 | `result` | 0 miss, 1 hit, 2 sunk | ship 0..4, and 0 when a miss |
| 5 | `reset` | - | - |
| 6 | `bye` | - | - |
| 7 | `fleet` | ship 0..4 | that ship's placement, packed |
| 8 | `ack` | - | - |
| 9 | `page` | which page, 0..9 | - |
| 10 | `myfleet` | ship 0..4 | that ship's placement, packed |
| 11 | `start` | your seat, 0 a / 1 b | your team, 0 red / 1 black |
| 12 | `mark` | cell, col + row*10 | result in the low two bits, ship in the rest |
| 13 | `damage` | cell, col + row*10 | 1 when it hit, 0 when it missed |
| 14 | `turn` | 1 when the move is yours | - |

Types 9 to 14 come from the **server**, never from a player: a player who
could move another player's screen, or mark their grid, could cheat with it.

`mark`, `damage` and `turn` put a returning player back on their board. They
say what the position IS, where `shot` and `result` say what happened -- a
result means "the shot you just fired", which means nothing to a client that
has just restarted and fired nothing. Applying them answers nothing and fires
nothing, which is what keeps a reconnect from injecting a second copy of the
match into the match.

A `fleet` packet carries one ship, and a whole layout is five of them. The
frame is a fixed eight bytes -- that is what lets an ESP-NOW payload be read
with no length prefix and no allocation -- and a five-ship layout does not fit
in two argument bytes. Five small packets keep the frame; one big one would
not, and the size of the frame is worth more than the tidiness of a single
message. Send them **before** the `ready` that follows, so a peer seeing
`ready` knows the whole layout is already behind it.

The placement packs into `b` as `(col + row * 10) * 2`, with the orientation
in the low bit. The largest value is 199, so a position and a heading fit in
one spare byte. Both ends implement this, and the two implementations are
checked against each other.

Fleets are exchanged so each player knows **which vessels** they are hunting;
the ship list on the top panel is drawn from it. Positions are deliberately
never drawn on the tracking grid -- finding those is the game.

Two argument bytes is not a guess at what might be needed later. It is what
the game actually says: a shot is a coordinate, and a result is an outcome
plus which ship. Nothing in the rulebook needs a third.

The numbering of `result` matches `game::ShotResult` and the ship index
matches `game::ShipType` in `src/game/fleet.h`, so the firmware casts between
them rather than translating. **Changing either enum changes this protocol.**

## Validation

The relay rejects rather than forwards anything it cannot explain, so neither
the browser nor the firmware has to defend itself against a malformed peer.
A packet is refused if the version is not 1, the type is not one of the fourteen,
`src` is the broadcast id, a shot is off the 10x10 board, a result is not
miss/hit/sunk, it names a sixth ship, or it is a miss that names a ship at all.

## Encodings

**JSON**, for the browser:

```json
{"v":1,"seq":7,"src":1,"dst":2,"t":3,"a":4,"b":6}
```

**Binary**, 8 bytes fixed, for ESP-NOW and for the board over HTTP - no length
prefix and no allocation to read:

| Byte | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| | `v` | `t` | `src` | `dst` | `seq` lo | `seq` hi | `a` | `b` |

`seq` is little-endian, matching the C3.

`POST /v1/send` with `Content-Type: application/octet-stream` takes this form,
and takes **several frames back to back** in one body: handing over a fleet is
six packets, and six connections to say it would cost six times as much for
nothing. A batch is all-or-nothing -- every frame is checked before any is
accepted, because half a fleet landing is worse than none.

One implementation of this layout per language, not per call site:
`game::packWire()` / `unpackWire()` in `src/game/netplay.h`, and
`MarshalWire` / `UnmarshalWire` here. Laying the bytes out by hand at each
call site is how the board came to send its sequence number where the type
belonged, which the relay could only report as `unknown type: 0`.

## Who is who

There are only ever two seats at this table, so the relay hands them out
rather than letting clients name themselves.

`POST /v1/join {"name":"browser"}` returns the seat and a token:

```json
{"peer":1,"name":"a","token":"9f2c..."}
```

Every `send` and `recv` after that carries `X-Peer-Token`. The **seat decides
who you are, not the packet**: a `send` whose `src` disagrees with the token's
seat is refused with 403 rather than quietly corrected, because a mismatch
means the two ends disagree about who is who and rewriting it would hide that.
A `recv` reads only the token holder's own mail.

This is identification, not security. The relay is a local development tool
and the token exists so two honest clients cannot be confused for each other,
not to withstand an attacker who can already read your loopback traffic.

**A client asking for a seat it already holds is taking its own seat back**,
not asking for a second one. That is what makes reflashing the board work: the
old session holds its seat for another forty-five seconds, and the freshly booted
board would otherwise find the table full of itself. The old token is
invalidated, so the dead session cannot go on using it. Names must therefore
be unique per client, which is why the browser appends a random suffix per
tab -- two tabs both called `browser` would take the seat off each other for
ever.

A token that stops being recognised comes back as 401, and both clients treat
that as "rejoin" rather than as a permanent failure.

A seat is released by `POST /v1/leave`, and reclaimed automatically after 45
seconds of silence — a browser tab that is closed never says goodbye, and
without that the second seat would be lost until the relay restarted.

## HTTP endpoints

| Route | Does |
| --- | --- |
| `POST /v1/join` | claim a seat; returns `{peer, name, token}`, 409 when both are taken |
| `POST /v1/join?fmt=text` | the same, as `<peer> <token>` on one line, for a client with no JSON parser |
| `POST /v1/leave` | release a seat; token by header or `?token=` for `sendBeacon` |
| `POST /v1/send` | one JSON packet, a JSON array of them, or 8-byte frames back to back as `application/octet-stream`; 401 no token, 403 wrong seat, 400 invalid, 409 refused by `-esp-now` |
| `GET /v1/recv?since=N` | long poll, up to 25s; returns `{packets, next}`; token identifies the reader |
| `GET /v1/recv?...&wait=MS` | cap the park; `wait=0` answers at once, for a client with a loop to run |
| `GET /v1/recv?...&fmt=bin` | the body is 8-byte frames back to back, count is length over eight; cursor in `X-Next` |
| `GET /v1/match` | what the relay can tell about a game; `?room=N` picks one of the five, else the live one |
| `GET /v1/status` | version, flags, packet count, seated players |
| `POST /v1/lobby/join` | announce yourself; `X-Player-Id` is who you are, the body names you |
| **admin** | the routes below need `X-Admin-Token`, printed by the relay at startup |
| `POST /v1/reset` | drop every table's match history |
| `POST /v1/restart` | the same, then send both players back to the title |
| `POST /v1/force-page?page=N` | march one lobby's players to a screen; `?room=K` says which, and `match` is refused unless that lobby's fleets are down |
| `POST /v1/seats/drop?name=X` | free the one seat held by that name, leaving the other player alone |
| `POST /v1/seats/clear` | free both seats, for when gone-but-polling clients hold them |
| `GET /v1/packets.csv` | the match as a spreadsheet; admin because every fleet row carries a ship's position |
| `GET /v1/log?since=N` | what the terminal is showing, for the dashboard; ship positions are held back without the admin token |
| `GET /v1/lobby` | who else is connected: `{players:[{id,name,slot,chose_you}], matched, seat, token}` |
| `GET /v1/lobby?fmt=text` | the same as `slot chose name` lines, name last so it may hold spaces |
| `POST /v1/lobby/invite?to=UUID` | or `?slot=N`; the older "choose each other" pairing, kept and tested but used by no client here |
| `GET /v1/reconnect` | the board as it stands, for a player who has walked back into their lobby; `?fmt=bin` gives 8-byte frames and `X-Next` |
| `GET /v1/rooms` | the five numbered lobbies and who is in them (names are capped at 16 bytes, so a room line always fits a board's buffer); `?fmt=text` gives `n count yours a\|b`, `?since=V` answers 204 when nothing has changed since version V (`X-Lobby-Version`) |
| `POST /v1/rooms/join?n=K` | sit down in room K; 409 when it already has two players |
| `POST /v1/rooms/leave` | stand up again; leaving a running match ends it and clears the room |
| `GET /healthz` | liveness |
| `GET /` | the WASM page, from the same origin as the API |

`since` is a **cursor into the relay's log, not a sequence number** - pass back
the `next` you were last given. Sequence numbers belong to their sender and
two senders will reuse them, so they cannot order a shared log.

A peer never reads its own packets back; a client acting on its own shot as
though the opponent had fired it is the bug this rule exists to prevent.

## Watching a match

The relay prints every packet it passes, which matters because it is often the
only thing watching a match between two headless boards. Shots are printed
with the label the panel itself uses — rows lettered down the side, columns
numbered across, letter first — so a line in the terminal can be compared with
the screen without translating anything:

```
join   a is "browser"
join   b is "esp32"
browser  a -> esp32    shot   A5
esp32    b -> browser  result SUNK CARRIER
```

`-quiet` turns the per-packet lines off and leaves the join and reset lines.

## The ESP-NOW flag

`-esp-now` reserves gameplay for the direct board-to-board link that does not
exist yet. With it set, the relay still serves the page and still accepts
`join`, `ready`, `reset` and `bye`, but **refuses `shot` and `result` with 409**.

The refusal is the point. Once the boards carry the match themselves, a relay
that also forwarded shots would deliver every move twice and desynchronise the
game. Failing loudly on a half-migrated setup is better than a match that
quietly drifts.
