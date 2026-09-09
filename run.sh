#!/usr/bin/env bash
#
# Run the relay on a server -- a Proxmox container is what this was written
# for -- and, on request, make it start itself at boot.
#
#   ./run.sh                 pull, build, serve (this is what systemd runs)
#   ./run.sh --deps          install what is missing AND update go and emsdk
#   ./run.sh --install       install and enable the systemd service, then start it
#   ./run.sh --nginx         install the nginx site for the two hostnames
#   ./run.sh --uninstall     stop and remove the service
#
# The build installs anything it is missing -- go, node, emscripten -- and
# leaves versions alone otherwise; only --deps moves them forward, so a reboot
# never changes the toolchain under a working build.
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
GRAFANA_PORT="${GRAFANA_PORT:-8092}"
# Where the admin token lives between runs. The relay makes a fresh one every
# start unless it is given one, which would mean re-pasting it into Grafana
# after every reboot; generated once and kept here, it stays put.
TOKEN_FILE="${TOKEN_FILE:-$REPO/.admin-token}"
SERVICE="${SERVICE:-battleship}"
# The account this runs as on the private Proxmox host, and its password, so
# the script can install packages and write a unit file without a prompt.
#
# This is a credential in a file: anyone who can read this repo can read it.
# That is the deliberate trade for an unattended installer on a private box --
# override it with SUDO_USER_NAME / SUDO_PASS in the environment, or give the
# account passwordless sudo and delete these two lines.
SUDO_USER_NAME="${SUDO_USER_NAME:-battleship}"
SUDO_PASS="${SUDO_PASS:-battleship}"
# Where nginx should look for the relay, and the address the server answers on
# for a browser typing it directly. Same box by default; set RELAY_HOST when
# nginx runs somewhere the relay does not.
RELAY_HOST="${RELAY_HOST:-127.0.0.1}"
SERVER_IP="${SERVER_IP:-192.168.86.104}"
GAME_HOST="${GAME_HOST:-battleship.nirvek.xyz}"
DASH_HOST="${DASH_HOST:-dashship.nirvek.xyz}"

# Become root and carry on, using the password above -- so --install and
# --nginx can be run as the battleship user with no sudo typed in front. The
# settings are passed explicitly because sudo resets the environment.
reexec_as_root() {
  [ "$(id -u)" -eq 0 ] && return 0
  command -v sudo >/dev/null || die "not root and no sudo; log in as root and re-run"
  # The first argument says what for; the rest are the script's own options and
  # are the only thing that may be handed back to it. Forwarding the reason as
  # well is how root's copy came to be run with "install the service" as $1.
  local why="$1"; shift
  say "becoming root to $why"
  local env_args
  env_args="PORT=$PORT DASH_PORT=$DASH_PORT GRAFANA_PORT=$GRAFANA_PORT \
SERVICE=$SERVICE GAME_HOST=$GAME_HOST DASH_HOST=$DASH_HOST \
RELAY_HOST=$RELAY_HOST SERVER_IP=$SERVER_IP TOKEN_FILE=$TOKEN_FILE \
SUDO_USER_NAME=$SUDO_USER_NAME"
  if sudo -n true 2>/dev/null; then
    # shellcheck disable=SC2086
    exec sudo env $env_args "$0" "$@"
  fi
  printf '%s\n' "$SUDO_PASS" | sudo -S -p '' true 2>/dev/null \
    || die "sudo refused the stored password for $(id -un); set SUDO_PASS, or run as root"
  # shellcheck disable=SC2086
  exec sudo -S -p '' env $env_args "$0" "$@" < <(printf '%s\n' "$SUDO_PASS")
}

# The token, in one place: whatever was passed in, else the saved one, else a
# new one saved for next time. Readable only by the account that runs this.
ensure_token() {
  [ -n "$ADMIN_TOKEN" ] && return 0
  if [ -s "$TOKEN_FILE" ]; then
    ADMIN_TOKEN="$(cat "$TOKEN_FILE")"
  else
    ADMIN_TOKEN="$(head -c 16 /dev/urandom | od -An -tx1 | tr -d ' \n')"
    ( umask 077; printf '%s\n' "$ADMIN_TOKEN" > "$TOKEN_FILE" )
    say "new admin token saved to $TOKEN_FILE"
  fi
  chmod 600 "$TOKEN_FILE" 2>/dev/null || true
  # Grafana reads this file on `docker compose up`, so the token never has to
  # be copied by hand -- and it is rewritten every start, so a token changed
  # here reaches Grafana on its next restart.
  ( umask 077; cat > "$REPO/deploy/grafana/.env" <<EOF
# Written by run.sh -- do not edit, and do not commit. Regenerated every start.
RELAY=http://host.docker.internal:$PORT
ADMIN_TOKEN=$ADMIN_TOKEN
GRAFANA_PORT=$GRAFANA_PORT
EOF
  )
}

say()  { printf '\033[36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[33m==>\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[31m==>\033[0m %s\n' "$*" >&2; exit 1; }

# ---- dependencies ---------------------------------------------------------
#
# What a build needs, and how each one is got:
#
#   go        the relay is Go. Debian's package lags the language, and
#             server/go.mod asks for a version newer than bookworm ships, so
#             it comes from the official tarball into /usr/local/go.
#   node/npm  `make web` type-checks the page with `npx tsc`.
#   emcc      the page is WASM. emscripten is installed as emsdk under $HOME,
#             which is how upstream distributes it.
#   nginx     only for --nginx.
#
# ENSURE installs what is missing and leaves what is there alone; UPDATE (via
# --deps) also moves go and emsdk to the current release. The split is
# deliberate: this script runs at boot, and a machine that quietly changed
# toolchain on every reboot would be a machine whose builds are not
# reproducible.
MIN_GO="${MIN_GO:-1.24.2}"
EMSDK_DIR="${EMSDK_DIR:-$HOME/emsdk}"

# Runs a command as root, or explains why it cannot.
# Root, by whichever route is open: already root, passwordless sudo, or the
# password above fed to `sudo -S` on stdin.
as_root() {
  if [ "$(id -u)" -eq 0 ]; then
    "$@"
  elif ! command -v sudo >/dev/null; then
    die "need root for: $*  (no sudo on this machine; run as root)"
  elif sudo -n true 2>/dev/null; then
    sudo "$@"
  elif printf '%s\n' "$SUDO_PASS" | sudo -S -p '' true 2>/dev/null; then
    printf '%s\n' "$SUDO_PASS" | sudo -S -p '' "$@"
  else
    die "need root for: $*  (sudo refused; set SUDO_PASS, or run as root)"
  fi
}

apt_get() {
  command -v apt-get >/dev/null || return 1
  as_root env DEBIAN_FRONTEND=noninteractive apt-get "$@"
}

# "1.24.2" >= "1.24.0"? sort -V is in coreutils and does the whole job.
version_at_least() {
  [ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -1)" = "$2" ]
}

# Empty when go is not installed -- and it has to SUCCEED while saying so.
# `set -o pipefail` makes a pipeline that starts with a missing command return
# 127, which under `set -e` killed the script mid-assignment with no message
# at all: "git pull" and then silence. Ask before piping.
go_version() {
  command -v go >/dev/null 2>&1 || return 0
  go version 2>/dev/null | awk '{print $3}' | sed 's/^go//'
}

install_go() {
  local want="$1"
  local arch
  case "$(uname -m)" in
    x86_64|amd64) arch=amd64 ;;
    aarch64|arm64) arch=arm64 ;;
    armv7l) arch=armv6l ;;
    *) die "no Go build for $(uname -m); install it by hand" ;;
  esac
  local os tar
  os="$(uname -s | tr '[:upper:]' '[:lower:]')"
  tar="go${want}.${os}-${arch}.tar.gz"
  say "installing go $want"
  curl -fsSL "https://go.dev/dl/${tar}" -o "/tmp/$tar" || die "could not download $tar"
  # Replaced, not merged: an old tree left in place is how a "new" toolchain
  # ends up running yesterday's compiler.
  as_root rm -rf /usr/local/go
  as_root tar -C /usr/local -xzf "/tmp/$tar"
  rm -f "/tmp/$tar"
  export PATH="/usr/local/go/bin:$PATH"
}

# The newest released go, or MIN_GO on a box that cannot reach go.dev. The
# sed at the end of a pipeline succeeds whatever curl did, so the fallback has
# to test the string rather than the exit status.
latest_go() {
  local v
  v="$(curl -fsSL https://go.dev/VERSION?m=text 2>/dev/null | head -1 | sed 's/^go//' || true)"
  [ -n "$v" ] || v="$MIN_GO"
  printf '%s\n' "$v"
}

ensure_go() {
  local update="${1:-}"
  export PATH="/usr/local/go/bin:$PATH"
  local have; have="$(go_version)"
  # Only the toolchain THIS script installed is the one it replaces. A go from
  # apt, brew or a version manager belongs to whatever put it there, and
  # ripping it out from under that is not an upgrade, it is a surprise.
  local ours=""; [ "$(command -v go 2>/dev/null)" = "/usr/local/go/bin/go" ] && ours=yes

  if [ -z "$have" ]; then
    install_go "$(latest_go)"
  elif ! version_at_least "$have" "$MIN_GO"; then
    say "go $have is older than the $MIN_GO server/go.mod asks for"
    if [ -n "$ours" ]; then
      install_go "$(latest_go)"
    else
      die "go $have is too old and was not installed by this script ($(command -v go)); upgrade it there, or remove it and re-run"
    fi
  elif [ -n "$update" ] && [ -n "$ours" ]; then
    local latest; latest="$(latest_go)"
    if [ -n "$latest" ] && [ "$latest" != "$have" ]; then install_go "$latest"
    else say "go $have is current"; fi
  elif [ -n "$update" ]; then
    say "go $have (from $(command -v go); left alone -- not ours to update)"
  else
    say "go $have"
  fi
}

ensure_node() {
  if command -v node >/dev/null && command -v npx >/dev/null; then
    say "node $(node -v)"
    return
  fi
  say "installing node and npm"
  apt_get update -qq && apt_get install -y --no-install-recommends nodejs npm \
    || die "install node (>= 18) and npm, which 'make web' needs for tsc"
}

# emscripten, as upstream ships it. Sourced rather than installed system-wide:
# emsdk puts its tools on PATH from a script it generates, and copying those
# paths somewhere else is how they go stale.
use_emsdk() {
  [ -f "$EMSDK_DIR/emsdk_env.sh" ] || return 1
  # shellcheck disable=SC1091
  . "$EMSDK_DIR/emsdk_env.sh" >/dev/null 2>&1 || return 1
  command -v emcc >/dev/null
}

ensure_emcc() {
  local update="${1:-}"
  if command -v emcc >/dev/null || use_emsdk; then
    if [ -n "$update" ] && [ -d "$EMSDK_DIR/.git" ]; then
      say "updating emscripten"
      ( cd "$EMSDK_DIR" && git pull --ff-only && ./emsdk install latest && ./emsdk activate latest ) \
        || warn "emsdk update failed -- keeping the version already installed"
      use_emsdk || true
    fi
    say "emcc $(emcc -v 2>&1 | head -1 | awk '{print $NF}' || true)"
    return
  fi

  # None of this is fatal. build() falls back to a page that is already built
  # and only gives up when there is none, so a compiler that cannot be
  # installed must not take the relay down with it: as a service, dying here
  # means systemd restarts it, it dies again, and the game never comes up
  # although everything it actually needs to serve is sitting in web/.
  if ! command -v git >/dev/null; then
    warn "git is needed to install emscripten -- skipping"
    return 0
  fi
  say "installing emscripten into $EMSDK_DIR (this takes a few minutes)"
  if [ ! -d "$EMSDK_DIR" ] &&
     ! git clone --depth 1 https://github.com/emscripten-core/emsdk.git "$EMSDK_DIR"; then
    warn "could not create $EMSDK_DIR -- is it writable by $(id -un)?"
    return 0
  fi
  if ! ( cd "$EMSDK_DIR" && ./emsdk install latest && ./emsdk activate latest ); then
    warn "emsdk install failed -- serving whatever is already built in web/"
    return 0
  fi
  use_emsdk || warn "emsdk installed but emcc is still not on PATH"
}

# Everything the build needs, in one call. UPDATE is passed through so only
# --deps moves versions.
deps() {
  local update="${1:-}"
  if ! command -v curl >/dev/null; then
    say "installing curl"
    apt_get update -qq || true
    apt_get install -y --no-install-recommends curl ca-certificates \
      || die "curl is needed to fetch the go toolchain"
  fi
  ensure_go "$update"
  ensure_node
  ensure_emcc "$update"
}

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
  deps

  if command -v emcc >/dev/null; then
    say "building the page (emcc)"
    make web
  elif [ -f web/sim.js ] && [ -f web/sim.wasm ] && [ -f web/main.js ]; then
    warn "no emcc -- serving the build already in web/"
  else
    die "no emcc and no build in web/: install emscripten, or build elsewhere and copy web/sim.js, web/sim.wasm and web/main.js here"
  fi

  say "building the relay"
  # CGO_ENABLED=0: the relay imports nothing outside the standard library and
  # no "C", so cgo buys it nothing -- but Go turns cgo on by default, and a
  # container image without linux-libc-dev then fails the build on a missing
  # <linux/errno.h> that has no bearing on this program. Off, it builds
  # anywhere and links statically, which is what a container wants.
  ( cd server && CGO_ENABLED=0 go build -o relay . )
}

# ---- serve ----------------------------------------------------------------
serve() {
  ensure_token
  say "game      http://0.0.0.0:$PORT"
  say "dashboard http://0.0.0.0:$DASH_PORT"
  # exec, so systemd supervises the relay itself rather than this script.
  exec server/relay \
    -addr ":$PORT" \
    -dashboard-addr ":$DASH_PORT" \
    -web web \
    -dashboard dashboard \
    -admin-token "$ADMIN_TOKEN"
}

# ---- boot -----------------------------------------------------------------
# The unit runs THIS script, so a reboot pulls and rebuilds before serving --
# which is what "make sure it is up to date before launching" means when the
# launching is done by systemd at three in the morning.
install_service() {
  reexec_as_root "install the service" --install
  # The account the relay runs as. SUDO_USER is who invoked sudo, which is
  # the right guess when that is a person and the wrong one when it is a
  # script; the configured name wins.
  local user="${SUDO_USER_NAME:-${SUDO_USER:-root}}"
  id -u "$user" >/dev/null 2>&1 || die "no such user: $user (set SUDO_USER_NAME)"
  local unit="/etc/systemd/system/$SERVICE.service"

  # --install runs as root, so $HOME here is /root and EMSDK_DIR with it. The
  # service runs as $user, who cannot write /root -- it would try to install
  # emscripten there on every start, fail, and be restarted for ever. The
  # unit gets the home of the account that will actually run it.
  # `|| true` because of pipefail: getent is absent on some minimal images,
  # and a pipeline that starts with a missing command returns 127, which under
  # set -e would kill the install here rather than fall back on the next line.
  local home
  home="$(getent passwd "$user" 2>/dev/null | cut -d: -f6 || true)"
  [ -n "$home" ] || home="/home/$user"
  local unit_emsdk="$home/emsdk"
  say "the service runs as $user (home $home)"

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
# systemd hands a unit a minimal PATH and go is never on it. emscripten adds
# itself from $EMSDK_DIR/emsdk_env.sh, which the script sources.
Environment=PATH=/usr/local/go/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
# systemd sets no HOME, and emsdk, npm and git all write under it.
Environment=HOME=$home
Environment=EMSDK_DIR=$unit_emsdk
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
  reexec_as_root "remove the service" --uninstall
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
  reexec_as_root "install the nginx site" --nginx
  if ! command -v nginx >/dev/null; then
    say "installing nginx"
    apt_get update -qq && apt_get install -y --no-install-recommends nginx \
      || die "nginx is not installed and apt could not install it"
  fi
  local site="/etc/nginx/sites-available/$SERVICE"

  say "writing $site"
  sed -e "s|@GAME_HOST@|$GAME_HOST|g" \
      -e "s|@DASH_HOST@|$DASH_HOST|g" \
      -e "s|@PORT@|$PORT|g" \
      -e "s|@DASH_PORT@|$DASH_PORT|g" \
      -e "s|@UPSTREAM@|$RELAY_HOST|g" \
      -e "s|@SERVER_IP@|$SERVER_IP|g" \
      deploy/battleship.nginx.conf > "$site"

  mkdir -p /etc/nginx/sites-enabled
  ln -sf "$site" "/etc/nginx/sites-enabled/$SERVICE"
  nginx -t
  systemctl reload nginx
  say "$GAME_HOST -> $RELAY_HOST:$PORT,  $DASH_HOST -> $RELAY_HOST:$DASH_PORT"

  # A 502 means nginx could not reach the relay, which is worth finding out
  # here rather than from a browser. nginx is fine; the thing behind it is not.
  # -s not -sS: curl's own "Failed to connect" is noise next to the advice
  # below, and printing it twice for two ports buries the one useful line.
  local u down=0
  for u in "$RELAY_HOST:$PORT" "$RELAY_HOST:$DASH_PORT"; do
    if curl -fs -o /dev/null --max-time 5 "http://$u/" 2>/dev/null; then
      say "upstream $u answers"
    else
      warn "upstream $u does NOT answer -- nginx will return 502 for it"
      down=1
    fi
  done
  if [ "$down" -eq 1 ]; then
    warn ""
    warn "nginx is fine; the relay behind it is not running. Start it:"
    warn "    ./run.sh --install     (and it comes back after a reboot)"
    warn "If the relay lives on another machine, point nginx at it instead:"
    warn "    RELAY_HOST=$SERVER_IP ./run.sh --nginx"
  fi
}

case "${1:-serve}" in
  serve|"")    pull; build; serve ;;
  --install)   pull; deps update; build; install_service ;;
  --uninstall) uninstall_service ;;
  --nginx)     install_nginx ;;
  --deps)      deps update ;;
  --no-pull)   build; serve ;;
  -h|--help)   sed -n '2,21p' "$0" ;;
  *)           die "unknown option: $1  (try --help)" ;;
esac
