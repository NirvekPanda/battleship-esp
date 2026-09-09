#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <string.h>

#include "../src/game/game.h"
#include "config.h"

// Board to board, with no relay in the middle.
//
// This is what the 8-byte frame was fixed for: an ESP-NOW payload is read
// straight off the radio with no length prefix, no parser and no allocation.
// The core cannot tell this transport from the HTTP one -- it hands packets
// out and takes packets in, and something else decides how they travel. That
// seam is why the whole online game works here unchanged.
//
// Two boards find each other by broadcasting a short hello. A game frame is
// eight bytes and a hello is four, so the two are told apart by length alone
// and a hello can never be mistaken for a move.

namespace espnow {

constexpr uint8_t HELLO[4] = {'B', 'S', 'H', 'P'};
constexpr uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
constexpr uint32_t HELLO_MS = 400;
constexpr int INBOX_MAX = 16;

// Written by the WiFi task in onRecv() and read by the main loop, so both are
// volatile and the ORDER of the two writes matters: the address is filled in
// first and the flag published last, or a loop that saw havePeer between the
// two would send frames to half an address -- and seatFor() would compute a
// seat from it, which is how both boards could decide they were seat A.
static volatile uint8_t peerMac[6] = {0};
static uint8_t selfMac[6] = {0};
static volatile bool havePeer = false;
static bool started = false;
static uint32_t nextHello = 0;

// Filled by the radio callback, drained by the loop. The callback runs on the
// WiFi task, so the two ends are kept apart by a spinlock rather than by
// hoping they never overlap.
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static game::NetPacket inbox[INBOX_MAX];
static volatile int inCount = 0;
// A peer that turned up in the receive callback and still has to be
// registered from the main loop.
static volatile bool pendingPeer = false;
// A hello to send back, queued by the receive callback for the main loop:
// esp_now_send is not something to call from inside the callback.
static volatile bool pendingHello = false;

inline bool linked() { return havePeer; }

// Who is seat A. Decided by comparing MAC addresses, for the same reason the
// relay decides it by UUID order: seat A shoots first, so it cannot be left to
// whichever board happened to speak first, and both ends must reach the same
// answer without asking each other.
inline uint8_t seatFor(const uint8_t *self, const uint8_t *other) {
  return memcmp(self, other, 6) < 0 ? game::PEER_A : game::PEER_B;
}

inline void addPeer(const uint8_t *mac) {
  esp_now_peer_info_t p = {};
  memcpy(p.peer_addr, mac, 6);
  p.channel = 0;  // whatever channel we are already on
  p.ifidx = WIFI_IF_STA;
  p.encrypt = false;
  esp_now_add_peer(&p);
}

inline void onRecv(const uint8_t *mac, const uint8_t *data, int len) {
  if (len == sizeof(HELLO) && memcmp(data, HELLO, sizeof(HELLO)) == 0) {
    if (!havePeer) {
      for (int i = 0; i < 6; i++) peerMac[i] = mac[i];
      // Registering the peer is an esp_now call, which ESP-IDF asks not to be
      // made from inside the receive callback. It is deferred to the main
      // loop, which is also where every other esp_now call this file makes
      // happens -- and the flag is published last, after the address is
      // whole, so the loop never acts on half of one.
      pendingPeer = true;
      // Answer it. discover() stops broadcasting the moment a peer is known,
      // so a board that HEARS a hello before it has sent one would never send
      // one at all -- and the other board, which only ever learns a MAC from
      // a hello, would drop every frame it was then sent. Both would sit on
      // the connecting page for ever, each believing it had found the other.
      pendingHello = true;
      havePeer = true;
    }
    return;
  }
  if (len != game::WIRE_SIZE || !havePeer) return;

  portENTER_CRITICAL_ISR(&lock);
  if (inCount < INBOX_MAX) inbox[inCount++] = game::unpackWire(data);
  portEXIT_CRITICAL_ISR(&lock);
}

// Bring the radio up. Safe to call when already joined to an access point:
// ESP-NOW rides the channel the station is on, so two boards on the same
// network are on the same channel by construction.
inline bool begin() {
  if (started) return true;
  if (WiFi.getMode() == WIFI_MODE_NULL) WiFi.mode(WIFI_STA);
  esp_wifi_start();
  if (esp_now_init() != ESP_OK) {
    Serial.println(F("espnow: could not start the radio"));
    return false;
  }
  esp_now_register_recv_cb(onRecv);
  addPeer(BROADCAST);  // so a hello can be sent to everyone at once
  esp_read_mac(selfMac, ESP_MAC_WIFI_STA);
  started = true;
  Serial.printf("espnow: listening as %02X:%02X:%02X:%02X:%02X:%02X\n",
                selfMac[0], selfMac[1], selfMac[2], selfMac[3], selfMac[4], selfMac[5]);
  return true;
}

// Say hello until somebody answers, then stop: a board that kept shouting
// after the game began would only be adding noise to the channel it is
// playing on.
inline void discover(uint32_t nowMs) {
  if (havePeer || !started) return;
  if ((int32_t)(nowMs - nextHello) < 0) return;
  nextHello = nowMs + HELLO_MS;
  esp_now_send(BROADCAST, HELLO, sizeof(HELLO));
}

// Whatever the core wants to say, straight onto the radio. One frame per
// packet: there is no connection to amortise here, so batching would only
// delay the first of them.
inline void flush(game::Game &g) {
  if (!havePeer) return;
  // The hello owed to a board that said hello first.
  if (pendingHello) {
    pendingHello = false;
    esp_now_send(BROADCAST, HELLO, sizeof(HELLO));
  }
  // Registering the peer, on the loop rather than in the callback.
  if (pendingPeer) {
    pendingPeer = false;
    uint8_t mac[6];
    for (int i = 0; i < 6; i++) mac[i] = peerMac[i];
    addPeer(mac);
  }
  game::NetPacket p{};
  while (g.takeOutbound(p)) {
    uint8_t w[game::WIRE_SIZE];
    game::packWire(p, w);
    uint8_t to[6];
    for (int i = 0; i < 6; i++) to[i] = peerMac[i];
    esp_now_send(to, w, sizeof(w));
  }
}

inline void receive(game::Game &g) {
  for (;;) {
    game::NetPacket p{};
    portENTER_CRITICAL(&lock);
    const bool any = inCount > 0;
    if (any) {
      p = inbox[0];
      for (int i = 1; i < inCount; i++) inbox[i - 1] = inbox[i];
      inCount--;
    }
    portEXIT_CRITICAL(&lock);
    if (!any) return;
    g.receive(p);
  }
}

// Called once a peer appears: both boards work out their seats from the two
// MAC addresses and reach the same answer independently.
inline void assignSeats(game::Game &g) {
  uint8_t peer[6];
  for (int i = 0; i < 6; i++) peer[i] = peerMac[i];
  const uint8_t seat = seatFor(selfMac, peer);
  g.setSelfPeer(seat);
  // Seat A is red, seat B is black -- a rule, so that neither board has to
  // ask the other what colour it is.
  g.setTeam(seat == game::PEER_A ? game::Team::Red : game::Team::Black);
  Serial.printf("espnow: paired with %02X:%02X:%02X:%02X:%02X:%02X -- seat %c\n",
                peerMac[0], peerMac[1], peerMac[2], peerMac[3], peerMac[4], peerMac[5],
                seat == game::PEER_A ? 'a' : 'b');
}

}  // namespace espnow
