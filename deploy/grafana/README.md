# The dashboard in Grafana

The same shape as `dashboard/`: the server output down the left half, who is
in each lobby top right, and under it -- where the dashboard has its controls
-- a link back to them, because they cannot work here. See below.

    https://dashship.nirvek.xyz/grafana

That is the link. It lands straight on this dashboard rather than on a Grafana
home page, and `/grafana/d/battleship/battleship-relay` is the same page by
its own name.

## The log reads like the dashboard's

Same seven columns, same headings, same order and roughly the same widths:
when, lobby, page, from, type, cell, detail. `to` and `seq` are off there, so
they are absent here.

They are declared in the query rather than left to the parser. The parser
takes its fields from the rows it is given, and a server remark carries no
type and no cell -- so a quiet relay produced a table missing two of its
columns, which then appeared later and shifted everything sideways.

## The controls are NOT here

The panel in the bottom right links to the dashboard instead of driving the
relay, and that is deliberate.

Every control endpoint is a POST that needs the admin token in a header.
Grafana keeps that token inside the datasource, where only its backend can
read it -- which is exactly what keeps an operator credential out of the
browser. A form panel's buttons run in the browser. Wired up as they were,
the request went to the browser's own origin, with no token and with
`${lobby}` never substituted: it 404'd, and the relay logged nothing. The
buttons looked like controls and were not.

Making them real would mean putting the token somewhere the page can read it,
which is the one thing the datasource arrangement exists to prevent. So the
controls stay on the dashboard, one link away on the same hostname.

That also means one plugin here instead of two.

    ./run.sh                        # the relay, and the token Grafana needs
    cd deploy/grafana && docker compose up -d
    # http://localhost:8092  ->  "Battleship relay"

No arguments and nothing to paste. `run.sh` keeps the admin token in
`.admin-token` and writes it, the relay's address and the port into
`deploy/grafana/.env` every time it starts; compose reads that file. The token
is generated once and reused, because the relay invents a new one on every
start unless it is given one -- which would have meant re-pasting it into
Grafana after every reboot. Both files are gitignored.

## Versions move together

Grafana 12.4.10 and Infinity 4.0.0, both pinned. Infinity 4.x imports
`react/jsx-runtime`, which only Grafana >=11.6.11 hands to a plugin: on an
older image the import 404s and the datasource fails to load with a SystemJS
error. Leaving the plugin version off installs the newest build, which is
exactly how that breaks -- change the two together or not at all.

## What does the work

One plugin, installed by the compose file:

| Plugin | For |
| --- | --- |
| `yesoreyeram-infinity-datasource` | reads the relay's JSON straight from `/v1/log`, `/v1/rooms`, `/v1/match` |

No exporter and no database. The relay already answers with arrays of objects,
which is exactly what a Grafana table wants, so the JSON goes in as it is.
The admin token is a datasource header, so it is sent by Grafana's backend and
never reaches a browser -- which also means the log arrives unredacted.

## What it does not do as well as `dashboard/`

Worth knowing before switching:

- **Refresh.** The hand-written dashboard long-polls and paints in about a
  second; Grafana re-runs a query on a timer, and 1s is its floor
  (`GF_DASHBOARDS_MIN_REFRESH_INTERVAL` above). A shot appears within a
  second or two rather than as it happens.
- **The log is a table, not a feed.** It re-queries the whole buffer and
  re-sorts, rather than appending what is new: no follow-the-tail scrolling,
  no per-column collapse, no drag-resize of the columns.
- **Controls are a form, not buttons.** One submit per action, with a lobby
  and a page chosen above it, instead of a grid of buttons that grey
  themselves out. Grafana has no way to know that "gameplay" is refused until
  both fleets are down -- the relay still refuses it with a 409, but the
  refusal arrives as an error toast rather than as a disabled button.
- **Two plugins.** Both are widely used and Apache-licensed, but they are
  still two more things to install and keep current than a page that needs
  nothing.

What is gained: history, alerting, and a place to put the relay beside
whatever else the machine is running.

## Behind the name

Grafana is told it lives under a path, not at a host root:
`GF_SERVER_ROOT_URL` ends in `/grafana/` and `GF_SERVER_SERVE_FROM_SUB_PATH`
is on. Both are needed -- Grafana builds every asset URL and every redirect
from `root_url`, so without them the page loads and then asks for
`/public/build/...` at the top of the host, gets the dashboard's `index.html`
back, and renders nothing.

Because Grafana expects to see the prefix, nothing in front of it may strip
the prefix. nginx passes `/grafana/` straight through (`deploy/battleship.nginx.conf`),
and a Cloudflare ingress rule must do the same.

**Cloudflare.** The tunnel's rules for these hostnames are managed in the
Cloudflare dashboard, not in `/etc/cloudflared/config.yml` -- the
`originService` in cloudflared's log is the truth. Either:

- point `dashship.nirvek.xyz` at nginx (`http://192.168.86.104:80`) and let
  nginx route `/` to the dashboard and `/grafana/` to Grafana; or
- add a second rule for `dashship.nirvek.xyz` with path `/grafana*` going to
  `http://192.168.86.104:8092`, above the catch-all for that hostname.

The first is fewer moving parts and is what the nginx site is written for.
