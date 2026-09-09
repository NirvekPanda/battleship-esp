#include <Arduino.h>
#include "config.h"
#include "controls.h"
#include "display.h"
#include "espnow.h"
#include "net.h"
#include "game/game.h"

// Device backend. The game itself lives in src/game/ and knows nothing about
// the ESP32; this file only samples the switch and pushes the two Screens at
// the panels. web/ runs the identical core under WASM.
game::Game gameCore;

void setup() {
  Serial.begin(115200);
  // The native USB CDC port only exists once a host opens it, so a banner
  // printed the instant setup() runs is written into the void. Wait briefly
  // for the monitor to attach, but never forever -- the board has to boot
  // with nothing plugged into it.
  const uint32_t waitUntil = millis() + 1500;
  while (!Serial && millis() < waitUntil) delay(10);

  controlsBegin();
  displayBegin();
  netBegin();
  gameCore.begin();
  displayPush(gameCore.top(), gameCore.bottom());
}

// Print the switch state whenever it changes, so a miswired or dead
// direction is obvious on the monitor.
void traceButtons(const game::Input &in) {
  static uint8_t prev = 0xFF;
  uint8_t now = (in.up << 0) | (in.right << 1) | (in.down << 2) |
                (in.left << 3) | (in.center << 4);
  if (now == prev) return;
  prev = now;
  Serial.printf("buttons: U%d R%d D%d L%d C%d\n", in.up, in.right, in.down,
                in.left, in.center);
}

void loop() {
  controlsPoll();  // fold in any arrow keys typed at the serial monitor

  // Tell the core how the link is getting on. It cannot open a socket and
  // does not know what one is; it only decides what to draw and when the
  // connecting page may move on.
  const uint32_t now = millis();

  // ---- board to board ----
  //
  // Chosen on the title screen, before any of the online machinery is
  // touched. The core is handed packets and asked for packets exactly as it
  // is online: the transport is the only thing that differs, which is what
  // the seam in netplay.h exists for.
  if (gameCore.mode() == game::Mode::Offline) {
    static bool announced = false;
    if (espnow::begin()) {
      espnow::discover(now);
      espnow::receive(gameCore);
      espnow::flush(gameCore);
      if (espnow::linked() && !announced) {
        announced = true;
        espnow::assignSeats(gameCore);
      }
      // Linked means a board has answered, which is what the connecting page
      // is waiting for.
      gameCore.setLink(espnow::linked() ? game::Link::Online : game::Link::Connecting);
    } else {
      gameCore.setLink(game::Link::Offline);
    }

    game::Input in;
    in.up = upPressed(); in.right = rightPressed(); in.down = downPressed();
    in.left = leftPressed(); in.center = centerPressed();
    traceButtons(in);
    gameCore.tick(in, now);
    displayPush(gameCore.top(), gameCore.bottom());
    delay(20);
    return;
  }

  netPoll(now);
  gameCore.setLink(netState == NetState::Ready    ? game::Link::Online
                   : netState == NetState::Off    ? game::Link::Offline
                                                  : game::Link::Connecting);

  // ---- the lobby ----
  //
  // A seat is a role in a match, handed out when two players choose each
  // other, so the board announces itself to the LOBBY first and only holds a
  // seat once it has an opponent. Claiming one on arrival is what used to
  // limit the whole relay to two connected clients.
  // Not before the player has answered the title's question. The board is in
  // Online mode by default, so a reachable relay used to throw it onto the
  // lobby list mid-thought -- and made OFFLINE unreachable whenever a relay
  // happened to be running.
  static bool inLobby = false;
  const bool pastTitle = gameCore.page() != game::Page::Start;
  if (pastTitle && !netSeated() && netState == NetState::Ready && !inLobby &&
      netJoinDue(now)) {
    if (netLobbyJoin()) {
      inLobby = true;
      gameCore.setPage(game::Page::Rooms);
      Serial.printf("lobby:  joined as %s\n", netIdentity());
    }
  }
  // Holding centre to walk out, from anywhere in a game. Read outside the
  // lobby-page block on purpose: leaving a MATCH is the case that matters,
  // and a check that only ran on the lobby list would never see it.
  if (gameCore.takeRoomLeave()) {
    netRoomLeave();
    netUnseat();
    inLobby = false;
    Serial.println(F("lobby:  left -- back to the lobby list"));
  }

  // A seat that has gone means the match is over -- decided, or its room
  // cleared -- so the board goes back to the lobby list with a fresh core
  // rather than keeping the last game's fleet and shots on screen.
  //
  // The verdict comes first, though. A match ends by the relay clearing the
  // lobby, which takes the seat with it, so the moment the seat goes is a
  // moment after WIN or LOSE went up, and going straight back to the list
  // takes the answer off the screen before it can be read.
  //
  // The pending flag is what remembers that: "the seat has gone and we still
  // owe ourselves a reset". Trying to hold this in wasSeated instead meant
  // the very next line cleared it, so the reset never came and the board sat
  // on the verdict for ever -- the flag has to outlive the pass that sets it.
  static bool resetPending = false;
  static bool wasSeated = false;
  if (wasSeated && !netSeated()) {
    resetPending = true;
  }
  wasSeated = netSeated();

  // Not while the verdict is up: that page shows the answer, then both
  // boards, and leaves when the player presses. The seat being gone is not a
  // reason to take it off them.
  if (resetPending && gameCore.page() != game::Page::Over) {
    resetPending = false;
    Serial.println(F("lobby:  seat gone -- back to the lobby list"));
    gameCore.begin();
    gameCore.setPage(game::Page::Rooms);
    gameCore.setMyRoom(0);
    inLobby = false;
  }

  if (inLobby && !netSeated() && gameCore.page() == game::Page::Rooms) {
    if (netRoomsPoll(gameCore, now)) {
      // Seated. If there is already a match in this lobby -- because this is
      // the one we walked out of, or restarted out of -- the relay hands the
      // board back and we are put down where we left off. A fresh match has
      // no board to hand back, and we start from placement.
      if (netReconnect(gameCore)) {
        Serial.println(F("lobby:  rejoined -- back on the board"));
      } else {
        Serial.println(F("lobby:  matched -- starting"));
        gameCore.setPage(game::Page::Place);
      }
    }
    // The gesture is the core's -- a press joins, a hold goes back -- so this
    // only carries the decision to the relay. Reading the button here as well
    // is what let the press that chose ONLINE fall through and join room 1.
    if (const int room = gameCore.takeRoomJoin()) {
      if (netRoomJoin(room)) {
        gameCore.setMyRoom(room);
        Serial.printf("lobby:  joined lobby %d\n", room);
      }
    }

  }

  // Claim a seat the first time the relay answers, and tell the core which
  // player it is -- that decides who shoots first. Retried on every pass
  // until it takes, so a board that boots before the laptop still gets in.
  // Whether we hold a seat is net.h's to say, not a flag of our own: a seat
  // can be lost after it was granted -- the relay restarted, the seats were
  // cleared -- and the retry has to start again from that.
  // netJoin() -- claiming a seat directly, with no lobby in front of it -- is
  // what a board does when it is pointed at a bare relay. The rooms path
  // above is what runs when there is a lobby, which is every relay this repo
  // builds.

  // Carry the core's packets. It owns no transport and knows nothing of
  // sockets; this is the whole of the board's side of the link.
  if (netSeated()) {
    netReceive(gameCore, now);
    netFlush(gameCore);
  }

  game::Input in;
  in.up     = upPressed();
  in.right  = rightPressed();
  in.down   = downPressed();
  in.left   = leftPressed();
  in.center = centerPressed();

  traceButtons(in);

  gameCore.tick(in, now);
  displayPush(gameCore.top(), gameCore.bottom());

  // A frame that takes too long is the difference between a cursor that moves
  // when you press a direction and one that seems stuck. Every network call in
  // this loop is time-boxed for that reason, so if a frame overruns anyway it
  // is worth saying which one and by how much -- reported at most once a
  // second, since the whole point is not to make a slow loop slower.
  // Say which game is being played the moment it is decided. Solo and linked
  // look identical until someone is left waiting, and by then the cause is
  // several screens back.
  static bool announced = false;
  // The same ordinal trap as setPage(): Waiting, Lobby and Rooms are numbered
  // after Starting but come before it in the flow, so ">= Starting" fires on
  // the lobby list.
  const game::Page pg = gameCore.page();
  if (!announced && (pg == game::Page::Starting || pg == game::Page::Match)) {
    announced = true;
    Serial.printf("match:  %s (link %s, seat %s)\n",
                  gameCore.practice() ? "SOLO -- no opponent will be waited for"
                                      : "linked, two players",
                  gameCore.everLinked() ? "seen" : "never seen",
                  netSeated() ? "held" : "none");
  }

  const uint32_t spent = millis() - now;
  static uint32_t nextWarn = 0;
  if (spent > SLOW_FRAME_MS && (int32_t)(millis() - nextWarn) >= 0) {
    nextWarn = millis() + 1000;
    Serial.printf("slow:   frame took %ums (link %s)\n", (unsigned)spent,
                  netState == NetState::Ready ? "up" : "down");
  }

  delay(20);
}
