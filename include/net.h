#pragma once
#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "../src/game/game.h"

// The board's side of the link: associate with WiFi, then check that the
// relay is actually answering. Kept out of src/game/, which must stay free of
// hardware (CLAUDE.md rule 2) -- the core only ever learns a game::Link state.
//
// The two are deliberately separate questions. Being on the network is not
// the same as having something to talk to, and reporting Online the moment
// WiFi associates would mean the connecting page clears while the relay is
// still not running.

enum class NetState : uint8_t { Off, Joining, NoServer, Ready };

// Every socket operation is time-boxed, because all of them sit in the same
// loop that reads the buttons and draws the panels.
//
// WiFiClient::setTimeout() is in SECONDS, and connect() without an explicit
// timeout uses a default measured in seconds too. At 50Hz that is not a
// timeout, it is a freeze: one unreachable connect used to stall the game for
// whole seconds at a time, which made placing a ship unusable. Nothing here
// may block for longer than a frame or two.
// Two dial timeouts, because the two situations have opposite needs.
//
// Mid-match every millisecond is spent inside the loop that reads buttons and
// draws panels, so a connect must be short even at the cost of failing.
// Before the board has a seat there is no game to stall -- it is sitting on
// the connecting page -- and failing there is much worse than being slow:
// the board concludes there is no relay and plays alone for the rest of the
// match. A first connect over a weak link pays ARP and TCP retransmissions,
// and on a -76dBm signal that comfortably exceeds half a second.
// One dial timeout, not two. The split existed to keep a mid-match connect
// short, but keep-alive made the dial rare enough that the distinction stopped
// paying for itself -- and having both constants at the same number said the
// opposite of the comment above, which is worse than either choice.
constexpr int NET_CONNECT_MS = 3000;
// The budget for a whole response, enforced by this file rather than by the
// socket. WiFiClient::setTimeout() takes SECONDS, so the shortest it can be
// told to wait is 1000ms -- fifty frames -- and any read that did not complete
// paid the full two seconds. That is where "frame took 2000ms" came from: not
// the relay, which answers in well under a millisecond, but this end waiting
// on a socket with a timeout measured in the wrong unit.
constexpr int NET_READ_MS = 120;
constexpr uint32_t NET_RECV_EVERY_MS = 120;
constexpr uint32_t NET_PROBE_EVERY_MS = 2000;
// Treat the link as up while a poll has succeeded recently. Long enough to
// ride out one lost packet, short enough that a relay going away is noticed.
constexpr uint32_t NET_STALE_MS = 5000;
// When the relay is not answering, back off. Retrying an unreachable address
// every 120ms means paying the connect timeout eight times a second, and the
// game stutters in step with it.
constexpr uint32_t NET_RETRY_MS = 2000;
constexpr uint32_t NET_JOIN_EVERY_MS = 2000;
// Above this, a frame is slow enough to feel: the cursor repeat is STEP_MS
// (120ms), so a frame near that is already eating a whole cursor step.
constexpr uint32_t SLOW_FRAME_MS = 100;

static NetState netState = NetState::Off;
static uint32_t netNextProbe = 0;

inline bool wifiConfigured() { return sizeof(WIFI_SSID) > 1; }

// SERVER_IP parsed once, so every connection can use the IPAddress overload.
//
// connect(const char *host, ...) goes through name resolution even when the
// string is a literal address, and that lookup does not respect the connect
// timeout in any useful way -- with a short cap it simply fails, every time.
// The board then never sees the relay, never reports a link, and quietly
// decides it is playing alone. Resolving here removes the lookup from the
// path entirely rather than making the timeout generous enough to hide it.
static IPAddress netServer;
static bool netServerParsed = false;

inline bool netServerAddr() {
  if (!netServerParsed) netServerParsed = netServer.fromString(SERVER_IP);
  return netServerParsed;
}

// Print who and where we are. The board has no screen worth reading an
// address off and no keyboard to ask, so the serial monitor is where this
// lands -- run `pio run -t upload -t monitor` and it is the first thing shown.
inline void netBanner() {
  Serial.println();
  Serial.println(F("---- battleship-esp ----"));
  if (!wifiConfigured()) {
    Serial.println(F("wifi:   not configured (WIFI_SSID is empty in include/config.h)"));
    Serial.println(F("        playing offline; the relay is not reachable"));
    Serial.println(F("------------------------"));
    return;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("wifi:   %s  rssi %d dBm\n", WIFI_SSID, WiFi.RSSI());
    Serial.print(F("board:  "));
    Serial.print(WiFi.localIP());
    Serial.printf("   mac %s\n", WiFi.macAddress().c_str());
    Serial.print(F("subnet: "));
    Serial.print(WiFi.subnetMask());
    Serial.print(F("   gateway "));
    Serial.println(WiFi.gatewayIP());
    Serial.printf("relay:  http://%s:%d\n", SERVER_IP, SERVER_PORT);

    // The commonest way to get this wrong is to point the board at an address
    // on a different network from the one it just joined, which produces a
    // silent timeout rather than an error. Say so at boot instead.
    IPAddress parsed;
    if (parsed.fromString(SERVER_IP)) {
      const uint32_t mask = (uint32_t)WiFi.subnetMask();
      if (((uint32_t)parsed & mask) != ((uint32_t)WiFi.localIP() & mask)) {
        Serial.println(F("        WARNING: that is not on this board's subnet"));
      }
      if (parsed == IPAddress(127, 0, 0, 1)) {
        Serial.println(F("        WARNING: 127.0.0.1 is this board, not your laptop"));
      }
      if (((uint32_t)parsed | mask) == 0xFFFFFFFFu) {
        Serial.println(F("        WARNING: that is the subnet broadcast, not a host"));
      }
    } else {
      Serial.println(F("        WARNING: SERVER_IP is not a valid address"));
    }
  } else {
    Serial.printf("wifi:   could not join %s\n", WIFI_SSID);
    Serial.println(F("        playing offline"));
  }
  Serial.println(F("------------------------"));
}

inline void netBegin() {
  if (!netServerAddr()) {
    Serial.printf("relay:  SERVER_IP \"%s\" is not a valid address\n", SERVER_IP);
  }
  if (!wifiConfigured()) {
    netState = NetState::Off;
    netBanner();
    return;
  }

  netState = NetState::Joining;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("wifi:   joining %s", WIFI_SSID);

  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < WIFI_TIMEOUT_MS) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  netBanner();
  netState = (WiFi.status() == WL_CONNECTED) ? NetState::NoServer : NetState::Off;
}

// Wait for at least one byte, or the deadline, whichever comes first.
// Returns false if the deadline passed or the peer went away.
inline bool netReadable(WiFiClient &c, uint32_t deadline) {
  while ((int32_t)(millis() - deadline) < 0) {
    if (c.available()) return true;
    if (!c.connected()) return false;
    delay(1);
  }
  return false;
}

// Read one CRLF line, giving up at the deadline. Returns the line length, or
// -1 if the deadline passed first -- which the caller must treat as a torn
// response, not an empty line.
//
// Deliberately not readBytesUntil(): that blocks on the socket's own timeout,
// which cannot be set finer than a second. Deliberately not String either --
// this runs several times per poll and heap churn on a C3 is worth avoiding.
inline int netReadLineBy(WiFiClient &c, char *buf, size_t cap, uint32_t deadline) {
  size_t n = 0;
  for (;;) {
    if (!c.available()) {
      if (!netReadable(c, deadline)) { buf[n] = 0; return -1; }
      continue;
    }
    const int ch = c.read();
    if (ch < 0) continue;
    if (ch == '\n') break;
    if (n + 1 < cap) buf[n++] = (char)ch;
  }
  buf[n] = 0;
  if (n && buf[n - 1] == '\r') buf[n - 1] = 0;

  // The length AFTER stripping, not the bytes consumed. HTTP ends its headers
  // with a bare "\r\n", and readBytesUntil eats the '\n' and hands back the
  // '\r' -- one byte. Returning that count makes the blank line look like a
  // header, so every "read until the blank line" loop runs straight on into
  // the body: netJoin() would swallow the "<peer> <token>" line and never
  // seat the board, and netReceive() would start reading binary frames one
  // line late and misalign every packet.
  return (int)strlen(buf);
}

// The same, on the shared connection, with the budget this file guarantees.
inline int netReadBytesBy(WiFiClient &c, uint8_t *out, int want, uint32_t deadline) {
  int got = 0;
  while (got < want) {
    if (!c.available() && !netReadable(c, deadline)) return got;
    const int n = c.read(out + got, (size_t)(want - got));
    if (n <= 0) continue;
    got += n;
  }
  return got;
}

// Ask the relay whether it is there. Called from the main loop, rate-limited,
// because a board that boots before the laptop should find the relay when it
// appears rather than needing a reset.
//
// Written against WiFiClient rather than HTTPClient on purpose. The question
// is only "is the relay answering", and HTTPClient drags in the whole
// request/response machinery to ask it -- including a flush() on teardown
// that reports every empty socket as
//
//     flush(): fail on fd 48, errno: 11, "No more processes"
//
// which is EAGAIN, newlib's strerror text for it being a museum piece: it
// means "nothing to read", not that anything is wrong. Rather than silence a
// misleading error, do not take the path that raises it.
inline bool relayAnswers() {
  WiFiClient c;
  c.setTimeout(2);
  const uint32_t began = millis();
  if (!c.connect(netServer, SERVER_PORT, NET_CONNECT_MS)) {
    // Say why, once: an unreachable relay and a relay that answers wrongly
    // need different things done about them.
    // How long it actually took distinguishes a refused connection (fast) from
    // one that timed out (slow), and those want different things done.
    static bool said = false;
    if (!said) {
      said = true;
      Serial.printf("relay:  no connection to %s:%d after %ums (gave it %dms)\n",
                    SERVER_IP, SERVER_PORT, (unsigned)(millis() - began),
                    NET_CONNECT_MS);
      Serial.printf("        rssi %d dBm; relay must be on 0.0.0.0, not localhost\n",
                    WiFi.RSSI());
    }
    return false;
  }

  c.print(F("GET /healthz HTTP/1.1\r\nHost: " SERVER_IP "\r\nConnection: close\r\n\r\n"));

  // Only the status line matters, and it is the first thing on the wire.
  char line[96];
  netReadLineBy(c, line, sizeof(line), millis() + NET_CONNECT_MS);
  c.stop();
  return strstr(line, "200") != nullptr;
}

// ---- carrying packets ----
//
// The board speaks the same protocol as the browser but in the fixed 8-byte
// wire form, not JSON. That is the same layout ESP-NOW will carry, and it
// needs no parser on this end: a response is frames back to back, so the
// count is its length over eight.
//
// Three things keep this cheap enough to sit in a 50Hz loop:
//   - the poll connection is kept open and reused, so the common case costs
//     no TCP handshake at all;
//   - outbound packets go in one request, not one each -- handing over a
//     fleet is six packets, and six connections to say it would be six times
//     the cost for nothing;
//   - the poll asks for wait=0, so it never parks. The loop still has to
//     draw and read buttons.

static char netToken[40] = {0};
static uint8_t netSelfPeer = 0;
static int netCursor = 0;
static uint32_t netNextRecv = 0;
static WiFiClient netConn;  // kept open, used for send and recv alike

inline bool netSeated() { return netToken[0] != 0; }

static uint32_t netNextJoin = 0;

// Rate-limited, because a refused or unreachable join costs a connect timeout
// and there is no point paying it every pass of a 50Hz loop.
inline bool netJoinDue(uint32_t nowMs) {
  if ((int32_t)(nowMs - netNextJoin) < 0) return false;
  netNextJoin = nowMs + NET_JOIN_EVERY_MS;
  return true;
}

// When the relay was last known to answer. Set by the poll, which is a better
// health check than a probe of its own: it is the traffic that actually
// matters, and it is already happening.
static uint32_t netLastOk = 0;

// Our seat is gone: the relay was restarted, the seats were cleared, or this
// board reconnected and took its own seat back under a new token. Forget it,
// so the join is retried rather than every request failing for ever against a
// token nobody recognises.
inline void netUnseat() {
  netToken[0] = 0;
  netCursor = 0;
  netLastOk = 0;
  netConn.stop();
  Serial.println(F("relay:  seat lost, rejoining"));
}

// Claim a seat. Returns the peer id, or 0 if the relay would not seat us.
inline uint8_t netJoin() {
  WiFiClient c;
  c.setTimeout(2);
  if (!c.connect(netServer, SERVER_PORT, NET_CONNECT_MS)) return 0;

  static const char body[] = "{\"name\":\"" PLAYER_NAME "\"}";
  c.printf("POST /v1/join?fmt=text HTTP/1.1\r\nHost: " SERVER_IP
           "\r\nContent-Type: application/json\r\nContent-Length: %u"
           "\r\nConnection: close\r\n\r\n%s",
           (unsigned)(sizeof(body) - 1), body);

  // Joining happens before the game is interactive, so it can afford longer
  // than a frame -- but it is still this file's budget, not the socket's.
  const uint32_t deadline = millis() + NET_CONNECT_MS;
  char line[96];
  netReadLineBy(c, line, sizeof(line), deadline);
  const bool ok = strstr(line, "200") != nullptr;
  while (netReadLineBy(c, line, sizeof(line), deadline) > 0) {
  }  // headers, to the blank line
  netReadLineBy(c, line, sizeof(line), deadline);  // the body: "<peer> <token>"
  c.stop();

  if (!ok) {
    Serial.println(F("relay:  join refused (both seats taken?)"));
    return 0;
  }
  int peer = 0;
  char tok[40] = {0};
  if (sscanf(line, "%d %39s", &peer, tok) != 2) return 0;
  strncpy(netToken, tok, sizeof(netToken) - 1);
  netSelfPeer = (uint8_t)peer;
  netCursor = 0;
  netLastOk = millis();
  Serial.printf("relay:  seated as peer %d\n", peer);
  return netSelfPeer;
}

// Everything the core wants to say, in one request.
// One connection, kept open, carrying both directions.
//
// Opening a socket is the expensive part on a weak link: the board seats
// fine with three seconds to dial but a fresh connect fails at six hundred
// milliseconds, which is how a whole fleet handover came to be dropped with
// "send failed, 6 packet(s) lost" while the other player waited for it.
// HTTP/1.1 keep-alive lets one socket carry request after request, so the
// dial is paid once rather than several times a second -- and when it must be
// paid, it is given long enough to succeed.
//
// The price is that a response no longer ends at EOF: every reply must be
// drained to its Content-Length or the next request reads the tail of the
// last one.
inline bool netEnsureConn() {
  if (netConn.connected()) return true;
  netConn.stop();
  netConn.setTimeout(2);
  return netConn.connect(netServer, SERVER_PORT, NET_CONNECT_MS);
}

// Read status line and headers. Returns false if the reply is not a 200, and
// fills in the body length and the poll cursor when they are present.
inline bool netReadResponse(int *length, int *next, uint32_t deadline) {
  char line[96];
  *length = 0;
  if (netReadLineBy(netConn, line, sizeof(line), deadline) <= 0) {
    // Nothing, or a torn status line. Either way the socket is no longer
    // trustworthy: a half-read response left on it would be parsed as the
    // next one.
    netConn.stop();
    return false;
  }
  if (strstr(line, "401")) { netUnseat(); return false; }
  const bool ok = strstr(line, "200") != nullptr;
  for (;;) {
    const int n = netReadLineBy(netConn, line, sizeof(line), deadline);
    if (n < 0) { netConn.stop(); return false; }  // ran out of time mid-headers
    if (n == 0) break;                            // the blank line
    if (!strncasecmp(line, "Content-Length:", 15)) *length = atoi(line + 15);
    else if (next && !strncasecmp(line, "X-Next:", 7)) *next = atoi(line + 7);
  }
  return ok;
}

inline void netDrain(int length, uint32_t deadline) {
  uint8_t sink[64];
  while (length > 0) {
    const int want = length > 64 ? 64 : length;
    const int n = netReadBytesBy(netConn, sink, want, deadline);
    if (n <= 0) { netConn.stop(); return; }
    length -= n;
  }
}

// Everything the core wants to say, in one request on the shared connection.
inline void netFlush(game::Game &g) {
  if (netToken[0] == 0) return;

  if (g.outboundCount() == 0) return;

  // Connect BEFORE emptying the core's outbox. takeOutbound() removes packets
  // for good, so draining first and dialling second means a failed dial
  // silently destroys them -- and the fleet handover is six packets in one
  // batch, so a single badly-timed failure used to lose a whole layout and
  // leave the other player on a page with no way past it.
  if (!netEnsureConn()) return;

  uint8_t frames[game::OUTBOX_MAX * game::WIRE_SIZE];
  int n = 0;
  game::NetPacket p{};
  while (n < game::OUTBOX_MAX && g.takeOutbound(p)) {
    game::packWire(p, frames + n * game::WIRE_SIZE);
    n++;
  }
  if (n == 0) return;
  netConn.printf("POST /v1/send HTTP/1.1\r\nHost: " SERVER_IP
                 "\r\nX-Peer-Token: %s\r\nContent-Type: application/octet-stream"
                 "\r\nContent-Length: %d\r\nConnection: keep-alive\r\n\r\n",
                 netToken, n * game::WIRE_SIZE);
  netConn.write(frames, (size_t)n * game::WIRE_SIZE);

  const uint32_t deadline = millis() + NET_READ_MS;
  int length = 0;
  if (!netReadResponse(&length, nullptr, deadline)) {
    Serial.printf("relay:  send rejected, %d packet(s) lost\n", n);
  }
  netDrain(length, deadline);
}

// Collect whatever is waiting and hand it to the core.
inline void netReceive(game::Game &g, uint32_t nowMs) {
  if (netToken[0] == 0) return;
  if ((int32_t)(nowMs - netNextRecv) < 0) return;
  netNextRecv = nowMs + NET_RECV_EVERY_MS;

  if (!netEnsureConn()) {
    netNextRecv = nowMs + NET_RETRY_MS;
    return;
  }
  netConn.printf(
      "GET /v1/recv?since=%d&fmt=bin&wait=0 HTTP/1.1\r\nHost: " SERVER_IP
      "\r\nX-Peer-Token: %s\r\nConnection: keep-alive\r\n\r\n",
      netCursor, netToken);

  const uint32_t deadline = millis() + NET_READ_MS;
  int length = 0, next = -1;
  if (!netReadResponse(&length, &next, deadline)) {
    // Drain even on failure. This file's own rule -- every reply read to its
    // Content-Length -- applies most of all to error replies: an nginx 502 or
    // a Cloudflare error page is kilobytes of HTML, and leaving it queued
    // means the next poll parses "<html>" as a status line and every packet
    // after that is read one response behind.
    netDrain(length, deadline);
    netNextRecv = nowMs + NET_RETRY_MS;
    return;
  }

  // Packets back to back, eight bytes each. The cursor advances per frame,
  // not after the loop: a short read used to leave it where it was, so the
  // relay resent everything and the frames already handed over were delivered
  // twice -- a duplicated shot answered twice, a duplicated fleet counted
  // twice.
  int read = 0;
  for (; read + game::WIRE_SIZE <= length; read += game::WIRE_SIZE) {
    uint8_t w[game::WIRE_SIZE];
    if (netReadBytesBy(netConn, w, game::WIRE_SIZE, deadline) != game::WIRE_SIZE) {
      netConn.stop();  // torn mid-body; the socket is no longer trustworthy
      return;
    }
    g.receive(game::unpackWire(w));
    netCursor++;
  }
  // A body that is not a whole number of frames would otherwise leave its
  // remainder in the socket and misalign the next reply.
  netDrain(length - read, deadline);
  // Only when the header was actually there. It seeds to -1, and assigning it
  // blind is how a reply with no X-Next -- a proxy that stripped it, an error
  // shape that still parsed -- used to rewind the cursor to before the frames
  // just handed over, so the relay resent them all and every shot was
  // answered twice.
  if (next > netCursor) netCursor = next;
  netLastOk = nowMs;  // the relay answered; that is the health check
}

// ---- the lobby ----------------------------------------------------------
//
// Names do not fit in an 8-byte frame, so the lobby is fetched over HTTP as
// lines the board can read with sscanf. What it invites by is a SLOT -- a row
// number in the relay's own list -- so a board never has to hold a
// 36-character UUID for someone it may not even choose.

static uint32_t netNextLobby = 0;
static char netPlayerId[40] = {0};

// This board's identity: derived from the MAC, which is already unique per
// board and survives a reflash. A UUID kept in flash would be another thing
// to lose; the radio's address is one the hardware guarantees.
inline const char *netIdentity() {
  if (netPlayerId[0] == 0) {
    const String mac = WiFi.macAddress();  // 00:11:22:33:44:55
    int n = 0;
    for (unsigned i = 0; i < mac.length() && n < (int)sizeof(netPlayerId) - 8; i++) {
      if (mac[i] != ':') netPlayerId[n++] = (char)tolower(mac[i]);
    }
    // Shaped like a UUID so the relay's list looks the same whoever is in it.
    const char *tail = "-esp-0000-000000000000";
    for (const char *c = tail; *c && n < (int)sizeof(netPlayerId) - 1; c++) netPlayerId[n++] = *c;
    netPlayerId[n] = '\0';
  }
  return netPlayerId;
}

// Announce ourselves to the lobby. The name is the team colour, since a board
// has no keyboard to be given one with.
inline bool netLobbyJoin() {
  WiFiClient c;
  c.setTimeout(2);
  if (!c.connect(netServer, SERVER_PORT, NET_CONNECT_MS)) return false;
  static const char body[] = "{\"name\":\"" PLAYER_NAME "\"}";
  c.printf("POST /v1/lobby/join HTTP/1.1\r\nHost: " SERVER_IP
           "\r\nX-Player-Id: %s\r\nContent-Type: application/json"
           "\r\nContent-Length: %u\r\nConnection: close\r\n\r\n%s",
           netIdentity(), (unsigned)(sizeof(body) - 1), body);
  char line[96];
  netReadLineBy(c, line, sizeof(line), millis() + NET_CONNECT_MS);
  const bool ok = strstr(line, "200") != nullptr;
  c.stop();
  return ok;
}

// The five numbered lobbies, drawn by the core on the top panel. Same page,
// same cursor, on the board and in the browser: the only thing a browser does
// differently is let its player type a name.
//
// Returns true once matched, which this poll also reports -- picking a room
// and being paired in it are one conversation, so a board has one endpoint to
// keep in step rather than two.
// The lobby version this board last saw. The relay answers "nothing changed"
// when it still holds, which is almost every poll: a list of five rooms is
// asked for once a second and changes perhaps twice a minute.
static int netLobbyVersion = -1;

inline bool netRoomsPoll(game::Game &g, uint32_t nowMs) {
  if ((int32_t)(nowMs - netNextLobby) < 0) return false;
  netNextLobby = nowMs + 1000;

  // On the connection the board already holds. A fresh socket a second was a
  // TCP handshake a second, on a chip with a handful of sockets and a poll
  // loop that also has to draw two panels.
  if (!netEnsureConn()) return false;
  netConn.printf("GET /v1/rooms?fmt=text&since=%d HTTP/1.1\r\nHost: " SERVER_IP
                 "\r\nX-Player-Id: %s\r\nConnection: keep-alive\r\n\r\n",
                 netLobbyVersion, netIdentity());

  WiFiClient &c = netConn;
  const uint32_t deadline = millis() + NET_READ_MS * 4;
  // Long enough for the longest room line there can be: "n count yours " and
  // two names, each capped by the relay at game::LOBBY_NAME_CHARS BYTES, with
  // a pipe between them. A line that did not fit was truncated, and the count
  // of bytes consumed then undershot Content-Length for ever -- the loop read
  // past the end of the body, timed out, and re-dialled the connection on
  // every poll from then on.
  char line[16 + 2 * game::LOBBY_NAME_CHARS + 8];
  int status = 0, length = 0, version = netLobbyVersion;
  if (netReadLineBy(c, line, sizeof(line), deadline) < 0) {
    c.stop();
    return false;
  }
  sscanf(line, "HTTP/1.%*d %d", &status);
  for (;;) {
    const int n = netReadLineBy(c, line, sizeof(line), deadline);
    if (n == 0) break;   // the blank line: the headers are done
    if (n < 0) {
      // Out of time mid-headers. Not the same thing as the blank line, and
      // treating it as one used to send this function on to parse a body that
      // had not arrived -- leaving the rest of the reply queued on a socket
      // that is kept alive, so the next poll read it as a status line.
      netConn.stop();
      return false;
    }
    sscanf(line, "Content-Length: %d", &length);
    sscanf(line, "X-Lobby-Version: %d", &version);
  }
  netLobbyVersion = version;
  netLastOk = millis();
  if (status == 204 || length == 0) return false;  // nothing changed

  // Read exactly Content-Length. The socket is kept alive now, so a body left
  // half-read is not a closed connection -- it is the next reply's status line
  // arriving in the middle of this one's, and every poll after that reading
  // one response behind.
  bool matched = false;
  int consumed = 0;
  g.clearRooms();
  while (consumed < length) {
    const int n = netReadLineBy(c, line, sizeof(line), deadline);
    if (n < 0) {
      netConn.stop();  // torn mid-body; the socket is no longer trustworthy
      return false;
    }
    consumed += n + 1;  // the newline the relay wrote
    if (line[0] == 0) continue;

    char seat[8], team[8];
    int nameAt = 0;
    if (sscanf(line, "matched %7s %7s %39s %n", seat, team, netToken, &nameAt) >= 3) {
      netSelfPeer = (seat[0] == 'b') ? game::PEER_B : game::PEER_A;
      g.setSelfPeer(netSelfPeer);
      g.setTeam(team[0] == 'b' ? game::Team::Black : game::Team::Red);
      // Who we are playing, for the waiting page and the verdict. This poll
      // returns as soon as it is told about the match, so a room row naming
      // the two of us may never be read -- the name comes with the pairing
      // or not at all.
      if (nameAt > 0 && line[nameAt] != '\0') g.setNames(PLAYER_NAME, line + nameAt);
      netCursor = 0;
      matched = true;
      continue;  // read the rest of the body; do not leave it in the socket
    }
    int mine = 0;
    if (sscanf(line, "you %d", &mine) == 1) {
      g.setMyRoom(mine);
      continue;
    }
    // "n count yours a|b" -- a pipe rather than a space between the names,
    // because a browser player's name may well have a space in it and only
    // one of the two can be the separator.
    int n2 = 0, count = 0, yours = 0, at = 0;
    if (sscanf(line, "%d %d %d %n", &n2, &count, &yours, &at) == 3 && at > 0) {
      char *a = line + at;
      char *b = strchr(a, '|');
      if (b) *b++ = '\0';
      g.setRoom(n2, (uint8_t)count, a, b ? b : "", yours != 0);
      // Both players of OUR room, by name, so the verdict can say whose win
      // it is. Ours is the one that is not the other one.
      if (yours && count >= 2) {
        const char *me = PLAYER_NAME;
        const char *them = strcmp(a, me) == 0 ? (b ? b : "") : a;
        g.setNames(me, them);
      }
    }
  }
  return matched;
}

// Sit down in a numbered room. A full one is refused by the relay, which is
// the check that counts: two boards pressing centre on the same row in the
// same instant is what a greyed-out row cannot cover.
inline bool netRoomJoin(int n) {
  WiFiClient c;
  c.setTimeout(2);
  if (!c.connect(netServer, SERVER_PORT, NET_CONNECT_MS)) return false;
  c.printf("POST /v1/rooms/join?n=%d HTTP/1.1\r\nHost: " SERVER_IP
           "\r\nX-Player-Id: %s\r\nContent-Length: 0"
           "\r\nConnection: close\r\n\r\n",
           n, netIdentity());
  char line[96];
  netReadLineBy(c, line, sizeof(line), millis() + NET_READ_MS * 2);
  const bool ok = strstr(line, "200") != nullptr;
  c.stop();
  return ok;
}


// Stand up again, back to the list of five. The relay frees the room, so the
// other player's next poll shows it empty rather than showing someone who has
// gone.
// The board as it stands, for a player who has just walked back into their
// lobby. Frames, not JSON: the same 8-byte records the board already reads,
// so this needs no parser -- and one exchange puts back the whole position
// however long the match has been running.
//
// The cursor comes back with it (X-Next). Polling from where the board left
// off would replay every shot of the match on top of the position it was just
// handed; polling from the end is right precisely because the position is
// already up to date.
inline bool netReconnect(game::Game &g) {
  if (!netSeated() || !netEnsureConn()) return false;
  netConn.printf("GET /v1/reconnect?fmt=bin HTTP/1.1\r\nHost: " SERVER_IP
                 "\r\nX-Peer-Token: %s\r\nConnection: keep-alive\r\n\r\n",
                 netToken);

  const uint32_t deadline = millis() + NET_READ_MS * 4;
  int length = 0, next = -1;
  if (!netReadResponse(&length, &next, deadline)) {
    netDrain(length, deadline);
    return false;
  }

  int read = 0, applied = 0;
  for (; read + game::WIRE_SIZE <= length; read += game::WIRE_SIZE) {
    uint8_t w[game::WIRE_SIZE];
    if (netReadBytesBy(netConn, w, game::WIRE_SIZE, deadline) != game::WIRE_SIZE) {
      netConn.stop();
      return false;
    }
    g.receive(game::unpackWire(w));
    applied++;
  }
  netDrain(length - read, deadline);
  if (next >= 0) netCursor = next;
  netLastOk = millis();
  Serial.printf("lobby:  rejoined -- %d frames of board restored\n", applied);
  return applied > 0;
}

inline void netRoomLeave() {
  WiFiClient c;
  c.setTimeout(2);
  if (!c.connect(netServer, SERVER_PORT, NET_CONNECT_MS)) return;
  c.printf("POST /v1/rooms/leave HTTP/1.1\r\nHost: " SERVER_IP
           "\r\nX-Player-Id: %s\r\nContent-Length: 0"
           "\r\nConnection: close\r\n\r\n",
           netIdentity());
  char line[96];
  netReadLineBy(c, line, sizeof(line), millis() + NET_READ_MS * 2);
  c.stop();
}

inline void netPoll(uint32_t nowMs) {
  if (netState == NetState::Off) return;
  if (WiFi.status() != WL_CONNECTED) {
    netState = NetState::Joining;
    return;
  }

  // Once seated, the poll IS the health check. A separate probe on its own
  // socket only competes with it for the handful the chip has, and the two
  // disagreeing is what made the link flap between "up" and "not answering"
  // every couple of seconds.
  if (netSeated()) {
    netState = (nowMs - netLastOk < NET_STALE_MS) ? NetState::Ready : NetState::NoServer;
    return;
  }

  if ((int32_t)(nowMs - netNextProbe) < 0) return;
  netNextProbe = nowMs + NET_PROBE_EVERY_MS;

  const NetState was = netState;
  netState = relayAnswers() ? NetState::Ready : NetState::NoServer;
  if (netState != was) {
    Serial.printf("relay:  %s at http://%s:%d\n",
                  netState == NetState::Ready ? "up" : "not answering",
                  SERVER_IP, SERVER_PORT);
  }
}
