# The dashboard in Grafana

Same three areas as `dashboard/`: the server output down the left half, who is
in each lobby top right, the controls under it.

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

Grafana 12.4.10, Infinity 4.0.0, Business Forms 6.3.5, all pinned. Infinity
4.x imports `react/jsx-runtime`, which only Grafana >=11.6.11 hands to a
plugin: on an older image the import 404s and the datasource fails to load
with a SystemJS error. Business Forms 6.3.5 needs >=12.3.0. Leaving the plugin
versions off installs the newest build, which is exactly how that breaks --
change the three together or not at all.

## What does the work

Two plugins, installed by the compose file:

| Plugin | For |
| --- | --- |
| `yesoreyeram-infinity-datasource` | reads the relay's JSON straight from `/v1/log`, `/v1/rooms`, `/v1/match` |
| `volkovlabs-form-panel` | turns a panel into a form that POSTs to `/v1/force-page` and `/v1/restart` |

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
