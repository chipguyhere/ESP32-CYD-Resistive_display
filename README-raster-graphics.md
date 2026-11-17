# Raster graphics on the CYD: a tutorial

For a developer — or an AI assistant writing code for one — who wants full-screen
animation on these boards without LVGL: a picture drawn a strip at a time, straight
to the panel, with no tearing. It collects what was learned building the two raster
examples, so a new sketch can skip the experiments that taught it:

- **`RasterBouncingBalls`** — the reference for the technique, and the tool that tests
  a board and saves what it can do. Start by reading it.
- **`RasterFireRainSnow`** — a whole-screen, landscape, two-core version: three
  animated scenes at up to about 30 frames a second on every board in the family.
  Most of the numbers below come from it.

The reasoning behind the tear-free timing is in
[README-engineering.md § Beam chasing](README-engineering.md#beam-chasing); this file
states what to do with it.

If you are an artist planning a piece, read **section 0** first. It needs no
programming, and it says what to design toward. The rest is for whoever writes the
code.

## 0. Designing for it: a guide for artists

These boards can show smooth, full-screen moving pictures. They do it with very
little memory, though, so the picture is never stored anywhere whole. It is
**painted fresh every frame**, a thin strip at a time, a split second before the
screen shows that strip. Everything about what's cheap and what's expensive follows
from that. Design with it in mind and the piece runs smoothly; design against it and
it stutters or tears (a visible line where the old picture meets the new).

**The canvas**

- **320 × 240** on the 2.4" to 3.2" boards, **480 × 320** on the 3.5" and 4.0" ones.
  Design at the size of your board, or for both.
- **About 29 frames a second, at most.** The screen sets this, not the code. Slow,
  graceful motion looks best; fast motion shows the frame steps.
- **65,536 colours (RGB565), not millions.** Smooth gradients show steps ("banding")
  unless the code dithers them, which it can do cheaply. Plan for dithered skies and
  shadows; very dark, slow gradients band the most.
- **The backlight can fade the whole picture to black and back for free.** It's the
  cheapest transition there is.

**What's cheap**

- **A painted background that doesn't change.** Skies, mountains, a moon, a room. It
  is prepared on a computer ahead of time and stored with the program. Show as much
  detail in it as you like. Make it **an image that is the same every frame**: moving
  it, scaling it or recolouring it costs much more (see below).
- **A background made of rules rather than pixels.** A sky that is a gradient, a grid
  floor in perspective, a pattern that scrolls. These cost almost nothing, and they
  can move.
- **Many small things.** Raindrops, snowflakes, sparks, stars, embers: hundreds of
  small dots and streaks are fine. Their cost is the area they cover, not their
  number.
- **Light that changes a whole area by the same amount.** A lightning flash, a sunset
  reddening, firelight flickering, a fade: all cheap.
- **Things that are soft.** Smoke, mist and clouds can be stored and drawn at half or a
  quarter of the detail, and nobody can tell.

**What's expensive**

- **Big see-through layers.** Every see-through pixel has to be mixed with what's
  behind it. A haze over the whole screen costs about as much as all the rest of the
  scene. Keep translucent effects to part of the screen.
- **Several layers stacked everywhere.** Each layer that covers a pixel adds to its
  cost. Three full-screen layers is too many; one full-screen layer plus small
  details is comfortable.
- **Scaling, rotating or bending big images.** These can be done, slowly and with
  care. The mountains coming closer in the snow scene cost a large share of that
  scene's time. Plan one such effect per scene, not several.
- **Lots of large stored images.** There is room for a few screen-sized pictures
  (2–3 MB in all), not a flipbook. An animation is better made of rules and particles
  than of stored frames.
- **Busy detail everywhere at once.** A frame has a fixed time budget. A scene with
  its busiest part in one region (a fire in the middle) is easier than one busy all
  over.

**Patterns that work**

- **A still painted backdrop, a procedural floor or sky, and particles on top.** All
  three scenes in `RasterFireRainSnow` are this, and it's a good starting shape.
- **Fake depth with size and speed.** Far things small, slow and faint; near things
  large, fast and bright. Cheap, and convincing.
- **Let light do the drama.** A flash, a glow or a colour shift over the whole scene
  reads as a big event and costs little.
- **Use the dark.** Scene changes during a fade to black hide any setup cost.
- **Try it before the board.** The examples can draw frames on a computer as images,
  so you can judge a look in seconds before anything is uploaded.

When an idea needs something on the "expensive" list, a bigger board may do it
easily. Section 11 says what each kind of extra hardware would make possible.

## 1. The shape of it

There is no frame buffer. A 240 × 320 frame is 150 KB, which a classic ESP32 can't
spare beside everything else, and sending a finished frame tears anyway. Instead:

1. The panel's rows are sent in **chunks** of 16 rows, from a few small DMA-capable
   **bounce buffers** (7.5 KB each on a 240-wide panel, 10 KB on a 320-wide one).
2. A **render task** draws each chunk into a free bounce buffer just before it is
   needed, and queues it.
3. A **raster task** owns the SPI bus. Each frame it waits for the right moment in the
   panel's own refresh, then sends the chunks in order as they arrive. Afterwards it
   reads the line the panel is scanning (command `0x45`) to keep its model of the scan
   in step.

Copy `scan.cpp` / `scan.h` and `raster.cpp` / `raster.h` from either example; they
are the plumbing. Your drawing goes in the render task. The details of the tasks and
the bus are in [README-engineering.md § Tasks and the bus](README-engineering.md#tasks-and-the-bus).

## 2. First: what can this board do?

Whether a picture can be tear-free depends on two numbers, and both differ from
board to board:

- ***w*, one row's send time.** At the normal 40 MHz SPI clock a 240-wide row takes
  about **97 µs**; at 80 MHz about **48 µs**. A 320-wide row at 80 MHz takes about
  **65 µs**.
- ***L*, one line of the panel's scan.** The stock refresh runs from about 57 Hz to
  95 Hz depending on the controller and the unit — 37 to 55 µs a line.

Writing *B* rows, the write drifts against the scan by *B* · (*w* − *L*). That drift
plus about 1 ms of guard must fit inside one refresh, or the scan overtakes the
write and the picture tears. For the **whole screen** (all 320 rows) that is a much
stricter test than for a part of it.

You don't work this out by hand. **Run `RasterBouncingBalls` on the board once.** It
saves two records in the board's NVS that your sketch reads:

| Record | Read with | Use |
|---|---|---|
| The 80 MHz speed test | `cydr_loadSpeedTest(rec)` — `rec.result == CYDR_SPEED_PASSED` | `display.changeSpiClock(80000000)` only when it passed |
| A slower refresh the user chose | `display.applyRefreshChoice()` | apply it only when the stock rate isn't tear-free; it returns false when nothing is saved |

```cpp
cydr_speed_test_t rec;
const bool tested = cydr_loadSpeedTest(rec);       // false: never tested, or an older test version
if (tested && rec.result == CYDR_SPEED_PASSED) display.changeSpiClock(80000000);
// ... time the scan, measure the transfer, plan ...
if (!plan.tearFree && display.applyRefreshChoice()) { /* re-time the scan, re-plan */ }
```

Rules that hold up:

- **Treat "no record" as "failed".** Stay at 40 MHz and tell the user to run the test
  (`RasterFireRainSnow` shows a notice on screen). A record from an older version of
  the test loads as no record, so a board tested long ago needs testing again.
- **Never slow the refresh on your own.** A slower refresh can flicker on some
  patterns, and only the user can judge that. `RasterBouncingBalls` shows test
  patterns and lets the user choose.
- **Full-screen sketches need the "whole screen" choice.** `RasterBouncingBalls`
  sizes its main offer for its 240-row ball area. When that rate isn't tear-free for
  all the rows, it adds a second choice, marked **"Whole screen tear-free too"**
  (Flip shows it). Take that one for a sketch like `RasterFireRainSnow`. On the boards
  tried, it came out at 56.7–58.4 Hz.

What the family looked like, all running `RasterFireRainSnow` tear-free:

| Board | Controller | SPI clock | Refresh | Frames a second |
|---|---|---|---|---|
| ESP32-32E 3.5"/4.0" (×2) | ST7796, 320 × 480 | 80 MHz | stock 57.4 / 59.1 Hz | 28.7 / 29.5 |
| ESP32-2432S032 | ST7789 | 80 MHz | stock 59.1 Hz | 29.4 |
| ESP32-32E 2.8" | ST7789 | 80 MHz | stock 59.8 Hz | 29.9 |
| ESP32-32E 2.8" | ILI9341 | 40 MHz | saved 57.3 Hz (stock 95) | 28.5 |
| ESP32-2432S032C | ST7789 | 40 MHz | saved 56.7 Hz (stock 60) | 28.3 |
| ESP32-2432S028 | ILI9341 | 40 MHz | saved 56.9 Hz (stock 95) | 28.3 |
| ES3C28P (ESP32-S3) | ILI9341 | 40 MHz | saved 58.4 Hz (stock 82) | 29 |

At 40 MHz a whole 320-row frame takes 31 ms to send, so the best is one frame every
two refreshes: about 29 fps at 58 Hz. Exactly 30 fps needs a 60 Hz refresh that is
still tear-free, which these boards don't have.

## 3. The budget that matters is per chunk, not per frame

The most expensive misunderstanding: **a longer frame period does not buy drawing
time.** Every frame still goes out in one pass behind the scan, taking the same
~31 ms whether a frame lasts two refreshes or ten. Each chunk must be drawn by the
time its turn comes in that pass. The extra refreshes are only idle time before the
next frame, and the render task can use them only to fill bounce buffers ahead. With
6 spare buffers that is 6 of the frame's 20–30 chunks.

So the budget is roughly **the send time plus the lookahead**: about 34 ms of drawing
per core per frame on these boards, whatever the frame rate. Plan the drawing to fit
that, and treat frame rate as fixed by the panel.

What happens when a chunk is late:

- **Late frame.** The raster task gives up at the chunk's deadline. The rows below
  keep the previous frame's pixels, which shows as a one-frame tear. Count these, and
  slow down or cut work if they recur.
- **Slipped start.** The frame finished too close to the next start and waits one
  more refresh. That is a stutter, not a tear.

Two rules for the counts:

- **Ignore lates while the screen is dark** (a fade between scenes, where a new
  scene's setup makes a frame or two late). Otherwise an adaptive frame rate steps
  down just as the next scene appears.
- **Don't trust per-frame averages alone.** One heavy moment, like a lightning flash
  or a scene with more on screen, is what tears.

## 4. Landscape without tearing

The panel always scans its native portrait rows, top to bottom. If you rotate the
panel to landscape (MADCTL) and send landscape rows, every row you send spans all the
native rows, and beam chasing can't work. Instead, **keep the panel in rotation 0 and
draw sideways**: native row *r* is landscape column *x*, and a 16-row chunk is a strip
of 16 landscape columns.

```cpp
// Normal (the picture's top along the portrait right edge, the library's rotation 1):
//   panel row r = column x = r, pixel c of that row = picture row y = H - 1 - c
// Flipped (rotation 3):
//   panel row r = column x = W - 1 - r, pixel c = picture row y = c
uint16_t *col = flip ? dst + (CHUNK - 1 - xi) * H : dst + xi * H + (H - 1);
const int dir = flip ? 1 : -1;            // pixel y of this column is col[y * dir]
```

Store anything you read per column **column-major** (index `x * height + y`), so a
column is one contiguous run in memory.

## 5. Using both cores

One core can't draw a full screen in time. What worked:

- **Two render workers**, one on each core. The raster task runs on the core that
  owns the SPI interrupt (`display.spiCore()`). Tasks that touch the bus must stay on
  that core. The second worker shares it at one priority below the raster task.
- **Each worker takes the next undrawn chunk** (a shared counter, taken *after* the
  worker has a free buffer), rather than fixed even/odd chunks. Expensive chunks, like
  the middle of a fire, then don't leave one core idle while the other runs late. The
  raster task must accept chunks out of order: keep early arrivals in a small table
  until their turn.
- **Split the per-frame step across the workers too.** The physics and table-building
  run before any chunk can be drawn, so they come straight out of the budget. In
  `RasterFireRainSnow` one worker runs the fire simulation and the ground tables
  while the other composites the smoke and moves the stars, then they meet at a
  barrier.
- **State a worker changes while drawing must be per worker** (`__thread` variables,
  or arrays indexed by worker).
- **On a single-core chip** (the ESP32-C5) put both workers on the one core; the code
  runs, just slower.
- **A task that never blocks starves the idle task**, and the task watchdog then
  resets the board after 5 s ("IDLE0 did not reset the watchdog"). A render loop that
  walks a corrupted list, for example, looks exactly like this.

## 6. Memory on the classic ESP32

The classic ESP32 has about 320 KB of data RAM, and full-screen animation uses most
of it:

- **Static RAM has its own ceiling.** Past about 120 KB of globals the link fails with
  "`dram0_0_seg` overflowed". Put the per-scene state of things that never run at the
  same time in a `union`: `RasterFireRainSnow`'s fire, storm and snow simulations
  share one block.
- **Count only usable heap.** `heap_caps_get_free_size(MALLOC_CAP_8BIT)` includes, on
  the classic ESP32, IRAM that `malloc()` can't hand out, and on the S3, all of its
  PSRAM. Use `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` when deciding how much to take.
- **Allocate in this order:** your tables first, then create the tasks (park them on a
  semaphore until the raster task starts), then bounce buffers with what's left,
  keeping ~12 KB free. Allocating buffers first left no room for task stacks.
- **Bounce buffers are lookahead.** Six of them (60 KB on a 320-wide panel) was enough
  once the work was balanced.
- **On the S3, keep hot tables in internal RAM.** `malloc()` may put a large block in
  PSRAM, which is slower to read; use `heap_caps_malloc(n, MALLOC_CAP_INTERNAL |
  MALLOC_CAP_8BIT)`.

## 7. Flash is slow when both cores want it

Big images live in flash (`static const` arrays) and are read through a 32 KB cache.
With both cores drawing, cache misses queue behind each other. Measured on a 4"
board, streaming a 480 × 171 backdrop from flash cost about **11 ms of CPU a frame**,
and a mountain layer scaled from a 1.8× source about 40 ms — most of the budget.

- **Read flash in order.** Column-major assets read sequentially; any scattered read
  is a miss.
- **Copy what a scene reads every frame into RAM** when it fits, packed if need be.
  The snow clouds were copied in as 4-bit luminance + 4-bit alpha (25 KB) at scene
  start.
- **Draw it instead of storing it** where you can. A sky gradient is a table of row
  colours. Stars are a list of points, which also lets them move. Both cost almost
  nothing and read no flash.
- **Put hot code in IRAM** (`IRAM_ATTR`). Code fetched through the same cache misses
  too.
- **Small `const` tables used per pixel belong in RAM.** A dither matrix declared
  `const` lives in flash, and can be evicted while the other core streams an image.
- **Prefer magnifying a 1.0× image to shrinking a larger one.** Scaling down reads
  more source per pixel.

## 8. Cheap pixels

Rough cost on these chips: a few cycles a pixel for a table lookup and store, 15–25
for a blend, 100+ once flash misses or floating point creep in. A 480 × 320 frame
is 153,600 pixels, and two cores have about 16 million cycles for it.

- **Draw straight into the bounce buffer, already byte-swapped.** The panel wants
  big-endian RGB565. Drawing into a tile and copying it out cost a full pass.
- **Build tables once a frame, not per pixel.** A ground plane's colour depends only on
  the row and on how much of a grid line covers the pixel. Work out the coverage once
  (it never changes), then build a per-row colour table each frame (the lines scroll,
  the light flickers). The pixel is then a lookup.
- **The spread-RGB565 trick.** `(c | c << 16) & 0x07E0F81F` gives each field headroom,
  so one multiply blends all three channels. Add `0x02008010` before the `>> 5`: the
  truncating version leans half a step toward the background. Green's steps are half
  the size of red's and blue's, so faint overlays came out **green-tinted**.
- **Saturating add in the same form:** after adding two spread values, smear each
  field's carry bit down over the field.
- **Per-channel colour changes as two byte tables.** Scaling and offsetting each
  channel is linear, so `out = T_hi[c >> 8] + T_lo[c & 255]` is exact. Green's two
  halves add up within its field without carrying. Rebuild the 512 entries each frame
  for a sunset tint, a lightning flash, or both at once.
- **Ordered dithering hides 565 banding**, and it can also choose between two
  precomputed tables per pixel. The firelight's smooth falloff is 4 brightness tables
  dithered 4 × 4 by column, rather than a per-pixel multiply.
- **Bilinear scaling, cached.** When magnifying, consecutive picture rows mostly reuse
  the same two source rows, so interpolate across once per source row, then down per
  pixel.
- **Half-resolution layers** for soft things: clouds, smoke density on a 4-pixel
  grid.
- **No `sinf`, `sqrtf` or division per pixel.** Precompute a circle or ring as a table,
  and use squared distance with a linear edge for soft dots.

## 9. Measuring

- **Count cycles per stage on the board** (`esp_cpu_get_cycle_count()`, per worker),
  and print CPU milliseconds a frame for each stage every second, beside frames, late
  frames and slipped starts. Every optimization that paid off was found this way.
  Several guesses about where the time went were wrong.
- **Build the drawing code on a computer too.** Keep it free of Arduino and FreeRTOS
  calls, and a small harness can write frames as images to check the look in seconds
  (`RasterFireRainSnow/tools/host_preview.cpp`). Host timings mean nothing for the
  board.
- **Check that the build you're timing is the one that was flashed.** A failed build
  can leave the old binary to be uploaded, and the numbers stay the same no matter
  what you change.

## 10. Pitfalls that cost time

- **`sinf()` of an ever-growing clock gets slower.** Sway, wind and ripples took
  `sinf(time × k)`. After a few minutes the argument was large enough to make the
  frame step 2–3 ms slower, enough to stutter. Restart the clock at each scene change.
- **Scan readings stretch under load.** With both cores pulling from flash, a `0x45`
  reading took up to ~300 µs. A rule that rejects readings over 200 µs then rejects
  nearly all of them, and the scan model drifts. Accept up to ~350 µs: the reading is
  timed at its middle, and the model corrects only part of the way.
- **Serial must never block the raster task.** On a UART board, call
  `Serial.setTxBufferSize()` before `begin()`. On the S3's own USB, call
  `Serial.setTxTimeoutMs(0)`.
- **Partition scheme.** Pictures for two panel sizes took about 1.5 MB, too big for
  the default 1.2 MB app slot. Use "Huge APP" on the classic boards.
- **Optimize the hot code.** The Arduino core compiles at `-Os`; put
  `#pragma GCC optimize ("O2")` at the top of the file that draws.
- **The RISC-V toolchain (ESP32-C5) defines a macro `HZ`.** A variable of that name
  won't compile there.
- **On a computer, clang's vectorizer miscompiled one small table-building loop.** The
  device compiler was unaffected. If the host preview looks wrong in a regular
  pattern, try building it with `-fno-vectorize`.

## 11. What more hardware buys

The classic CYD is about as constrained as a board with a colour screen gets: a
240 MHz dual-core ESP32, about 320 KB of RAM, no PSRAM, 4 MB of flash, and a screen
on an SPI link of 40 or 80 MHz. Here is what each kind of upgrade would open up, for
someone choosing a board for a piece:

| More of… | What it opens up |
|---|---|
| **RAM** (internal) | More bounce buffers, so expensive parts of a frame can be drawn well ahead, and more of the scene's images held in RAM instead of read from flash. A whole frame buffer needs 150–300 KB, beyond any CYD's internal RAM. |
| **PSRAM** (external RAM, megabytes) | Room for whole frame buffers (draw anything in any order, keep the last frame and only redraw what changed) and for large images and animations decoded into RAM. It is slower than internal RAM and shares a bus with flash, so it suits storing pictures better than heavy per-pixel work. |
| **Flash** (8–32 MB) | More and larger stored pictures: more scenes, more layers, short image sequences. It doesn't make anything faster. |
| **Bandwidth to the screen** (faster SPI, an 8- or 16-bit parallel bus, an RGB or MIPI panel) | The frame goes out in a few milliseconds instead of ~31, so the full refresh rate (60 fps) is reachable. The drawing budget per frame grows, and the scan-chasing constraints mostly disappear. RGB and MIPI panels refresh from a frame buffer in RAM, with no tearing to manage. A panel with a "TE" (tearing effect) pin gives the timing with no scan-line reading. |
| **PIE** (the ESP32-S3's and P4's SIMD vector instructions) | One instruction mixes or adds 8 to 16 pixels at once. Blends, fades, see-through layers and dithering get several times cheaper, if the code uses them (hand-written assembly or Espressif's `esp-dsp` / `esp-dl` routines; the compiler won't do it for you). |
| **PPA** (the ESP32-P4's 2D engine) | Hardware scaling, rotation, blending and colour conversion of whole images while the CPU does something else. Scaling or rotating a big picture, the most expensive item for the artist above, becomes nearly free. |
| **A faster CPU** (the P4's 400 MHz cores) | Roughly double the drawing per frame, before PIE and PPA are counted. |

**Within this family**, the **ES3C28P** (the black ESP32-S3 board) already has 8 MB
of PSRAM, 16 MB of flash and PIE, so it has the most headroom of the CYDs. In
`RasterFireRainSnow` it drew a frame in about 12 ms, against 33 ms on a classic
board. Its screen is on the same SPI link, though, so the frame rate stays at
about 29. The NM-CYD-C5 has PSRAM but a single core.
