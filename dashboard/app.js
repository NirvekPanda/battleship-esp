// The dashboard talks to the relay on its own origin: served either by the
// relay itself on :8081, or by nginx with /v1 proxied through (see nginx.conf).
// Either way these paths are relative, so neither needs CORS.

// Every element app.js expects the page to provide. Checked once, up front,
// because the alternative is a null reference part way through rendering: the
// panel simply stays empty and the page gives no hint that anything is wrong.
// The usual cause is a cached index.html paired with a fresh app.js.
const NEEDED = [
  "logTable", "cols", "logHead",
  "log", "logRows", "seats", "match", "matchNote", "pages", "pageNote",
  "rooms", "roomsNote", "roomTitle", "actions",
  "splitH", "splitV", "splitP", "stack",
  "restart", "hardRestart", "restartNote", "csv", "token", "where",
  "controlsLobby",
  "forget", "forgetNote",
  "dot", "connText",
];

const $ = (id) => document.getElementById(id);

{
  const missing = NEEDED.filter((id) => !document.getElementById(id));
  if (missing.length > 0) {
    document.body.innerHTML =
      `<div style="padding:24px;font:14px ui-monospace,monospace;color:#e2b8c6">` +
      `<b>Dashboard is out of date.</b><br><br>` +
      `This page is missing: ${missing.join(", ")}.<br>` +
      `It is almost certainly a cached copy of index.html running against a ` +
      `newer app.js.<br><br>Reload with a hard refresh ` +
      `(<b>Cmd-Shift-R</b> / Ctrl-Shift-R).</div>`;
    throw new Error(`dashboard: page is missing ${missing.join(", ")}`);
  }
}

// Every line still in the table, so a column being switched on can be filled
// in for rows that are already there.
const KEEP = 500;
const kept = [];

function redrawRows() {
  const rows = $("logRows");
  rows.innerHTML = "";
  for (const line of kept) rows.appendChild(buildRow(line));
  $("log").scrollTop = $("log").scrollHeight;
}

// One row, with a cell per visible column. A line from the relay itself has
// no packet fields, so it gets a single cell spanning everything after the
// timestamp rather than a row of blanks.
function buildRow(line) {
  const tr = document.createElement("tr");
  const isPacket = Boolean(line.kind);

  for (let i = 0; i < COLUMNS.length; i++) {
    const c = COLUMNS[i];

    // A collapsed column keeps its cell so the columns after it stay lined up
    // with their headers; it simply has nothing in it.
    if (hidden.has(c.key)) {
      addCell(tr, c.key, "");
      continue;
    }
    if (c.key === "when") {
      addCell(tr, "when", stamp(new Date(line.at)));
      continue;
    }
    if (!isPacket) {
      // A remark from the relay has no packet fields, so it runs across the
      // rest of the row rather than sitting in one column with blanks beside
      // it.
      const td = addCell(tr, "plain", line.text);
      td.colSpan = COLUMNS.length - i;
      break;
    }
    const v = line[c.key];
    addCell(tr, c.key, v === undefined || v === null ? "" : String(v));
  }
  return tr;
}

// textContent, never innerHTML: a player picks their own name, and it ends up
// in this table.
function addCell(tr, cls, text) {
  const td = document.createElement("td");
  td.className = cls;
  td.textContent = text;
  tr.appendChild(td);
  return td;
}

const pad = (n, w = 2) => String(n).padStart(w, "0");
function stamp(d) {
  return (
    `${pad(d.getMonth() + 1)}-${pad(d.getDate())} ` +
    `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}` +
    `.${pad(d.getMilliseconds(), 3)}`
  );
}

// Mirrors game::Page in src/game/game.h and pageNames in server/packet.go.
// Only the four worth jumping to are offered; the rest are transient.
const PAGES = [
  { n: 0, label: "START", why: "the title screen" },
  { n: 1, label: "CONNECTING", why: "the handshake" },
  { n: 2, label: "PLACE SHIPS", why: "lay out the fleet" },
  { n: 4, label: "GAMEPLAY", why: "aim and fire", needsFleets: true },
];

// The control endpoints need the token the relay prints at startup. Kept in
// localStorage so it survives a reload; it is an operator credential for a
// local service, not a user secret.
let adminToken = localStorage.getItem("adminToken") ?? "";

let logCursor = 0;
let bothReady = false;

function setConnected(up, text) {
  $("dot").className = "dot " + (up ? "up" : "down");
  $("connText").textContent = text;
}

// ---- columns -------------------------------------------------------------
//
// One entry per field the relay reports. Every packet type has to land in
// these without anything being crammed together: a shot has a cell, a result
// has none, a page packet comes from the server and goes to everyone. Squeeze
// them into fewer columns and the parts that differ per type end up as prose
// in one cell, which cannot be aligned, scanned or hidden.
const COLUMNS = [
  { key: "when",   label: "when",   min: 60,  def: 118 },
  // Which lobby: "main" for the one everyone arrives in, "lobby N" for one of
  // the five games. Five matches at once share this stream, so without it
  // their packets read as one interleaved game.
  { key: "scope",  label: "lobby",  min: 44,  def: 70 },
  { key: "page",   label: "page",   min: 40,  def: 74 },
  { key: "player", label: "from",   min: 40,  def: 108 },
  { key: "to",     label: "to",     min: 40,  def: 96,  off: true },
  { key: "kind",   label: "type",   min: 40,  def: 66 },
  { key: "cell",   label: "cell",   min: 36,  def: 52 },
  { key: "detail", label: "detail", min: 100, def: 300 },
  { key: "seq",    label: "seq",    min: 36,  def: 52,  off: true },
];

// Widths and which columns are showing, both remembered: someone who widens
// "detail" to read a long line, or hides "seq" because they are not chasing
// duplicates, does not want to do it again on the next reload.
const store = {
  read(key, fallback) {
    try {
      return JSON.parse(localStorage.getItem(key) ?? "null") ?? fallback;
    } catch {
      return fallback; // a corrupt entry is not worth failing the page over
    }
  },
  write(key, value) {
    try {
      localStorage.setItem(key, JSON.stringify(value));
    } catch {
      /* private windows refuse to store; it still works for this session */
    }
  },
};

let widths = store.read("colWidths", {});
let hidden = new Set(
  store.read("colHidden", COLUMNS.filter((c) => c.off).map((c) => c.key)),
);

const shown = () => COLUMNS.filter((c) => !hidden.has(c.key));
const widthOf = (c) => Math.max(c.min, Math.round(widths[c.key] ?? c.def));

// The table is rebuilt from COLUMNS rather than written out in HTML, so a
// column added there needs no matching edit to the page.
//
// Every column is always present. Collapsing one narrows it to its label and
// blanks its cells; it does not leave the table. That keeps the control where
// the column is -- a header that vanished would need a second place to put
// the button that brings it back, and then two places to look.
function buildHead() {
  const cols = $("cols");
  const head = $("logHead");
  cols.innerHTML = "";
  head.innerHTML = "";

  const last = COLUMNS[COLUMNS.length - 1];
  for (const c of COLUMNS) {
    const off = hidden.has(c.key);

    const col = document.createElement("col");
    col.style.width = `${off ? collapsedWidth(c) : widthOf(c)}px`;
    col.dataset.key = c.key;
    cols.appendChild(col);

    const th = document.createElement("th");
    th.className = off ? `${c.key} collapsed` : c.key;
    th.dataset.key = c.key;

    const label = document.createElement("span");
    label.className = "label";
    label.textContent = c.label;
    th.appendChild(label);

    const chev = document.createElement("button");
    chev.className = "chev";
    // Down closes, up opens -- the chevron points the way the column will go.
    chev.textContent = off ? "\u25B4" : "\u25BE";
    chev.title = off ? `show ${c.label}` : `hide ${c.label}`;
    chev.addEventListener("click", () => setHidden(c.key, !off));
    th.appendChild(chev);

    // A collapsed column has no width worth dragging, and the last column has
    // nothing to its right to drag against.
    if (!off && c !== last) {
      const grip = document.createElement("span");
      grip.className = "grip";
      grip.addEventListener("pointerdown", (e) => startResize(c.key, e));
      grip.addEventListener("dblclick", () => autoFit(c.key));
      th.appendChild(grip);
    }
    head.appendChild(th);
  }
}

// Just enough for the label and its chevron, so the header sits hard against
// the column before it. Measured rather than guessed, since the labels differ
// in length and a fixed number would clip "detail" or leave "to" adrift.
function collapsedWidth(c) {
  const probe = document.createElement("canvas").getContext("2d");
  const style = window.getComputedStyle($("logTable"));
  probe.font = `10px ${style.fontFamily}`;
  return Math.round(probe.measureText(c.label).width) + 26;
}

function setHidden(key, hide) {
  if (hide) hidden.add(key);
  else hidden.delete(key);
  store.write("colHidden", [...hidden]);
  buildHead();
  redrawRows();
}

function colEl(key) {
  return $("cols").querySelector(`col[data-key="${key}"]`);
}

function setWidth(key, px) {
  const c = COLUMNS.find((x) => x.key === key);
  const w = Math.max(c.min, Math.round(px));
  widths[key] = w;
  const el = colEl(key);
  if (el) el.style.width = `${w}px`;
  return w;
}

function startResize(key, ev) {
  ev.preventDefault();
  const grip = ev.currentTarget;
  const startX = ev.clientX;
  const startW = widthOf(COLUMNS.find((c) => c.key === key));

  grip.classList.add("dragging");
  document.body.classList.add("resizing");
  // Pointer capture keeps the drag alive once the pointer leaves the grip,
  // which it does immediately -- that is the whole point of dragging.
  grip.setPointerCapture?.(ev.pointerId);

  const move = (e) => setWidth(key, startW + (e.clientX - startX));
  const stop = () => {
    grip.classList.remove("dragging");
    document.body.classList.remove("resizing");
    window.removeEventListener("pointermove", move);
    window.removeEventListener("pointerup", stop);
    store.write("colWidths", widths);
  };
  window.addEventListener("pointermove", move);
  window.addEventListener("pointerup", stop);
}

// Double-click a divider to fit the column to its widest line, as a
// spreadsheet does. Measured with a canvas rather than by reflowing the table,
// which would mean laying every row out twice to ask how wide it is.
function autoFit(key) {
  const probe = document.createElement("canvas").getContext("2d");
  const style = window.getComputedStyle($("logTable"));
  probe.font = `${style.fontSize} ${style.fontFamily}`;

  let widest = 0;
  for (const row of $("logRows").children) {
    const cell = row.querySelector(`td.${key}`);
    if (cell) widest = Math.max(widest, probe.measureText(cell.textContent).width);
  }
  if (widest > 0) {
    setWidth(key, widest + 14); // padding, plus a little air
    store.write("colWidths", widths);
  }
}

// ---- server output ----------------------------------------------------
// Only append what is new, and only stick to the bottom if the reader was
// already there -- scrolling back to read something must not be yanked away
// by the next line arriving.
async function pollLog() {
  for (;;) {
    try {
      // The admin token, when there is one: without it the relay holds back
      // the cell of every fleet line, since a ship's position is the only
      // secret in the game and the log is served to the whole WiFi.
      const res = await fetch(`/v1/log?since=${logCursor}`, {
        headers: adminToken ? { "X-Admin-Token": adminToken } : {},
      });
      if (!res.ok) throw new Error(res.status);
      const { lines, next } = await res.json();
      logCursor = next;

      const box = $("log");
      const atBottom = box.scrollHeight - box.scrollTop - box.clientHeight < 40;
      for (const line of lines) {
        kept.push(line);
        $("logRows").appendChild(buildRow(line));
      }
      // Kept as data, not only as DOM, so toggling a column can redraw what is
      // already on screen instead of only affecting lines that arrive later.
      while (kept.length > KEEP) kept.shift();
      while ($("logRows").childElementCount > KEEP) {
        $("logRows").removeChild($("logRows").firstChild);
      }
      if (atBottom) box.scrollTop = box.scrollHeight;

      setConnected(true, "relay up");
    } catch {
      setConnected(false, "relay unreachable");
    }
    await new Promise((r) => setTimeout(r, 700));
  }
}

// The CSV carries ship positions, so it is behind the admin token like the
// other control routes -- which means it cannot be a plain link any more: a
// link cannot carry a header. Fetched, then handed to the browser as a blob.
async function downloadCsv() {
  try {
    const res = await fetch("/v1/packets.csv", {
      headers: adminToken ? { "X-Admin-Token": adminToken } : {},
    });
    if (res.status === 401 || res.status === 403) {
      askForToken("The CSV carries ship positions, so it needs the admin token.");
      return;
    }
    if (!res.ok) throw new Error(res.status);
    const url = URL.createObjectURL(await res.blob());
    const a = document.createElement("a");
    a.href = url;
    a.download = `battleship-${new Date().toISOString().slice(0, 19).replace(/[:T]/g, "")}.csv`;
    a.click();
    URL.revokeObjectURL(url);
  } catch (err) {
    $("restartNote").textContent = `Could not download the CSV: ${err}`;
  }
}

// ---- what this browser remembers ---------------------------------------
//
// Column widths, collapsed columns, the panel sizes, the lobby being watched
// and the admin token. All of it is local: none of it is on the relay, so
// clearing it changes what this browser shows and nothing about any game.
//
// Listed by key rather than clearing the whole origin, so anything else
// stored under it is left alone.
const REMEMBERED = [
  "colWidths", "colHidden", "splitCol", "splitRow", "splitHalf", "watchRoom",
  "adminToken",
];

function forgetLocal() {
  let cleared = 0;
  for (const key of REMEMBERED) {
    try {
      if (localStorage.getItem(key) !== null) cleared++;
      localStorage.removeItem(key);
    } catch {
      $("forgetNote").textContent =
        "This browser will not let the page clear its storage.";
      return;
    }
  }
  $("forgetNote").textContent =
    `Cleared ${cleared} saved setting${cleared === 1 ? "" : "s"}. Reloading.`;
  // Reloaded rather than re-rendered: the widths, the collapsed columns and
  // the splitters are read once at startup, and putting each of them back by
  // hand is a second way of doing what a reload already does correctly.
  setTimeout(() => location.reload(), 400);
}

// ---- panel sizes --------------------------------------------------------
//
// Three splitters, one rule between them: each owns a CSS variable on the
// element whose tracks it divides, so a drag writes one custom property and
// the browser does the layout.
//
//   --col   on .grid    the server output against everything else
//   --row   on .stack   players against controls
//   --half  on .split   the main lobby against the lobby being watched
//
// All three are remembered, all three clamp so neither side can be squeezed
// out of existence, all three reset on a double-click, and all three move
// with the arrow keys -- they are `role="separator"` buttons, so a keyboard
// has to be able to work them.
//
// Below the mobile breakpoint they are display:none and the panels stack, so
// nothing here runs: offsetParent is null for a hidden element, which is the
// check every entry point makes.
const SPLITS = {
  splitH: { box: () => document.querySelector(".grid"), prop: "--col", key: "splitCol", axis: "x", min: 260 },
  splitV: { box: () => $("stack"), prop: "--row", key: "splitRow", axis: "y", min: 150 },
  splitP: { box: () => document.querySelector(".split"), prop: "--half", key: "splitHalf", axis: "x", min: 170 },
};

// The gutter the splitter itself occupies, so the far side keeps its minimum
// rather than its minimum minus the bar.
const GUTTER = 14;

function applySizes() {
  for (const cfg of Object.values(SPLITS)) {
    const saved = store.read(cfg.key, null);
    if (saved) cfg.box().style.setProperty(cfg.prop, saved);
  }
}

// Where a splitter would sit for a given pointer position, clamped so both
// sides keep MIN. Returns pixels.
function clampSize(cfg, px) {
  const r = cfg.box().getBoundingClientRect();
  const span = cfg.axis === "x" ? r.width : r.height;
  return Math.max(cfg.min, Math.min(px, span - cfg.min - GUTTER));
}

function setSize(cfg, px) {
  cfg.box().style.setProperty(cfg.prop, `${Math.round(px)}px`);
  store.write(cfg.key, cfg.box().style.getPropertyValue(cfg.prop));
}

function currentSize(cfg) {
  const set = cfg.box().style.getPropertyValue(cfg.prop);
  if (set.endsWith("px")) return parseFloat(set);
  // Never dragged: measure the first track, which is what 1fr resolved to.
  const first = cfg.box().firstElementChild.getBoundingClientRect();
  return cfg.axis === "x" ? first.width : first.height;
}

function resetSize(cfg) {
  cfg.box().style.removeProperty(cfg.prop);
  store.write(cfg.key, null);
}

function dragSplit(handle, cfg) {
  // Two presses in quick succession with no drag between them is a
  // double-click, counted here rather than left to the dblclick event: the
  // pointer capture below swallows it, and a splitter that cannot be put back
  // is one people are wary of moving in the first place.
  let lastDown = 0;
  let moved = false;

  // The pointer is captured, or a fast drag drops the bar the moment the
  // cursor outruns the 14px strip it started in.
  handle.addEventListener("pointerdown", (ev) => {
    if (handle.offsetParent === null) return;   // stacked: nothing to size
    ev.preventDefault();
    const now = performance.now();
    if (!moved && now - lastDown < 400) {
      lastDown = 0;
      resetSize(cfg);
      return;
    }
    lastDown = now;
    moved = false;
    handle.setPointerCapture(ev.pointerId);
    handle.classList.add("dragging");
    document.body.classList.add(cfg.axis === "x" ? "resizing-h" : "resizing-v");

    const r = cfg.box().getBoundingClientRect();
    const origin = cfg.axis === "x" ? r.left : r.top;

    const move = (e) => {
      moved = true;
      const at = (cfg.axis === "x" ? e.clientX : e.clientY) - origin;
      setSize(cfg, clampSize(cfg, at));
    };
    const stop = (e) => {
      handle.classList.remove("dragging");
      document.body.classList.remove("resizing-h", "resizing-v");
      if (e && handle.hasPointerCapture(e.pointerId)) handle.releasePointerCapture(e.pointerId);
      handle.removeEventListener("pointermove", move);
      handle.removeEventListener("pointerup", stop);
      handle.removeEventListener("pointercancel", stop);
    };

    handle.addEventListener("pointermove", move);
    handle.addEventListener("pointerup", stop);
    handle.addEventListener("pointercancel", stop);
  });

  // And the keyboard. A separator that can only be dragged is a control half
  // the people using it cannot reach.
  handle.addEventListener("keydown", (e) => {
    if (handle.offsetParent === null) return;
    const step = e.shiftKey ? 48 : 12;
    const back = cfg.axis === "x" ? "ArrowLeft" : "ArrowUp";
    const on = cfg.axis === "x" ? "ArrowRight" : "ArrowDown";
    if (e.key === back || e.key === on) {
      e.preventDefault();
      setSize(cfg, clampSize(cfg, currentSize(cfg) + (e.key === on ? step : -step)));
      return;
    }
    // Home, Enter or space put it back where it started, which is the way out
    // of a drag that went somewhere silly.
    if (e.key === "Home" || e.key === "Enter" || e.key === " ") {
      e.preventDefault();
      resetSize(cfg);
    }
  });
}

// ---- the main lobby, and the game lobby being watched -------------------
//
// Five games run at once, so "the players" is a question with five answers.
// The left half is the main lobby exactly as the players see it on their own
// panels; the right half is whichever of the five the operator has picked.
//
// Remembered, because someone watching lobby 3 does not want to find
// themselves back on lobby 1 after a reload.
let watching = Number(store.read("watchRoom", 1)) || 1;

function selectRoom(n) {
  watching = n;
  store.write("watchRoom", n);
  $("roomTitle").textContent = `Lobby ${n}`;
  // The controls act on the lobby being watched, so the header says which one
  // -- a page button that moves somebody is worth being unambiguous about.
  $("controlsLobby").textContent = `\u2014 lobby ${n}`;
  renderActions();
}

function renderRooms(rooms) {
  const box = $("rooms");
  box.innerHTML = "";
  for (const r of rooms) {
    const players = r.players ?? [];
    const b = document.createElement("button");
    b.className = "room" +
      (r.n === watching ? " sel" : "") +
      (players.length >= 2 ? " playing" : "");

    const n = document.createElement("span");
    n.className = "n";
    n.textContent = String(r.n);
    b.appendChild(n);

    const who = document.createElement("span");
    who.className = "who";
    // textContent throughout: a player types their own name.
    who.textContent =
      players.length === 0 ? "no players"
      : players.length === 1 ? players[0]
      : `${players[0]} vs ${players[1]}`;
    b.appendChild(who);

    const count = document.createElement("span");
    count.className = "count";
    count.textContent = `${players.length}/2`;
    b.appendChild(count);

    b.addEventListener("click", () => { selectRoom(r.n); void refreshMatch(); });
    box.appendChild(b);
  }
  const busy = rooms.filter((r) => (r.players ?? []).length > 0).length;
  $("roomsNote").textContent = busy === 0
    ? "Nobody has picked a lobby yet."
    : `${busy} of ${rooms.length} lobbies in use. Click one to watch it.`;
}

// The log, filtered to the lobby being watched: who did what, most recent
// last. It is the same data the server-output panel shows, read the other way
// round -- by player rather than by packet.
function renderActions() {
  const box = $("actions");
  box.innerHTML = "";
  const want = `lobby ${watching}`;
  const mine = kept.filter((l) => l.scope === want).slice(-12);
  if (mine.length === 0) {
    box.innerHTML = `<p class="empty">nothing has happened in this lobby yet</p>`;
    return;
  }
  for (const line of mine) {
    const row = document.createElement("div");
    row.className = "act";

    const at = document.createElement("span");
    at.className = "at";
    at.textContent = stamp(new Date(line.at));
    row.appendChild(at);

    const who = document.createElement("span");
    who.className = "who";
    who.textContent = line.player ?? "";
    row.appendChild(who);

    const what = document.createElement("span");
    what.className = "what";
    // A packet says what it was and where; a remark is already a sentence.
    what.textContent = line.kind
      ? [line.kind, line.cell, line.detail].filter(Boolean).join(" ")
      : (line.detail ?? line.text ?? "");
    row.appendChild(what);

    box.appendChild(row);
  }
  box.scrollTop = box.scrollHeight;
}

async function pollRooms() {
  for (;;) {
    try {
      const res = await fetch("/v1/rooms");
      if (res.ok) renderRooms((await res.json()).rooms ?? []);
    } catch {
      /* the log poll already reports the relay being down */
    }
    await new Promise((r) => setTimeout(r, 700));
  }
}

// ---- players and match state -------------------------------------------
async function refreshMatch() {
  try {
    const res = await fetch(`/v1/match?room=${watching}`);
    if (!res.ok) throw new Error(res.status);
    const m = await res.json();
    bothReady = m.bothReady;

    {
      const seats = $("seats");
      seats.innerHTML = "";
      if (!m.seats || m.seats.length === 0) {
        seats.innerHTML = `<p class="empty">nobody is seated in this lobby</p>`;
      } else {
        for (const s of m.seats) {
          const ships = m.fleet?.[s.peer] ?? 0;
          const state = m.ready?.[s.peer]
            ? "fleet placed"
            : ships > 0
              ? `placing (${ships}/5 reported)`
              : "not placed";
          const where = m.phase?.[s.peer] ?? "start";
          const quiet = Math.max(0, Math.floor(Date.now() / 1000 - s.lastSeen));
          const row = document.createElement("div");
          row.className = "seat";
          row.innerHTML =
            `<span class="id">${s.peer.toUpperCase()}</span>` +
            `<span class="name"></span>` +
            `<span class="state">on ${where} &middot; ${state} &middot; seen ${quiet}s ago</span>`;
          row.querySelector(".name").textContent = s.name;
          seats.appendChild(row);
        }
      }

      $("match").innerHTML =
        `<tr><td class="k">both fleets down</td><td>${m.bothReady ? "yes" : "no"}</td></tr>` +
        `<tr><td class="k">shots fired</td><td>${m.shots}</td></tr>`;
      $("matchNote").textContent = m.bothReady
        ? "Ready to play."
        : "Gameplay stays locked until both fleets are placed.";
      renderPages();
      renderActions();
    }
  } catch {
    /* the log poll already reports the relay being down */
  }
}

async function pollMatch() {
  for (;;) {
    await refreshMatch();
    await new Promise((r) => setTimeout(r, 700));
  }
}

// ---- force page ---------------------------------------------------------
function renderPages() {
  const box = $("pages");
  if (box.childElementCount === 0) {
    for (const p of PAGES) {
      const b = document.createElement("button");
      b.textContent = p.label;
      b.title = p.why;
      b.dataset.page = String(p.n);
      b.dataset.needsFleets = p.needsFleets ? "1" : "";
      b.addEventListener("click", () => forcePage(p));
      box.appendChild(b);
    }
  }
  for (const b of box.children) {
    b.disabled = b.dataset.needsFleets === "1" && !bothReady;
  }
  $("pageNote").textContent = bothReady
    ? `Both boards in lobby ${watching} jump on their next poll.`
    : "Gameplay is greyed out: both fleets in this lobby must be placed first.";
}

async function forcePage(p) {
  try {
    // Named with the lobby being watched: the relay sends the page to that
    // one table, and refuses "match" unless that table's fleets are down.
    const res = await fetch(`/v1/force-page?page=${p.n}&room=${watching}`, {
      method: "POST",
      headers: adminToken ? { "X-Admin-Token": adminToken } : {},
    });
    if (res.status === 401) {
      askForToken("The relay printed an admin token at startup. Paste it here:");
      return;
    }
    if (!res.ok) {
      const body = await res.json().catch(() => ({}));
      $("pageNote").textContent = body.error ?? `refused (${res.status})`;
    }
  } catch (err) {
    $("pageNote").textContent = String(err);
  }
}

function askForToken(why) {
  const given = window.prompt(why, adminToken);
  if (given === null) return;
  adminToken = given.trim();
  localStorage.setItem("adminToken", adminToken);
  $("pageNote").textContent = adminToken ? "Token saved. Try again." : "Token cleared.";
}

$("token").addEventListener("click", () =>
  askForToken("Admin token (printed by the relay at startup):"));

// ---- restart -----------------------------------------------------------
async function post(path, note) {
  try {
    const res = await fetch(path, {
      method: "POST",
      headers: adminToken ? { "X-Admin-Token": adminToken } : {},
    });
    if (res.status === 401) {
      askForToken("The relay printed an admin token at startup. Paste it here:");
      return;
    }
    const body = await res.json().catch(() => ({}));
    $("restartNote").textContent = res.ok ? note : (body.error ?? `refused (${res.status})`);
  } catch (err) {
    $("restartNote").textContent = String(err);
  }
}

$("restart").addEventListener("click", () => {
  post("/v1/reset", "Match history cleared. Players stay where they are.");
});

$("hardRestart").addEventListener("click", () => {
  post(
    "/v1/restart",
    "Both players sent to the title, still connected -- the handshake will " +
      "complete on their next poll.",
  );
});

$("csv").addEventListener("click", () => {
  void downloadCsv();
});

$("where").textContent = location.host;
buildHead();
renderPages();
$("forget").addEventListener("click", forgetLocal);

applySizes();
for (const [id, cfg] of Object.entries(SPLITS)) dragSplit($(id), cfg);

selectRoom(watching);   // names the panel and draws whatever is already known
void pollLog();
void pollMatch();
void pollRooms();
