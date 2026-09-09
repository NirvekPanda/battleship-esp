#!/usr/bin/env bash
#
# Run the relay on a server -- a Proxmox container is what this was written
# for -- and, on request, make it start itself at boot.
#
#   ./run.sh                 pull, build, serve (this is what systemd runs)
#   ./run.sh --install       install and enable the systemd service, then start it
#   ./run.sh --nginx         install the nginx site for the two hostnames
#   ./run.sh --uninstall     stop and remove the service
#
#   PORT=8090 DASH_PORT=8091 ADMIN_TOKEN=... ./run.sh
#
# The game page is served on PORT and the dashboard on DASH_PORT, by the relay
# itself: it serves both directories, so nothing else has to be running for a
# browser on the LAN to play. nginx is only needed to put the two public
# hostnames in front of them, which --nginx sets up.
set -euo pipefail

cd "$(dirname "$0")"
REPO="$(pwd)"

PORT="${PORT:-8090}"
DASH_PORT="${DASH_PORT:-8091}"
ADMIN_TOKEN="${ADMIN_TOKEN:-}"
SERVICE="${SERVICE:-battleship}"
GAME_HOST="${GAME_HOST:-battleship.nirvek.xyz}"
DASH_HOST="${DASH_HOST:-dashship.nirvek.xyz}"

say()  { printf '\033[36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[33m==>\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[31m==>\033[0m %s\n' "$*" >&2; exit 1; }

# ---- update ---------------------------------------------------------------
# Never fatal. This runs at boot, where the network may not be up yet and a
# machine that cannot reach GitHub should still start the game it already has.
pull() {
  if [ ! -d .git ]; then
    warn "not a git checkout; skipping the pull"
    return
  fi
  say "git pull"
  git pull --ff-only || warn "pull failed -- running what is already here"
}

# ---- build ----------------------------------------------------------------
# The page is WASM, so it needs emscripten. A server without it can still run
# a build that was made elsewhere, which is why a missing emcc is only fatal
# when there is nothing built to fall back on.
build() {
  command -v go >/dev/null || die "go is not installed"

  if command -v emcc >/dev/null; then
    say "building the page (emcc)"
    make web
  elif [ -f web/sim.js ] && [ -f web/sim.wasm ] && [ -f web/main.js ]; then
    warn "no emcc -- serving the build already in web/"
  else
    die "no emcc and no build in web/: install emscripten, or build elsewhere and copy web/sim.js, web/sim.wasm and web/main.js here"
  fi

  say "building the relay"
  ( cd server && go build -o relay . )
}

# ---- serve ----------------------------------------------------------------
serve() {
  say "game      http://0.0.0.0:$PORT"
  say "dashboard http://0.0.0.0:$DASH_PORT"
  # exec, so systemd supervises the relay itself rather than this script.
  exec server/relay \
    -addr ":$PORT" \
    -dashboard-addr ":$DASH_PORT" \
    -web web \
    -dashboard dashboard \
    ${ADMIN_TOKEN:+-admin-token "$ADMIN_TOKEN"}
}

# ---- boot -----------------------------------------------------------------
# The unit runs THIS script, so a reboot pulls and rebuilds before serving --
# which is what "make sure it is up to date before launching" means when the
# launching is done by systemd at three in the morning.
install_service() {
  [ "$(id -u)" -eq 0 ] || die "run --install as root (sudo ./run.sh --install)"
  local user="${SUDO_USER:-root}"
  local unit="/etc/systemd/system/$SERVICE.service"

  say "writing $unit"
  cat > "$unit" <<UNIT
[Unit]
Description=battleship relay (game on :$PORT, dashboard on :$DASH_PORT)
Documentation=file://$REPO/server/README.md
# The pull wants a network, and only a route out counts -- an interface being
# up says nothing about whether anything is reachable through it.
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=$user
WorkingDirectory=$REPO
Environment=PORT=$PORT
Environment=DASH_PORT=$DASH_PORT
${ADMIN_TOKEN:+Environment=ADMIN_TOKEN=$ADMIN_TOKEN}
# systemd hands a unit a minimal PATH, and go and emcc are rarely on it.
Environment=PATH=/usr/local/go/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:$REPO/../emsdk/upstream/emscripten
ExecStart=$REPO/run.sh
Restart=on-failure
RestartSec=5
# A relay that cannot pull or build should still be restarted rather than
# giving up for good: the usual cause is a network that has not arrived yet.
StartLimitIntervalSec=0

[Install]
WantedBy=multi-user.target
UNIT

  systemctl daemon-reload
  systemctl enable "$SERVICE"
  systemctl restart "$SERVICE"
  say "enabled at boot; follow it with:  journalctl -fu $SERVICE"
}

uninstall_service() {
  [ "$(id -u)" -eq 0 ] || die "run --uninstall as root"
  systemctl disable --now "$SERVICE" 2>/dev/null || true
  rm -f "/etc/systemd/system/$SERVICE.service"
  systemctl daemon-reload
  say "removed"
}

# ---- the two hostnames ----------------------------------------------------
# Only needed when the relay is reached by name from outside: nginx terminates
# the tunnel and proxies to the two ports. Same-origin inside, so the CORS
# block only matters for a page served from one name calling the other.
install_nginx() {
  [ "$(id -u)" -eq 0 ] || die "run --nginx as root"
  command -v nginx >/dev/null || die "nginx is not installed"
  local site="/etc/nginx/sites-available/$SERVICE"

  say "writing $site"
  sed -e "s|@GAME_HOST@|$GAME_HOST|g" \
      -e "s|@DASH_HOST@|$DASH_HOST|g" \
      -e "s|@PORT@|$PORT|g" \
      -e "s|@DASH_PORT@|$DASH_PORT|g" \
      deploy/battleship.nginx.conf > "$site"

  mkdir -p /etc/nginx/sites-enabled
  ln -sf "$site" "/etc/nginx/sites-enabled/$SERVICE"
  nginx -t
  systemctl reload nginx
  say "$GAME_HOST -> :$PORT,  $DASH_HOST -> :$DASH_PORT"
}

case "${1:-serve}" in
  serve|"")    pull; build; serve ;;
  --install)   pull; build; install_service ;;
  --uninstall) uninstall_service ;;
  --nginx)     install_nginx ;;
  --no-pull)   build; serve ;;
  -h|--help)   sed -n '2,20p' "$0" ;;
  *)           die "unknown option: $1  (try --help)" ;;
esac
