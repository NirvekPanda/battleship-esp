#pragma once

// ---- Screens ----
// The C3 has a single I2C peripheral (SOC_I2C_NUM == 1). The top panel owns
// it; the bottom panel runs on bit-banged I2C over its own pin pair, so both
// modules can keep the same hardwired address.
#define I2C_SCL 0
#define I2C_SDA 1
#define S2_SCL 3
#define S2_SDA 4
#define I2C_HZ 400000

#define OLED_ADDR 0x3C  // both modules, on their own separate buses
#define SCREEN_W 128
#define SCREEN_H 64

// ---- 5-way switch ----
// A, B, C, D are up, right, down, left, going CLOCKWISE. That order is a
// contract, not a naming convention: controls.h walks these four as a ring
// and applies BUTTON_ROTATION as a shift along it, so getting the handedness
// wrong mirrors an axis at every rotation instead of turning the directions.
// B and D are the horizontal pair, and on this switch they run the opposite
// way round from the labels -- right is GPIO5, left is GPIO20.
#define BTN_A 10
#define BTN_B 5
#define BTN_C 21

// common
#define BTN_D 20
#define BTN_CENTER 6
// switch COM wires straight to GND; buttons use INPUT_PULLUP

// ---- Panel orientation ----
// Which way up a module is mounted, named for where its header pins end up.
// PINS_UP is the SSD1306's own default scan order; PINS_DOWN turns the glass
// through 180 degrees. Nothing rotates in software: the controller has a
// segment remap and a COM scan direction of its own, so a flipped panel costs
// two init bytes and no per-frame work, and displayPush() stays a memcpy.
//
// Only 0 and 180 exist. A panel is 128x64, so a quarter turn is not a
// same-shape operation -- it would need a 64x128 layout, not a rotation.
#define PANEL_PINS_UP 0
#define PANEL_PINS_DOWN 1

#define PANEL1_ROTATION PANEL_PINS_UP   // top panel, hardware I2C
#define PANEL2_ROTATION PANEL_PINS_UP   // bottom panel, bit-banged I2C

// ---- Switch orientation ----
// How far clockwise the 5-way switch is mounted from the upright position the
// pin map above assumes. The directions are relabelled to match, so a switch
// fitted sideways still moves the crosshair the way it points. Centre is on
// the axis of rotation and never moves.
#define BTN_ROT_0 0
#define BTN_ROT_90 1
#define BTN_ROT_180 2
#define BTN_ROT_270 3

// The switch is mounted a quarter turn clockwise: the button wired as BTN_D
// ("left" upright) is the one now pointing up.
#define BUTTON_ROTATION BTN_ROT_90

// ---- Match relay ----
// Where the Go relay in server/ is listening. It serves the WASM page and
// routes packets between whoever is playing; see server/PROTOCOL.md.
//
// This must be the HOST address of the machine running the relay, and the
// relay must be started with `make -C server run ADDR=0.0.0.0:8080` so it
// listens beyond loopback.
//
// Two addresses that look plausible and are not:
//   127.0.0.1        loopback -- to this board that means the board itself
//   192.168.86.255   the .255 of a /24 is the SUBNET BROADCAST, not a host;
//                    a TCP connection to it does not reach anyone
// What this board calls itself when it claims a seat. It is the seat's
// identity, not just a label: the relay lets a client take back a seat it
// already holds, which is what makes reflashing work instead of the board
// finding the table full of itself.
//
// So changing this name STRANDS the seat held under the old one -- the new
// name no longer matches, and the old seat sits there until it times out.
// `make upload` drops it first for exactly this reason.
#define PLAYER_NAME "esp-red"

#define SERVER_IP "192.168.86.220"
#define SERVER_PORT 8080

// ---- WiFi ----
// Fill these in to put the board on the network. Left empty on purpose: with
// no SSID the firmware skips WiFi entirely and plays offline rather than
// blocking at boot, so an unconfigured board is still usable on a bench.
//
// The C3 has no 5GHz radio. A 2.4GHz network is not a preference here, it is
// the only thing that will associate.
// Credentials live in secrets.h, which git ignores, so a real SSID and
// password are never written into the repository's history -- a mistake that
// cannot be undone by a later commit. A checkout without that file still
// builds: WiFi is simply skipped and the board plays offline.
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID ""
#define WIFI_PASS ""
#endif

// How long to wait for an association before giving up and playing offline.
#define WIFI_TIMEOUT_MS 60000

// Drawing constants live in src/game/, which is hardware-independent; this
// header is pins, orientation and where the relay lives.
