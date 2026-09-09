#pragma once
#include <stdint.h>
#include "screen.h"

// The 10x10 battle grid and its pixel geometry. Portable: it draws into a
// gfx::Screen and knows nothing about which panel that Screen belongs to.
//
// One cell is CELL px on a side and neighbours share a border, so ten cells
// close at 10*5 + 1 = 51 px: lines sit at 0,5,10,...,50 with no short step at
// the end. Fifty would force the last row and column a pixel narrow, which is
// visible as an uneven gap in the dot grid.
namespace game {

constexpr int COLS = 10;
constexpr int ROWS = 10;
constexpr int CELL = 5;
constexpr int GRID_PX = COLS * CELL + 1;  // 51, closing border included

// Labels live in the gutters: rows A..J to the left, columns 0..9 above.
// The tiny font is 3x5, so the gutters are 4 and 6 px.
constexpr int LABEL_W = 4;
constexpr int LABEL_H = 6;
constexpr int GLYPH_W = 3;  // one tiny glyph
constexpr int GLYPH_H = 5;

// Clearance between the row letters and the grid. Two pixels, not one,
// because ORIGIN_X takes the odd pixel of centring slack: the letters keep
// the column they have always been drawn in, and the pixel ORIGIN_X gained
// shows up as clearance between them and the grid's left wall.
constexpr int LABEL_GAP = 2;

// Centred on a 128x64 panel, labels included. The grid and its left gutter
// are 55px wide, so the 73px of slack does not halve evenly and the division
// throws the odd pixel to the right of the grid; +1 hands it back to the left,
// which is where the dot grid and the crosshair were already being drawn.
constexpr int ORIGIN_X = (gfx::W - (GRID_PX + LABEL_W)) / 2 + LABEL_W + 1;  // 41
constexpr int ORIGIN_Y = (gfx::H - (GRID_PX + LABEL_H)) / 2 + LABEL_H;  // 9

// Plain aggregate: the ESP toolchain compiles this as C++11, where default
// member initializers would make brace initialization ill-formed.
// col and row are 0..9 on the board. Both also accept -1, which selects the
// label strip on that axis rather than a cell: (-1, -1) is the corner where
// the letters and the numbers meet, off the board entirely.
struct Coord { int col; int row; };
constexpr int HEADER = -1;

// What one cell shows. Empty and Ship are board state; Hit and Miss are shot
// marks, and are the only two the tracking grid ever holds.
enum class CellState : uint8_t { Empty, Ship, Hit, Miss };

// Left edge of the row letters, and the top of the column numbers.
constexpr int ROW_LABEL_X = ORIGIN_X - LABEL_GAP - GLYPH_W;  // 36, unmoved
constexpr int COL_LABEL_Y = ORIGIN_Y - LABEL_H;              // 3

// Top-left pixel of a cell's interior (inside its border).
inline int cellX(int col) { return ORIGIN_X + col * CELL; }
inline int cellY(int row) { return ORIGIN_Y + row * CELL; }

// Solid ruled grid: every wall drawn, plus the A..J / 0..9 labels. Used for
// your own board on the bottom panel.
void drawGrid(gfx::Screen &s, bool labels = true);

// The same geometry reduced to a single dot at each cell corner. Used for the
// grid you attack, on the top panel, so the crosshair is the only solid line
// on that panel and reads at a glance.
void drawDotGrid(gfx::Screen &s, bool labels = true);

// One cell's contents: a filled block for Ship, an X for Hit, an O for Miss.
void drawCell(gfx::Screen &s, Coord c, CellState st);

// A whole vessel as one shape rather than a run of cells: a solid capsule
// spanning its cells with rounded ends, like the ships in Figure 4 of the
// rulebook. No peg holes -- the hull is solid. The two shortest classes get a
// longer taper so their bows stay distinct at this size.
void drawShip(gfx::Screen &s, Coord origin, int len, bool vertical);

// The same silhouette at an arbitrary pixel position and size, off the grid.
// With outline = true only its edge is drawn, which is how the enemy fleet is
// listed beside the target grid.
void drawShipAt(gfx::Screen &s, int x, int y, int w, int h, int len, bool outline);

// The pixel column / row of a cell's own walls. Every cell is a full CELL
// step, the last included, because the grid carries its closing border.
int wallLeft(int col);
int wallRight(int col);
int wallTop(int row);
int wallBottom(int row);

// The panel is divided into three by two vertical rules: a left gutter, the
// grid, and a right gutter. Both panels use the same split, so the geometry
// lives here with the grid it is measured from rather than in a page.
constexpr int LEFT_RULE = ROW_LABEL_X - 2;
constexpr int RIGHT_RULE = ORIGIN_X + GRID_PX + 3;
constexpr int GUTTER_L_LEFT = 1;
constexpr int GUTTER_L_RIGHT = LEFT_RULE - 1;
constexpr int GUTTER_R_LEFT = RIGHT_RULE + 1;
constexpr int GUTTER_R_RIGHT = gfx::W - 2;

// The border and the two rules that box the grid off from its gutters, with
// the middle section closed top and bottom tight to the grid's own labels.
void drawPanelFrame(gfx::Screen &s);

// A hit on YOUR OWN board: the cell is filled and the X knocked out of it in
// unlit pixels. Deliberately not drawCell(Hit), which draws a lit X on an
// unlit cell -- that is the tracking-grid mark, for a hit you scored. These
// two must stay distinguishable at a glance, since one panel shows each.
void drawOwnHit(gfx::Screen &s, Coord c);

// A shot of theirs that found open water, on your own board: a 2x2 dot in the
// middle of the cell. Small on purpose -- it is a record of where they have
// been, and must not be mistaken for a ship or for damage.
void drawIncomingMiss(gfx::Screen &s, Coord c);

// A line struck along a sunk ship's axis, drawn over its hull.
void drawStrike(gfx::Screen &s, Coord origin, int len, bool vertical);

// Targeting reticle: four solid lines on the selected cell's own walls,
// spanning the grid and nothing beyond it, so it never reaches the A..J /
// 0..9 labels in the gutters. Against a dot grid these are the only unbroken
// lines on the panel.
void drawCrosshair(gfx::Screen &s, Coord c);

// The selection when the cursor is off the board on the row axis: the numbers
// strip is inverted. Draw it *before* the crosshair, so the crosshair's own
// lines are not flipped along with the strip and left as gaps in it.
void drawHeaderSelection(gfx::Screen &s, Coord c);

// The corner where the two strips meet, drawn when both axes are off the
// board. A filled square inside an unlit border, so it stays readable over
// whatever the crosshair has already drawn: call it last.
void drawHeaderCorner(gfx::Screen &s, Coord c);

}  // namespace game
