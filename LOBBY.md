# Lobby, identity, reconnect and ESP-NOW

Design for the next block of work. Written before the code because four of
these features share one decision -- what identifies a player -- and getting
that wrong makes the other three unbuildable.

## 0. The one decision everything hangs on

Today a player is identified by a **seat** (`a` or `b`) claimed by **name**,
and there are exactly two. That cannot carry any of what follows:

- a lobby needs more than two people connected at once;
- two players may both call themselves "nirvek";
- reconnecting needs to recognise a player who has *gone*, which is precisely
  when their seat has been reclaimed.

So identity moves to a **UUID the client generates once and keeps**. The name
becomes a label, free to collide. The seat becomes a per-match role rather
than an identity.

| Concept | Today | After |
| --- | --- | --- |
| identity | seat name, unique | UUID, client-generated, stable across reloads |
| display | the same string | name, may collide, truncated to 16 on the lobby page and 5 elsewhere |
| seat | claimed at join, 2 total | assigned when a match starts, `a`/`b` |
| capacity | 2 connected | many connected, 2 per match |

A player is therefore `{uuid, name, team, matchId?}`, and a match is
`{id, a: uuid, b: uuid, state}`. The relay already has the packet log it needs
to rebuild a match; what it lacks is the mapping from a returning UUID back to
one.

## 1. Protocol additions

The 8-byte frame stays. Packet types 10..14:

| `t` | Name | `a` | `b` | Meaning |
| --- | --- | --- | --- | --- |
| 10 | `hello` | team | — | I am here; my UUID is in the HTTP layer, not the frame |
| 11 | `lobby` | count | index | one entry of the player list, sent repeatedly |
| 12 | `invite` | peer slot | — | I choose that player as my enemy |
| 13 | `start` | your seat | your team | both chose each other; the match begins |
| 14 | `resume` | what follows | count | a snapshot is coming: fleets, then shots |

**UUIDs do not fit in the frame** -- 16 bytes against 8, and the frame is
fixed because ESP-NOW needs it so. So a UUID travels in the HTTP layer
(`X-Player-Id`) and over ESP-NOW as the MAC address, which is already unique
per board. The frame carries a **slot index** into the lobby list instead, and
the relay maps slot to UUID. That keeps the frame as it is and means a board
never has to hold a 16-byte string.

## 2. Lobby

A screen between Start and ship placement.

- **Top panel:** `LOBBY`, then the connected players, one per line, name
  truncated to 16 characters -- the line has no team letter on it, because red
  and black are a board's name rather than a column. A tick at the right end
  marks a player who has already chosen you. The cursor moves down the list;
  centre invites.
- **Bottom panel:** the wave, so a waiting screen never looks like a hung one.
- **You are never in your own list.** The relay omits the caller from the list
  it sends, rather than the client filtering: a client that forgot to filter
  would let a player invite themselves, and the relay would then be pairing
  someone with themselves with no rule left to catch it.
- **Pairing is mutual.** An invite alone does nothing; the relay starts a match
  only when A invites B *and* B invites A. That way there is no accept/decline
  state to get out of step -- both sides express the same thing and the relay
  looks for agreement.

## 3. Start page: online or offline

Top panel: `BATTLESHIP`, then two buttons under it -- `ONLINE` left, `OFFLINE`
right, left/right to choose and centre to confirm. Bottom panel keeps the ship.

- **ONLINE** is the flow that exists: connect, lobby, place, match.
- **OFFLINE** is ESP-NOW, board to board, no relay. It waits for the other
  board rather than for a server.

The choice is made before any network exists, which is the point: a board with
no WiFi configured can still play its neighbour.

## 4. Reconnect

The relay keeps a match after a player drops, because the packet log already
*is* the match: every fleet and every shot passed through it.

On `join` with a known UUID whose match is still running, the relay replies
`resume` and replays that player's view -- their own fleet, then the enemy
fleet, then every shot and result in order. The core already handles each of
those packets; replaying them rebuilds the boards with no new drawing code and
no second source of truth for what happened.

This is why **every move must go through the relay**, including ones a board
could resolve locally. A move that never became a packet cannot be replayed,
and the returning player's board would differ from their opponent's.

## 5. SUNK display

`SUNK` with the owner and the ship beneath it: `BLACK SUBMARINE`. Names are
truncated to 5 characters so a long one cannot push the line off a 128px
panel, and the ESP only ever shows `BLACK` or `RED` -- the team, not the
player's name, because the team is the thing that is always short enough and
always known.

## 6. Order of work

Each step is usable before the next exists:

1. **Identity.** UUID at join, name as a label, both carried and displayed.
2. **Start page.** Online/offline choice on the board; name entry in the browser.
3. **Lobby.** List, invite, mutual pairing, match start.
4. **Sunk display.** Team plus ship.
5. **Reconnect.** Snapshot replay on a returning UUID.
6. **ESP-NOW.** The offline path. Built: two boards find each other by
   broadcasting a four-byte hello, and a game frame is eight bytes, so the two
   are told apart by length alone and a hello can never be read as a move.
   Seats are decided by comparing MAC addresses -- the same reasoning as
   ordering by UUID online, since seat A shoots first and neither board can be
   allowed to win that by speaking first.
