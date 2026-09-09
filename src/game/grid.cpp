#include "grid.h"

namespace game {

namespace {
// The labels are the same either way, so both grid styles share them.
void drawLabels(gfx::Screen &s) {
  // Column numbers 0..9, one glyph per 5px column. A 3px glyph in a column
  // whose walls are 5px apart centres at +1; +2 sits it a pixel right of that,
  // which reads better against the digits' own left-heavy stems.
  for (int c = 0; c < COLS; c++) {
    const char lbl[2] = {static_cast<char>('0' + c), '\0'};
    s.textTiny(cellX(c) + 2, COL_LABEL_Y, lbl);
  }
  // Row letters A..J.
  for (int r = 0; r < ROWS; r++) {
    const char lbl[2] = {static_cast<char>('A' + r), '\0'};
    s.textTiny(ROW_LABEL_X, cellY(r) + 1, lbl);
  }
}
}  // namespace

// HEADER is a position like any other, so it has walls too: the label strip's
// own edges, one pixel clear of the glyphs.
int wallLeft(int col)  { return col == HEADER ? ROW_LABEL_X - 2 : cellX(col); }
int wallRight(int col) { return col == HEADER ? ORIGIN_X : cellX(col + 1); }
int wallTop(int row)   { return row == HEADER ? ORIGIN_Y - LABEL_H - 1 : cellY(row); }
int wallBottom(int row) { return row == HEADER ? ORIGIN_Y - 1 : cellY(row + 1); }

void drawGrid(gfx::Screen &s, bool labels) {
  // Outer box is GRID_PX x GRID_PX; every line lands on a multiple of CELL.
  s.drawRect(ORIGIN_X, ORIGIN_Y, GRID_PX, GRID_PX, true);
  for (int i = 1; i < COLS; i++) s.vLine(cellX(i), ORIGIN_Y, GRID_PX, true);
  for (int i = 1; i < ROWS; i++) s.hLine(ORIGIN_X, cellY(i), GRID_PX, true);

  if (labels) drawLabels(s);
}

void drawDotGrid(gfx::Screen &s, bool labels) {
  // One dot per corner: 11 x 11 of them, evenly spaced by CELL on both axes.
  for (int r = 0; r <= ROWS; r++)
    for (int c = 0; c <= COLS; c++) s.pixel(cellX(c), cellY(r), true);

  // And a line all the way round. A field of dots says where the cells are
  // but not where the board stops: the outermost dots are the same as every
  // other dot, so a crosshair at the edge looks like a crosshair anywhere
  // else and there is nothing to tell you it can go no further.
  s.drawRect(cellX(0), cellY(0), GRID_PX, GRID_PX, true);

  if (labels) drawLabels(s);
}

void drawCell(gfx::Screen &s, Coord c, CellState st) {
  if (c.col < 0 || c.col >= COLS || c.row < 0 || c.row >= ROWS) return;
  // Interior is the 4x4 inside the cell's own border lines.
  const int x = cellX(c.col) + 1, y = cellY(c.row) + 1;
  switch (st) {
    case CellState::Empty:
      break;
    case CellState::Ship:
      s.fillRect(x, y, CELL - 1, CELL - 1, true);
      break;
    case CellState::Hit:  // X
      for (int i = 0; i < 4; i++) { s.pixel(x + i, y + i, true); s.pixel(x + 3 - i, y + i, true); }
      break;
    case CellState::Miss:  // O
      s.drawRect(x, y, 4, 4, true);
      break;
  }
}

void drawShipAt(gfx::Screen &s, int x, int y, int w, int h, int len, bool outline) {
  if (w <= 0 || h <= 0) return;
  const bool vertical = h > w;

  // Build the silhouette off-screen so the outline can be traced from it.
  static gfx::Screen shape;
  shape.clear();
  shape.fillRect(x, y, w, h, true);

  // Rounded ends: clip the four corner pixels.
  shape.pixel(x, y, false);
  shape.pixel(x + w - 1, y, false);
  shape.pixel(x, y + h - 1, false);
  shape.pixel(x + w - 1, y + h - 1, false);

  // A destroyer or a cruiser is short enough that a one-pixel chamfer reads
  // as a rectangle, so their bow and stern taper over two pixels instead.
  if (len <= 3) {
    for (int i = 0; i < 2; i++) {
      if (vertical) {
        const int top = y + i, bot = y + h - 1 - i;
        shape.pixel(x, top, false);         shape.pixel(x + w - 1, top, false);
        shape.pixel(x, bot, false);         shape.pixel(x + w - 1, bot, false);
      } else {
        const int lef = x + i, rig = x + w - 1 - i;
        shape.pixel(lef, y, false);         shape.pixel(lef, y + h - 1, false);
        shape.pixel(rig, y, false);         shape.pixel(rig, y + h - 1, false);
      }
    }
  }

  for (int py = y; py < y + h; py++) {
    for (int px = x; px < x + w; px++) {
      if (!shape.pixelAt(px, py)) continue;
      if (outline) {
        // Interior pixels are the ones fully surrounded; drop those.
        const bool inner = shape.pixelAt(px - 1, py) && shape.pixelAt(px + 1, py) &&
                           shape.pixelAt(px, py - 1) && shape.pixelAt(px, py + 1);
        if (inner) continue;
      }
      s.pixel(px, py, true);
    }
  }
}

void drawShip(gfx::Screen &s, Coord origin, int len, bool vertical) {
  if (len <= 0) return;
  const int lastCol = vertical ? origin.col : origin.col + len - 1;
  const int lastRow = vertical ? origin.row + len - 1 : origin.row;
  if (origin.col < 0 || origin.row < 0 || lastCol >= COLS || lastRow >= ROWS) return;

  // The hull fills the cell interiors and the walls between them, stopping one
  // pixel inside the walls at either end.
  const int x = cellX(origin.col) + 1;
  const int y = cellY(origin.row) + 1;
  const int w = vertical ? CELL - 1 : wallRight(lastCol) - x;
  const int h = vertical ? wallBottom(lastRow) - y : CELL - 1;

  // Clear a one-pixel margin first. Without it the hull merges into the grid
  // walls it touches and the fleet reads as mesh rather than as five ships;
  // with it, each vessel sits on the board the way it does in Figure 4.
  s.fillRect(x - 1, y - 1, w + 2, h + 2, false);
  drawShipAt(s, x, y, w, h, len, false);
}

void drawPanelFrame(gfx::Screen &s) {
  s.drawRect(0, 0, gfx::W, gfx::H, true);
  s.vLine(LEFT_RULE, 0, gfx::H, true);
  s.vLine(RIGHT_RULE, 0, gfx::H, true);

  constexpr int TOP_RULE = COL_LABEL_Y - 2;            // clear of the numbers
  constexpr int BOTTOM_RULE = ORIGIN_Y + GRID_PX + 1;  // clear of the grid
  const int span = RIGHT_RULE - LEFT_RULE + 1;
  s.hLine(LEFT_RULE, TOP_RULE, span, true);
  s.hLine(LEFT_RULE, BOTTOM_RULE, span, true);

  // The middle box is closed by those two rules, so the panel border's own
  // top and bottom edges are redundant across it: drop them and leave the
  // section open to the edge of the screen.
  s.hLine(LEFT_RULE + 1, 0, span - 2, false);
  s.hLine(LEFT_RULE + 1, gfx::H - 1, span - 2, false);
}

void drawOwnHit(gfx::Screen &s, Coord c) {
  if (c.col < 0 || c.col >= COLS || c.row < 0 || c.row >= ROWS) return;
  const int x = cellX(c.col) + 1, y = cellY(c.row) + 1;
  s.fillRect(x, y, CELL - 1, CELL - 1, true);
  // The X is cut out of the filled cell, so the damage reads as a hole in the
  // hull rather than as another mark laid on top of it.
  for (int i = 0; i < 4; i++) {
    s.pixel(x + i, y + i, false);
    s.pixel(x + 3 - i, y + i, false);
  }
}

void drawIncomingMiss(gfx::Screen &s, Coord c) {
  if (c.col < 0 || c.col >= COLS || c.row < 0 || c.row >= ROWS) return;
  // A cell is CELL px including its shared border, so its interior is
  // CELL - 1 = 4 px: a 2x2 dot sits in the middle of it with a pixel of water
  // all round, which is what keeps it from reading as a filled cell.
  const int x = cellX(c.col) + 2, y = cellY(c.row) + 2;
  s.fillRect(x, y, 2, 2, true);
}

void drawStrike(gfx::Screen &s, Coord origin, int len, bool vertical) {
  if (len <= 0) return;
  const int x = cellX(origin.col) + 1;
  const int y = cellY(origin.row) + 1;
  // Down the middle of the hull, from the first cell to the last.
  if (vertical) s.vLine(x + (CELL - 1) / 2, y, len * CELL - 1, true);
  else          s.hLine(x, y + (CELL - 1) / 2, len * CELL - 1, true);
}

void drawHeaderSelection(gfx::Screen &s, Coord c) {
  // Off the board on the row axis means the numbers are selected. Only the
  // glyph band is touched; the crosshair is laid over it afterwards, so its
  // columns stay lit rather than being inverted into blank gaps.
  if (c.row == HEADER) s.invertRect(ORIGIN_X, COL_LABEL_Y, GRID_PX, GLYPH_H);
}

void drawHeaderCorner(gfx::Screen &s, Coord c) {
  if (c.row != HEADER || c.col != HEADER) return;
  const int x = ROW_LABEL_X, y = COL_LABEL_Y;
  s.fillRect(x - 1, y - 1, GLYPH_W + 2, GLYPH_W + 2, false);  // border
  s.fillRect(x, y, GLYPH_W, GLYPH_W, true);                   // square
}

void drawCrosshair(gfx::Screen &s, Coord c) {
  if (c.col < HEADER || c.col >= COLS || c.row < HEADER || c.row >= ROWS) return;

  // The lines run from the far edge of the label strips to the far edge of
  // the grid, so a header position is bracketed by the same cross as a cell
  // and the two never disagree about where the cursor is.
  const int top = wallTop(HEADER), left = wallLeft(HEADER);
  const int vh = ORIGIN_Y + GRID_PX - top;
  const int hw = ORIGIN_X + GRID_PX - left;

  s.vLine(wallLeft(c.col), top, vh, true);
  s.vLine(wallRight(c.col), top, vh, true);
  s.hLine(left, wallTop(c.row), hw, true);
  s.hLine(left, wallBottom(c.row), hw, true);
}

}  // namespace game
