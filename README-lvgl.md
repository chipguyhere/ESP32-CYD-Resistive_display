# Building with LVGL

This is one of the two ways to drive the screen on these boards: **LVGL 9**, a popular
library for buttons, sliders, labels, lists and whole screens that you design. If you
haven't chosen yet, the [README](README.md#two-ways-to-drive-the-screen--pick-one)
compares it with the other way, the [text terminal](README-terminal.md).

Choose LVGL when you want a graphical app: widgets, touch gestures, pictures, several
screens, or the board **turned sideways** (landscape). LVGL draws everything in
software, so it works in every orientation.

## You also need: LVGL 9

The examples use **LVGL 9**. Install it from the Arduino **Library Manager** (search for
"lvgl"). Each example folder has its own `lv_conf.h` file — that's LVGL's settings file,
and it needs to stay next to the `.ino`.

This library was written against LVGL 9.3 (the version SquareLine Studio supports) and
very likely works with later versions.

## The LVGL examples — pick your starting point

Open them from **File ▸ Examples ▸ chipguy_ESP32-CYD-Resistive_display**. Each is a
complete, working **starting point** — the screen, touch and LVGL are already set up
and running before your code gets control. Pick the one that matches how you want to
build your UI, copy it, and start replacing its placeholder screen with your own.

To turn an example into your own project, **save it under a new name** (File ▸ Save
As) so your work lives outside the library folder. Every example folder is
self-contained — it carries its own `lv_setup.hpp` and `lv_conf.h` — so the copy is
fully standalone.

### LvglClaudeCodeStub — write the UI yourself (by hand or with an AI)

Use this when you want to build the interface **in code**: yourself, or by handing the
sketch to an AI coding assistant such as Claude Code.

The UI lives in a small `ui.h` / `ui.cpp` pair in the sketch folder. All the hardware
and LVGL setup happens before `ui_init()` is called, so `ui_init()` is a blank canvas:
it gets a live LVGL screen and you create widgets on it. To start your app:

1. Open `ui.cpp` and look at `ui_init()`. The demo it ships with (a label that follows
   your finger, and a color-cycling rectangle) is just there to prove the screen and
   touch work — **delete it.**
2. In its place, build your own UI on `lv_screen_active()` using normal LVGL calls
   (`lv_label_create`, `lv_button_create`, event callbacks, …).
3. As your app grows, add more screens and helper functions, and declare the ones the
   sketch needs in `ui.h`.

Because everything except the UI is already wired up, this is the one to hand to
**Claude Code**: point it at the sketch folder and describe the app you want. It can
focus entirely on `ui.cpp` and the screens you add, without touching the display,
touch or LVGL plumbing.

**Reach for this one** when you want full control, a small program, or AI-assisted or
hand-written UI code.

### Lvgl93SquarelineLauncher — design the UI visually in SquareLine Studio

Use this when you'd rather **design the interface by dragging and dropping** in
[SquareLine Studio](https://squareline.io/) (a visual editor for LVGL) and just run the
result on the board.

The sketch's `src/` folder holds a small **placeholder** UI in the exact file shape a
SquareLine export produces (`ui.c`, `ui_Screen1.c`, …). The sketch calls the `ui_init()`
that SquareLine generates. To start your app:

1. In SquareLine Studio, create a project sized to your screen and orientation
   (**240×320** portrait or 320×240 landscape on the 2.8" and 3.2" boards; 320×480 or
   480×320 on the 3.5" and 4.0"), **16-bit** color, with **no rotation or offset** —
   this library handles rotation, so keep the project unrotated.
2. Lay out your screens visually.
3. **Export ▸ UI files** (the "Arduino TFT_eSPI profile" works, among others).
4. **Replace the whole `src/` folder** with your export. Build and upload —
   `ui_init()` now runs your design.

Treat `src/` as disposable: you overwrite it each time you re-export from SquareLine,
so don't keep anything of your own there. Your app's logic lives in the **sketch** —
after calling `ui_init()`, attach event handlers from the `.ino` to the objects
SquareLine exposes (e.g. `ui_Button1`). That way re-exporting the visuals never
touches your code.

**Reach for this one** when you want to iterate on layout visually and write little or
no C for the UI itself.

### LvglBouncingBalls and LvglCalculator — two finished demos

Not starting points, but worth a look: a **bouncing-balls** toy with sliders for count
and speed, and a working **calculator** styled after the familiar phone one. Both lay
themselves out from the screen size at run time, so the same `ui.cpp` works in any
orientation and even on a different-sized board. Good examples of how an LVGL app is
put together.

### MP3Player — sound, as a worked example

The reference for **using the speaker**: it plays `*.mp3` files from the SD card
through the board's amplifier, with a track list, transport buttons and a click-wheel
view. It needs the **ESP8266Audio** library (Library Manager) for MP3 decoding.
The audio code is written up in [README-sound.md](README-sound.md); if your sketch
needs sound, copy `AudioOutputCydDac.h` from this example.

---

## What the examples give you: the `lv_setup` API

Every example connects the hardware to LVGL through one small header,
**`lv_setup.hpp`** (plus **`lv_conf.h`** for LVGL's settings). That header and the two
global objects it defines are the whole surface you deal with — once `begin()` returns,
you are just using plain LVGL.

```cpp
#include "lv_conf.h"
#include "lvgl.h"
#include "lv_setup.hpp"

void setup() {
    Serial.begin(115200);

    lv_setup.begin();           // start display + touch + LVGL (default: portrait)

    // ...build your UI on lv_screen_active() here (or call ui_init())...
}

void loop() {
    lv_timer_handler();         // let LVGL run
    delay(5);
}
```

What the header provides:

- **`lv_setup.begin(rotation = 0)`** — call once in `setup()`. Starts the screen, the
  touch panel and LVGL, and connects them. After it returns, the active LVGL screen
  exists and you build your UI with ordinary LVGL calls. The examples call it as
  `lv_setup.begin()` and take the default (portrait).
  An optional second number caps how much memory LVGL's drawing buffers take, in
  bytes each — `lv_setup.begin(1, 12800)` — for a sketch that runs short of memory.
  Smaller buffers just mean LVGL redraws the screen in more, thinner pieces.
- **`display`** — a global display object. Handy methods: `display.width()` and
  `display.height()` (for the current rotation), and `display.setBacklight(0..100)`
  to dim or brighten the screen.
- **`lv_setup.touch()`** — the touch driver, for calibration. You won't normally call
  it; `lv_setup` already feeds touch into LVGL.

You don't draw or refresh anything yourself — LVGL does, through the callbacks
`lv_setup` installed. Your job is to create LVGL widgets and call `lv_timer_handler()`
in `loop()`.

### Portable across chipguy board libraries

Because every hardware detail lives in just two files — `lv_setup.hpp` and
`lv_conf.h` — your app code (`ui.cpp`, or the SquareLine `src/` files) doesn't know
which board it's on. It only ever talks to LVGL.

So to move an app to a **different display board**, drop in the `lv_setup.hpp` and
`lv_conf.h` from that board's chipguy library, and the rest of your sketch is
unchanged. (Different boards have different screen sizes, so layouts built with LVGL's
alignment travel best; fixed pixel positions may need adjusting.)

### Rotation

Pass `0`–`3` (or the matching degrees) to `lv_setup.begin()`. Display and touch rotate
together.

| rotation | degrees | orientation | 2.4" / 2.8" / 3.2" | 3.5" / 4.0" |
|---|---|---|---|---|
| 0 | 0 | portrait (native) | 240×320 | 320×480 |
| 1 | 90 | landscape | 320×240 | 480×320 |
| 2 | 180 | portrait, upside down | 240×320 | 320×480 |
| 3 | 270 | landscape, upside down | 320×240 | 480×320 |

`90` / `180` / `270` work as aliases for `1` / `2` / `3` (so `lv_setup.begin(90)` is
landscape). For the SquareLine example, size your SquareLine project to match.

## Touch calibration

The LVGL examples load the saved touch calibration for you, in
`lv_setup.begin()`. To make one, see [Touch calibration](README.md#touch-calibration--filtering)
in the README.

---

## Where to go next

Back to the [README](README.md) for the rest of the board — LED, buttons, power,
sound, SD card — and the full list of guides.
