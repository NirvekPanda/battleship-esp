#!/usr/bin/env sh
# What is wrong with the front door.
#
#   sudo ./deploy/diagnose.sh              > /tmp/battleship-diag.txt 2>&1
#   sudo GAME_HOST=... ./deploy/diagnose.sh
#
# Read-only: it starts nothing, stops nothing and writes nothing. Safe to
# paste -- the admin token is redacted wherever it appears.
#
# A 502 from nginx means one thing: nginx could not reach what it proxies to.
# The sections below answer, in order, "is the relay running", "is it on the
# address nginx is proxying to", and "does nginx get through to it".

PORT="${PORT:-8090}"
DASH_PORT="${DASH_PORT:-8091}"
GRAFANA_PORT="${GRAFANA_PORT:-8092}"
SERVICE="${SERVICE:-battleship}"
GAME_HOST="${GAME_HOST:-battleship.nirvek.xyz}"
DASH_HOST="${DASH_HOST:-dashship.nirvek.xyz}"
SERVER_IP="${SERVER_IP:-192.168.86.104}"

h() { printf '\n========== %s ==========\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }
# Never print the token, here or in any file this quotes.
scrub() { sed -E 's/[0-9a-f]{32}/<REDACTED-TOKEN>/g; s/(token[=: ]+)[^ "]+/\1<REDACTED>/Ig'; }

h "who and where"
date; uname -a; id -un
echo "hostname: $(hostname)"
echo "addresses:"; (ip -4 addr show 2>/dev/null || ifconfig 2>/dev/null) | grep -E 'inet ' | sed 's/^/  /'

h "is the relay process running"
if have systemctl; then systemctl status "$SERVICE" --no-pager -l 2>&1 | head -20 | scrub
else echo "no systemctl"; fi
echo "--- matching processes ---"
ps -eo pid,args 2>/dev/null | grep -E "[r]elay|[c]loudflared|[n]ginx: master" | scrub

h "what is listening"
# The column that matters is the address: 127.0.0.1:8090 can only be reached
# from this machine, 0.0.0.0:8090 from anywhere on the LAN. nginx in another
# container hitting a loopback-only relay is a 502 every time.
ports_re="Local|COMMAND|Proto|:80[[:space:]]|:$PORT|:$DASH_PORT|:$GRAFANA_PORT"
out=""
have ss      && out="$(ss -lntp 2>/dev/null | grep -E "$ports_re")"
[ -n "$out" ] || { have netstat && out="$(netstat -lntp 2>/dev/null | grep -E "$ports_re")"; }
[ -n "$out" ] || { have lsof && out="$(lsof -nP -iTCP -sTCP:LISTEN 2>/dev/null | grep -E "$ports_re")"; }
[ -n "$out" ] && echo "$out" || echo "nothing found listening on :80, :$PORT, :$DASH_PORT or :$GRAFANA_PORT"

h "the relay, reached directly (bypassing nginx)"
for target in "127.0.0.1:$PORT" "127.0.0.1:$DASH_PORT" "$SERVER_IP:$PORT" "$SERVER_IP:$DASH_PORT"; do
  code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "http://$target/" 2>/dev/null)
  case "$code" in ""|000) code="NO ANSWER (refused, or nothing listening there)";; esac
  echo "  http://$target/           -> $code"
done
echo "  (200 here and 502 through nginx means nginx is proxying to the wrong address)"

h "nginx config"
if have nginx; then
  nginx -v 2>&1
  nginx -t 2>&1 | scrub
  echo "--- enabled sites ---"
  ls -l /etc/nginx/sites-enabled/ 2>/dev/null
  echo "--- what this site proxies to (the 502 answer is usually here) ---"
  grep -rnE 'proxy_pass|server_name|listen' /etc/nginx/sites-enabled/ 2>/dev/null | scrub
else
  echo "nginx is NOT installed on this machine"
fi

h "through nginx, by name (what a browser gets)"
# Host header set by hand, so this tests nginx itself without DNS or the
# tunnel being involved.
for hostname in "$GAME_HOST" "$DASH_HOST"; do
  code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 8 -H "Host: $hostname" http://127.0.0.1/ 2>/dev/null)
  case "$code" in ""|000) code="NO ANSWER (nginx is not listening on :80 here)";; esac
  echo "  Host: $hostname  ->  $code"
done
echo "  502 here = nginx is up and the relay behind it is not reachable."
echo "  404/default page = the site is not enabled, or server_name does not match."

h "nginx error log (last 40, newest last)"
tail -n 40 /var/log/nginx/error.log 2>/dev/null | scrub || echo "no /var/log/nginx/error.log"

h "cloudflare tunnel"
if have cloudflared; then
  cloudflared --version
  if have systemctl; then systemctl status cloudflared --no-pager -l 2>&1 | head -15 | scrub; fi
  echo "--- ingress rules (the service: line must point at nginx, normally http://localhost:80) ---"
  for f in /etc/cloudflared/config.yml /etc/cloudflared/config.yaml "$HOME/.cloudflared/config.yml"; do
    [ -f "$f" ] && { echo "--- $f ---"; scrub < "$f"; }
  done
  if have journalctl; then
    echo "--- recent tunnel errors ---"
    journalctl -u cloudflared -n 25 --no-pager 2>/dev/null | grep -iE "error|refused|unable|502" | scrub | tail -15
  fi
else
  echo "cloudflared is not installed here (the tunnel may be a Cloudflare-hosted one, or on another box)"
fi

h "firewall"
have ufw && ufw status verbose 2>/dev/null | head -15
have iptables && iptables -S 2>/dev/null | grep -vE '^-P|^-N' | head -15
echo "(nothing printed above means no local firewall rules to blame)"

h "docker / grafana"
if have docker; then docker ps --format '  {{.Names}}  {{.Status}}  {{.Ports}}' 2>/dev/null; else echo "no docker"; fi

h "done"
echo "Send this whole file. The token is redacted; nothing else here is secret."
