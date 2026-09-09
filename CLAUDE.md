# Working rules for this repo

1. **Keep `README.md` updated whenever a file is updated. If a new file is
   created, make sure it is added to the diagram.** Both the architecture
   mermaid diagram and the Layout table in `README.md` must stay accurate:
   a new source file means a new node in the diagram (in the right subgraph
   for its target) and a new row in the table. A file that changes role, moves,
   or is deleted must be updated or removed in both places in the same change.

2. **`src/game/` is the portable core and must stay hardware-free.** No
   `Arduino.h`, `Serial`, `delay()`, `millis()`, `Wire.h`, or anything from
   `include/`. Time comes from the `nowMs` argument to `Game::tick()`. It is
   compiled unchanged by both the ESP32 firmware and the WASM build, so
   anything hardware-specific breaks the browser target.

3. **Never model the two panels as one canvas.** They are physically separate
   128x64 devices on separate buses, each with its own 0..63 coordinate space
   and its own `gfx::Screen`.

4. **`gfx::Screen` storage is the SSD1306 page format** (byte `x + (y/8)*128`
   holds 8 vertically stacked pixels, LSB topmost). Both backends rely on this
   for a straight 1024-byte copy. Do not change the layout without updating
   `include/display.h` and `web/main.ts` together.

5. **Verify before claiming done.** `make check` type-checks the core in about
   a second; `pio run` builds all three firmware environments; `make web`
   builds WASM and TypeScript. Run what the change touches, and report actual
   output rather than assuming.

6. **Timing must be derived from `nowMs`, not frame count.** The browser ticks
   at ~60Hz via `requestAnimationFrame`, the device at ~50Hz.

7. **Don't commit build output.** `web/sim.js`, `web/sim.wasm`, `web/main.js`
   and `.pio/` are generated.
