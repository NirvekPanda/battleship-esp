# Host (WASM) build of the game core, plus the two things that are awkward to
# run by hand: `serve`, which brings up the page and the relay together on one
# port, and `upload`, which frees the board's stale seat before flashing.
#
# The firmware itself is built by PlatformIO (`pio run`); `make upload` only
# wraps it. `make help` lists every target, `make help1` gives the order to
# run them in.
EMCC ?= emcc
SRC  := src/host/wasm_main.cpp src/game/screen.cpp src/game/game.cpp \
        src/game/place_page.cpp src/game/match_page.cpp src/game/fleet.cpp \
        src/game/grid.cpp src/game/sprite.cpp
CORE := src/game/screen.cpp src/game/game.cpp src/game/place_page.cpp \
        src/game/match_page.cpp src/game/fleet.cpp src/game/grid.cpp \
        src/game/sprite.cpp
OUT  := web/sim.js

EXPORTS := _sim_begin,_sim_set_input,_sim_set_link,_sim_set_self_peer,_sim_receive,_sim_take_outbound,_sim_my_turn,_sim_aim_col,_sim_aim_row,_sim_in_flight,_sim_set_page,_sim_page,_sim_set_hit_ship,_sim_tick,_sim_top,_sim_bottom,_sim_buf_size,_sim_width,_sim_height,_sim_lobby_name_max,_sim_clear_rooms,_sim_set_room,_sim_room_a_buf,_sim_room_b_buf,_sim_take_room_join,_sim_take_room_leave,_sim_set_my_room,_sim_room_count,_sim_set_names

.PHONY: help debug help1 web play serve upload clean check test test-core

# A bare `make` prints the short help. The whole point of splitting help from
# help-debug is that the way in should be obvious, and a bare `make` is what
# someone tries first. Stated outright rather than left to target order, which
# would otherwise make it whichever target happens to come first.
.DEFAULT_GOAL := help

# `help` lists what each target does. `help1` answers the other question --
# what to run, and in what order, to get a match actually running. The two
# ends have to be brought up in the right sequence, and the failures when they
# are not are silent ones, so the order is worth writing down.
# `make help` is the short list: the three commands that cover playing the
# game. `make help debug` is everything else -- diagnostics, one-off tools and
# the internals -- kept out of the way rather than deleted.
#
# The split is done by looking at the goals on the command line, so `make help`
# and `make help debug` print different things rather than one printing both.
DEBUG_HELP := $(filter debug,$(MAKECMDGOALS))

help:
ifeq ($(DEBUG_HELP),)
	@echo "battleship-esp"
	@echo ""
	@echo "  make play      build everything and run it: the page and the relay"
	@echo "                 together on http://localhost:8080"
	@echo "                 Open that address. A second tab is the second player."
	@echo "  make upload    flash the board and watch its serial output"
	@echo ""
	@echo "  While play is running, the relay dashboard is at"
	@echo "  http://localhost:8081 -- server output, who is connected,"
	@echo "  buttons to send both players to a page, and a CSV of the match."
	@echo "  make test      build every target and run every check"
	@echo ""
	@echo "  make help debug    diagnostics, tuning and everything else"
else
	@echo "Startup"
	@echo "  make play                 build the page, then serve it AND the relay"
	@echo "                            on :8080, bound to every interface so the"
	@echo "                            board can reach it. Stops whatever already"
	@echo "                            holds the port. 'make serve' is the same."
	@echo "  make play PORT=9000       somewhere else"
	@echo ""
	@echo "Flashing"
	@echo "  make upload               free the board's stale seat, flash, monitor"
	@echo "  make upload DROP=esp32    free a seat left under an EARLIER name"
	@echo "  make upload DROP=         flash without touching seats"
	@echo "  pio run                   build the firmware only"
	@echo ""
	@echo "  The board is PLAYER_NAME in include/config.h, now \"$(DROP)\". A seat"
	@echo "  can be taken back by the name holding it, so an ordinary reflash"
	@echo "  needs nothing -- RENAMING the player is what strands the old seat."
	@echo "  WiFi credentials go in include/secrets.h (gitignored); copy"
	@echo "  include/secrets.example.h to it."
	@echo ""
	@echo "Checks (host compiler only, no toolchain needed)"
	@echo "  make check                type-check the portable core, ~1s"
	@echo "  make test                 everything: type-check, the eleven host"
	@echo "                            suites, the WASM build, the firmware and"
	@echo "                            the relay's own tests"
	@echo "  make web                  build the WASM core + TypeScript only"
	@echo "  make test-core            just the host suites, no builds"
	@echo "  make clean                remove generated output and test binaries"
	@echo "  make help1                the same startup, written as an ordered"
	@echo "                            walkthrough with the reason for each step"
	@echo ""
	@echo "Dashboard (http://localhost:8081, served by the relay)"
	@echo "  server output, connected players, force-page buttons and a CSV"
	@echo "  export of every packet with timestamps."
	@echo "  make -C server dashboard    serve it through nginx instead"
	@echo "                              (brew install nginx; see nginx.conf)"
	@echo ""
	@echo "Relay, when you want it on its own"
	@echo "  make -C server run        loopback only"
	@echo "  make -C server run-lan    every interface (what 'make play' uses)"
	@echo "  make -C server run-quiet  without a line per packet"
	@echo "  make -C server run-espnow gameplay reserved for ESP-NOW"
	@echo "  make -C server seats-clear  free both seats when stale clients hold them"
	@echo "  make -C server kill-port    stop whatever holds the port"
	@echo "  make -C server test"
	@echo ""
	@echo "When it does not work"
	@echo "  501 Unsupported method    the page is on a plain file server."
	@echo "                            Open :8080, not :8000."
	@echo "  errno 104 on the board    the relay is on localhost only. Use"
	@echo "                            make play, which binds every interface."
	@echo "  errno 11 EAGAIN           newlib's wording for 'would block'."
	@echo "                            Harmless."
	@echo "  both seats are taken      the relay names who holds them; a live tab"
	@echo "                            elsewhere is usual. make -C server seats-clear"
	@echo "  SOLO on the board         it never saw the relay. The monitor prints"
	@echo "                            why at 'match:' and 'relay:'."
	@echo "  waiting for player        one side never sent its fleet; check both"
	@echo "                            printed a 'join' line on the relay."
	@echo ""
	@echo "  This machine is $(shell ipconfig getifaddr en0 2>/dev/null || echo '<no LAN address>');"
	@echo "  SERVER_IP in include/config.h must match it for the board."
endif

# So `make help debug` does not fail on an unknown goal.
debug:
	@:

web: $(OUT) web/main.js

$(OUT): $(SRC) src/game/game.h src/game/screen.h src/game/input.h \
        src/game/grid.h src/game/sprite.h src/game/ship_sprite.inc \
        src/game/fleet.h src/game/press.h src/game/game_config.h \
        src/game/netplay.h
	$(EMCC) $(SRC) -Isrc/game -O2 -o $(OUT) \
	  -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=web \
	  -sEXPORTED_FUNCTIONS=$(EXPORTS) \
	  -sEXPORTED_RUNTIME_METHODS=cwrap,HEAPU8 \
	  -sALLOW_MEMORY_GROWTH=1

# The page's own script, built separately from the WASM core.
#
# It used to be compiled as the last line of the sim.js recipe, which meant a
# change to main.ts alone rebuilt nothing at all: sim.js was already up to
# date, so make skipped the whole recipe and the browser kept loading the old
# JavaScript. Its own target with its own prerequisite is what makes editing
# the page work.
web/main.js: web/main.ts web/tsconfig.json
	npx --yes -p typescript@5 tsc -p web/tsconfig.json

# Serve the page from the relay, not from a file server. The page calls
# /v1/join and friends on its own origin, so a plain static server answers
# those with 501 ("Unsupported method ('POST')" is python's http.server
# saying it only does GET and HEAD) and the game silently drops to offline.
# The relay serves the same files AND the API, and run-lan binds every
# interface so the ESP32 can reach it too.
# One command to play: build the page, then serve it and the relay together.
#
# The page calls /v1/join on its own origin, so a plain static server answers
# with 501 and the game silently drops to offline. The relay serves the same
# files AND the API, bound to every interface so the board can reach it too,
# and stops whatever already holds the port first.
play: web
	@if command -v go >/dev/null 2>&1; then \
	  echo "http://localhost:$(PORT)   page + relay, reachable from the board"; \
	  $(MAKE) -C server run-lan PORT=$(PORT); \
	else \
	  echo "WARNING: Go not found, so this is the page WITHOUT the relay."; \
	  echo "         Placement and play work; joining a match will not."; \
	  echo "http://localhost:8000   (Ctrl-C to stop)"; \
	  cd web && python3 -m http.server 8000; \
	fi

serve: play

# Compile the portable core with the host compiler -- catches errors without
# needing emscripten or the ESP toolchain.
check:
	c++ -std=c++17 -Wall -Wextra -Isrc/game -fsyntax-only src/game/*.cpp

# Flash the board, having first freed the seat its previous session is still
# holding.
#
# The relay lets a client take back a seat it already holds, so an ordinary
# reflash needs nothing. But renaming the player -- PLAYER_NAME in
# include/config.h -- breaks that match: the old seat is held under the old
# name, nothing reclaims it, and the board comes back to a table that is full
# of a version of itself that no longer exists.
#
# DROP is the flag. It names the seat to free first, and defaults to the name
# the firmware is about to identify as. `make upload DROP=esp32` frees a seat
# left by an earlier name; `make upload DROP=` skips it entirely.
# awk rather than sed: make balances parentheses inside $(shell ...) without
# understanding that sed escapes them, so a capture group ends the call early.
DROP ?= $(shell awk -F'"' '/define PLAYER_NAME/{print $$2}' include/config.h)
RELAY ?= localhost:$(PORT)
PORT  ?= 8080
# The control endpoints need the relay's admin token. It is printed at
# startup ("admin  token ...") and can also be set with -admin-token, which is
# what a script wants: pass ADMIN_TOKEN=... here, or the seat is not freed and
# the reason is a 401 rather than a missing relay.
ADMIN_TOKEN ?=

upload:
	@if [ -n "$(DROP)" ]; then \
	  code=$$(curl -s -o /dev/null -w '%{http_code}' -X POST \
	    -H "X-Admin-Token: $(ADMIN_TOKEN)" \
	    "http://$(RELAY)/v1/seats/drop?name=$(DROP)" 2>/dev/null); \
	  case "$$code" in \
	    200) echo "freed any seat held by \"$(DROP)\"" ;; \
	    401|403) echo "note: relay refused (admin token); run: make upload ADMIN_TOKEN=..." ;; \
	    000|"") echo "note: no relay on $(RELAY); nothing to free" ;; \
	    *) echo "note: relay answered $$code; seat not freed" ;; \
	  esac; \
	fi
	pio run -t upload -t monitor

# One command that verifies everything, because "did I break it" should not
# require remembering four. The host suites are the part that catches real
# bugs; the builds are there so a change that only breaks the firmware or the
# browser cannot pass.
test: check test-core
	@$(MAKE) --no-print-directory web >/dev/null && echo "web        ok"
	@pio run >/dev/null 2>&1 && echo "firmware   ok" || { echo "firmware   FAILED"; exit 1; }
	@$(MAKE) --no-print-directory -C server test >/dev/null && echo "relay      ok"

# The placement and damage rules are the one part of the game that can be
# wrong in ways looking at the screen would not reveal, so they get tests.
test-core:
	@c++ -std=c++17 -Wall -Wextra -Isrc/game -o /tmp/fleet_test \
	  tools/fleet_test.cpp src/game/fleet.cpp && /tmp/fleet_test
	@c++ -std=c++17 -Wall -Wextra -Isrc/game -o /tmp/intro_test \
	  tools/intro_test.cpp $(CORE) && /tmp/intro_test
	@c++ -std=c++17 -Wall -Wextra -Isrc/game -o /tmp/handshake_test \
	  tools/handshake_test.cpp $(CORE) && /tmp/handshake_test
	@c++ -std=c++17 -Wall -Wextra -Isrc/game -o /tmp/espnow_seat_test \
	  tools/espnow_seat_test.cpp src/game/fleet.cpp && /tmp/espnow_seat_test
	@c++ -std=c++17 -Wall -Wextra -Isrc/game -o /tmp/forcepage_test \
	  tools/forcepage_test.cpp $(CORE) && /tmp/forcepage_test
	@c++ -std=c++17 -Wall -Wextra -Isrc/game -o /tmp/solo_test \
	  tools/solo_test.cpp $(CORE) && /tmp/solo_test
	@c++ -std=c++17 -Wall -Wextra -Isrc/game -o /tmp/sinking_test \
	  tools/sinking_test.cpp $(CORE) && /tmp/sinking_test
	@c++ -std=c++17 -Wall -Wextra -Isrc/game -o /tmp/flow_test \
	  tools/flow_test.cpp $(CORE) && /tmp/flow_test
	@c++ -std=c++17 -Wall -Wextra -Isrc/game -o /tmp/netplay_test \
	  tools/netplay_test.cpp $(CORE) && /tmp/netplay_test
	@c++ -std=c++17 -Wall -Wextra -Isrc/game -o /tmp/rooms_test \
	  tools/rooms_test.cpp $(CORE) && /tmp/rooms_test
	@c++ -std=c++17 -Wall -Wextra -Isrc/game -o /tmp/reconnect_test \
	  tools/reconnect_test.cpp $(CORE) && /tmp/reconnect_test

clean:
	rm -f web/sim.js web/sim.wasm web/main.js /tmp/fleet_test /tmp/intro_test /tmp/handshake_test /tmp/espnow_seat_test /tmp/forcepage_test /tmp/solo_test /tmp/sinking_test /tmp/flow_test \
	  /tmp/netplay_test /tmp/rooms_test /tmp/reconnect_test
