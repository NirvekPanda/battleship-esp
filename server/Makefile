# The local match relay. Run from this directory: `make -C server run`, or
# `cd server && make run`.
#
# The web root is ../web, so the relay serves the same files `make serve` does
# from the repo root -- the difference is that this one also routes packets,
# and serves the page from the same origin as the API so the browser needs no
# CORS handling. `make serve` at the repo root runs `run-lan` here, so that is
# the usual way in.
#
# Every run target stops whatever already holds the port (see kill-port), so
# restarting after a crash or a stray session never fails with "address
# already in use".
#
# Two seats, handed out by name. A client may take back a seat it already
# holds, which is what makes reflashing work; `seats-clear` frees both when
# gone-but-still-polling clients hold them, and POST /v1/seats/drop?name=X
# frees just one.
GO   ?= go
BIN  := relay
# Every interface by default. The boards are on the WiFi and so are the phones
# playing in a browser; a relay on loopback answers all of them with a
# connection reset. Set ADDR=localhost:8080 to keep it to this machine.
ADDR ?= 0.0.0.0:8080
PORT ?= 8080
WEB  ?= ../web
DASH ?= ../dashboard
# Where the relay serves the dashboard itself. Set DASH_ADDR= (empty) when
# nginx is serving it instead, or the two fight over the port.
DASH_ADDR ?= 0.0.0.0:8081

.DEFAULT_GOAL := help

.PHONY: help debug run run-lan run-quiet run-espnow kill-port seats-clear dashboard build test fmt vet clean

DEBUG_HELP := $(filter debug,$(MAKECMDGOALS))

# Most of the time this is driven from the repo root by `make play`, so the
# short help says so rather than listing everything it can do.
help:
ifeq ($(DEBUG_HELP),)
	@echo "Match relay (Go $(shell $(GO) env GOVERSION 2>/dev/null))"
	@echo ""
	@echo "  Usually you want 'make play' from the repo root, which runs this"
	@echo "  and serves the page with it."
	@echo ""
	@echo "  make run          this alone, on $(ADDR)"
	@echo "  make test         the packet and routing tests"
	@echo ""
	@echo "  make help debug   the rest"
else
	@echo "Running"
	@echo "  make run          every interface, on $(ADDR)"
	@echo "  make run-lan      the same thing, named for what it does"
	@echo "  make run-quiet    without a line per packet"
	@echo "  make run-espnow   gameplay reserved for ESP-NOW (refuses shots)"
	@echo "  make run PORT=9000 / ADDR=0.0.0.0:8080 / WEB=../web"
	@echo ""
	@echo "  Every run target stops whatever already holds the port first, so"
	@echo "  restarting never fails with 'address already in use'."
	@echo ""
	@echo "Seats (there are exactly two)"
	@echo "  make seats-clear  free both, when gone-but-polling clients hold them"
	@echo ""
	@echo "Dashboard"
	@echo "  make dashboard    serve it through nginx on :8081 (nginx.conf)"
	@echo "                    the relay already serves it there on its own"
	@echo "  make kill-port    stop whatever holds port $(PORT)"
	@echo ""
	@echo "  Seats are per name. A client reclaims a seat it already holds, so"
	@echo "  reflashing needs nothing -- renaming the board strands the old one:"
	@echo "  from the repo root, make upload DROP=<old name>."
	@echo ""
	@echo "Development"
	@echo "  make build        compile ./$(BIN)"
	@echo "  make fmt vet test"
	@echo "  make clean"
endif

debug:
	@:

# Bound to every interface, so anyone on the WiFi -- a board, a phone -- can
# reach it. `make run ADDR=localhost:8080` keeps it to this machine.
run: kill-port
	$(GO) run . -addr $(ADDR) -web $(WEB) -dashboard=$(DASH) -dashboard-addr=$(DASH_ADDR)

# What the board needs: bound to every interface, not just loopback. A relay
# on localhost answers a connection from the LAN with a reset, which the ESP32
# reports only as "errno 104, Connection reset by peer".
run-lan: kill-port
	$(GO) run . -addr 0.0.0.0:$(PORT) -web $(WEB) -dashboard=$(DASH) -dashboard-addr=$(DASH_ADDR)

run-quiet: kill-port
	$(GO) run . -addr $(ADDR) -web $(WEB) -dashboard=$(DASH) -dashboard-addr=$(DASH_ADDR) -quiet

# Reserved for the change that moves gameplay onto ESP-NOW. In this mode the
# relay serves the page and observes, and refuses to relay shots or results,
# so a half-migrated setup fails loudly instead of quietly running the match
# over both paths at once.
run-espnow: kill-port
	$(GO) run . -addr $(ADDR) -web $(WEB) -dashboard=$(DASH) -dashboard-addr=$(DASH_ADDR) -esp-now

# Stop whatever already holds the port before taking it.
#
# `go run` compiles to a temporary binary and execs it, so killing a previous
# session's make often leaves that child still listening -- and the next start
# dies with "address already in use" naming a process that is in no job list
# you can see. Ask the port who owns it instead of trying to remember.
#
# Not silent, deliberately: taking a port off something is worth one line, in
# case what it stopped was not what you expected.
kill-port:
	@pids=$$(lsof -ti tcp:$(PORT) -sTCP:LISTEN 2>/dev/null); \
	if [ -n "$$pids" ]; then \
	  echo "port $(PORT) held by pid(s) $$pids -- stopping"; \
	  kill $$pids 2>/dev/null || true; \
	  for i in 1 2 3 4 5; do \
	    lsof -ti tcp:$(PORT) -sTCP:LISTEN >/dev/null 2>&1 || break; \
	    sleep 0.3; \
	  done; \
	  pids=$$(lsof -ti tcp:$(PORT) -sTCP:LISTEN 2>/dev/null); \
	  if [ -n "$$pids" ]; then \
	    echo "still listening, forcing"; kill -9 $$pids 2>/dev/null || true; sleep 0.3; \
	  fi; \
	fi

# The dashboard through nginx, which is what nginx.conf describes. The relay
# already serves the same files on :8081 by itself, so this is only needed to
# reach the dashboard from another machine, or to put anything in front of it.
#
# -p makes dashboard/ the prefix, so every relative path in the config -- the
# document root and the temp directories -- lands inside the repo rather than
# in /usr/local.
dashboard:
	@command -v nginx >/dev/null 2>&1 || { \
	  echo "nginx is not installed: brew install nginx"; \
	  echo "(the relay already serves the dashboard on :8081 without it)"; \
	  exit 1; \
	}
	@mkdir -p $(DASH)/tmp
	@lsof -ti tcp:8081 -sTCP:LISTEN >/dev/null 2>&1 && { \
	  echo "Port 8081 is already taken -- most likely the relay serving the"; \
	  echo "dashboard itself. Start it with DASH_ADDR= so nginx can have the"; \
	  echo "port:  make run-lan DASH_ADDR="; \
	  exit 1; \
	} || true
	nginx -p "$(CURDIR)/$(DASH)" -c "$(CURDIR)/$(DASH)/nginx.conf"

# Free both seats when the table is held by clients that are gone but still
# polling -- a browser tab left open in another window is the usual one.
# Everyone still connected rejoins by themselves.
# ADMIN_TOKEN is the token the relay printed at startup, or the one passed to
# -admin-token: the control endpoints refuse anything else, and a 401 here
# reads as "nothing happened" if the flag is not passed on.
ADMIN_TOKEN ?=
seats-clear:
	@curl -fsS -X POST -H "X-Admin-Token: $(ADMIN_TOKEN)" \
	  http://$(ADDR)/v1/seats/clear && echo

build:
	$(GO) build -o $(BIN) .

test:
	$(GO) test ./...

fmt:
	$(GO) fmt ./...

vet:
	$(GO) vet ./...

clean:
	rm -f $(BIN)
