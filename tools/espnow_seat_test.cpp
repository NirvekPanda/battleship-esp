// Two boards deciding, without asking each other, which of them is seat A.
//
// They cannot negotiate it: whoever spoke first would win, and seat A shoots
// first. So the rule has to be a function of the two identities that both
// ends compute the same way -- the same reasoning as the relay ordering by
// UUID.
#include <cstdio>
#include <cstring>
#include <cstdint>

#include "netplay.h"

using namespace game;

// The rule under test, copied from include/espnow.h -- which cannot be
// included here because it is firmware.
static uint8_t seatFor(const uint8_t *self, const uint8_t *other) {
  return memcmp(self, other, 6) < 0 ? PEER_A : PEER_B;
}

static int failures = 0;
static void check(bool ok, const char *what) {
  if (!ok) { printf("  FAIL  %s\n", what); failures++; }
}

int main() {
  const uint8_t low[6]  = {0x24, 0x6F, 0x28, 0x00, 0x00, 0x01};
  const uint8_t high[6] = {0x7C, 0x4F, 0xAD, 0x4C, 0x08, 0xE8};

  // Each board asks the question from its own side.
  const uint8_t seatOfLow  = seatFor(low, high);
  const uint8_t seatOfHigh = seatFor(high, low);

  check(seatOfLow == PEER_A, "the lower address takes seat A");
  check(seatOfHigh == PEER_B, "and the higher takes seat B");
  check(seatOfLow != seatOfHigh, "the two boards never claim the same seat");

  // Asking in the other order must not change the answer: that is the whole
  // point, since neither board knows which of them asked first.
  check(seatFor(high, low) == PEER_B, "the answer does not depend on who asks");
  check(seatFor(low, high) == PEER_A, "nor on the order of the arguments");

  // Addresses that differ only in the last byte still separate cleanly.
  const uint8_t a[6] = {1, 2, 3, 4, 5, 6};
  const uint8_t b[6] = {1, 2, 3, 4, 5, 7};
  check(seatFor(a, b) == PEER_A && seatFor(b, a) == PEER_B,
        "a one-byte difference is enough to decide");

  if (failures == 0) { printf("espnow-seat: all checks passed\n"); return 0; }
  printf("espnow-seat: %d CHECK(S) FAILED\n", failures);
  return 1;
}
