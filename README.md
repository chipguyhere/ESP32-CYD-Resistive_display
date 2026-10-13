# chipguy_ESP32-CYD-Resistive_display

A ready-made **display + touch** driver for the **resistive-touch "Cheap Yellow
Display" boards** — the family of ESP32 boards with a touch screen on the front, sold
by many vendors. One library runs all of these:

| Board | Screen | Look for |
|---|---|---|
| **ESP32-2432S028** | 2.8", 240×320 | the original CYD; "ESP32-2432S028" on the back; micro-USB |
| **ESP32-32E 2.8"** | 2.8", 240×320 | *ESP32-32E … Resistance Touch* on the back; USB-C; a tiny 2-pin battery connector |
| **ESP32-32E 3.2"** | 3.2", 240×320 | *3.2" … ESP32-32E 240x320, Resistance Touch* on the back — the same wording as the 2.8", only the size differs |
| **ESP32-32E 3.5" / 4.0"** | 320×480 | *ESP32-32E 320x480, Resistance Touch* on the back |

You don't tell the library which one you have: it checks when it starts and sets up
the screen and touch to match. A sketch that cares — say, to blink the red LED, which
is on a different pin on the 2432S028 — can ask it.
[How to ask](README-board-detection.md).

The library handles the screen and the touch panel for you, so you don't have to
think about either. What's left for you is the fun part: writing your app with
**LVGL 9**, a popular library for drawing buttons, sliders, labels and screens.

The examples are the heart of the library. Each is a complete, working **starting
point** — the screen, touch and LVGL are already set up and running before your code
gets control. Pick the one that matches how you want to build your UI, copy it, and
start replacing its placeholder screen with your own.

> **Is this your board?** It has to have **resistive** touch — the kind you press
> with a fingernail or stylus, which feels slightly soft. Boards with **capacitive**
> touch (glass that responds to a light finger, like a phone; the "C" in
> ESP32-2432S032**C**) are not supported, even though they look the same.

Across these boards there are four different screen setups, two of them behind the
same 2.8" label. You don't need to know which one yours has.
[Details](README-board.md#screen-variants).

---

## Installation

This library is installed by downloading it as a ZIP from GitHub — it isn't in the
Arduino Library Manager. To install:

1. On the GitHub project page, click **Code ▸ Download ZIP**.
2. Unzip it, and move the resulting folder into your Arduino **libraries** folder:
   - macOS / Linux: `~/Documents/Arduino/libraries/`
   - Windows: `Documents\Arduino\libraries\`
3. Restart Arduino IDE so the examples show up in the File menu.

(Or, in the IDE: **Sketch ▸ Include Library ▸ Add .ZIP Library…** and pick the ZIP.)

### You also need: LVGL 9

The examples use **LVGL 9**. Install it from the Arduino **Library Manager** (search for
"lvgl"). Each example folder has its own `lv_conf.h` file — that's LVGL's settings file,
and it needs to stay next to the `.ino`.

This library was written against LVGL 9.3 (the version SquareLine Studio supports) and
very likely works with later versions.

### Arduino IDE board settings

In **Tools ▸ Board ▸ esp32**, choose **"ESP32 Dev Module"**. The default settings work.
Two of them matter, and the library checks them when you build: **Flash Size** must be
**4MB** and **PSRAM** must be **Disabled** — that's what every one of these boards
actually has. If a
build stops with an error mentioning flash size or PSRAM, the IDE has quietly changed
one of those settings. Set it back and build again.

### Plugging it in

For a USB-C board, use a plain **USB-A to USB-C** cable, or a basic C-to-C cable. The
fancy cables that come with laptops and fast chargers often **won't power the ESP32-32E
2.8" at all** — it's a quirk of the board, not a fault. If nothing lights up, try a
cheaper cable.

---

## The examples — pick your starting point

Open them from **File ▸ Examples ▸ chipguy_ESP32-CYD-Resistive_display**. All of them
run on the board as soon as you upload them, so you can confirm your board works
before changing a single line.

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

### TouchCalibrate — a one-time setup tool

Not a template — a tool you upload once to calibrate the touch panel and tune its
noise filters. It saves the result on the board so your real sketch picks it up
automatically. See [Touch calibration](#touch-calibration--filtering) below.

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

| rotation | degrees | orientation | 2.8" / 3.2" | 3.5" / 4.0" |
|---|---|---|---|---|
| 0 | 0 | portrait (native) | 240×320 | 320×480 |
| 1 | 90 | landscape | 320×240 | 480×320 |
| 2 | 180 | portrait, upside down | 240×320 | 320×480 |
| 3 | 270 | landscape, upside down | 320×240 | 480×320 |

`90` / `180` / `270` work as aliases for `1` / `2` / `3` (so `lv_setup.begin(90)` is
landscape). For the SquareLine example, size your SquareLine project to match.

### Touch calibration & filtering

You don't calibrate touch in code. Upload the **`TouchCalibrate`** example once, run
it, then upload your real sketch over the top. It walks through three steps on screen:

1. **Calibration** — tap a series of crosshairs with a stylus.
2. **Filter settings** — two sliders set the touch noise filters: a **minimum
   pressure** (ignore feather-light or phantom touches) and a **jump filter** (drop
   glitchy readings that teleport across the screen), with a live readout to help you
   pick values. A **Save** button stores them.
3. **Drawing test** — scribble to confirm it tracks cleanly.

Everything it produces is saved in a small area of the ESP32's flash that survives
reboots *and* uploading a new sketch. Your app picks it up **automatically** when
`lv_setup.begin()` runs. Do it once per board. (A board already calibrated with an
earlier version of this library, or a 3.5" / 4.0" calibrated with
`chipguy_ESP32-32E_CYD_4inch_display`, keeps that calibration.)

---

## What else is on the board

Besides the screen, these boards have an **RGB LED**, a **BOOT button** you can use as
a regular button, a few **small connectors**, a **speaker connector** and a **microSD
slot**; the ESP32-32E 2.8" also has a **battery connector with a charger**. This
library leaves all of those alone so your sketch can use them. Want to hook up a
**sensor**? Most small sensor boards use I²C, and the 32E 2.8" can talk I²C through its
SPI connector as long as you're not using the SD card at the same time. The pins,
what's already in use, and how to add your own hardware are in
**[README-board.md](README-board.md)**.

The pages about the rest of the board — connectors, power, sound and the SD card —
describe the **ESP32-32E 2.8"** in full. What's known about the other boards, and how
they differ, is in [README-board.md](README-board.md#other-boards); much of it is still
to be checked.

### TF / microSD card slot

Every board has a microSD slot. The library doesn't touch it and doesn't include a file
system, but **nothing about it conflicts with the screen** — the card has its own pins,
and they're the ones Arduino's standard `SD` library expects, so `SD.begin(5)` just
works alongside this library. Details in **[README-sdcard.md](README-sdcard.md)**.

### Power and battery

On the **ESP32-32E 2.8"**, plug in a single lithium cell and the board **charges it whenever USB is connected**
and **runs from it when USB is unplugged** — nothing to set up. It is **required** that you
use a lithium battery that includes a chip to automatically turn off the battery when
it reaches 0%, otherwise this board will overdischarge the battery and ruin it.
There is **no power switch** and no charge light, and the board can't turn itself off —
the best it can do is sleep, slowing the battery discharge. How to read the battery
voltage, what the reading means, how to sleep and wake, and how to make a charge
last are in **[README-power.md](README-power.md)**. The 2432S028 has no battery
connector; the other boards' power hasn't been looked at yet.

### Sound

Every board has a small amplifier and a 2-pin speaker connector — good for beeps,
voice and simple music (it's not hi-fi). On the ESP32-32E boards the amplifier is
**off until your sketch turns it on**; on the 2432S028 it is always on.
Not every audio library plays nicely with this board's touch screen, so start from the
**MP3Player** example, which shows a working setup, rather than a generic ESP32 audio
tutorial. The recipe, the traps and a copy-and-paste beep are in
**[README-sound.md](README-sound.md)**.

---

## Where to go next

| File | Who it's for |
|---|---|
| [README-board.md](README-board.md) | You want to use the LED, the button, the connectors, or add your own hardware, and need to know which pins are free. |
| [README-power.md](README-power.md) | You want to run on battery, read the battery level, or make the board sleep and wake. |
| [README-sound.md](README-sound.md) | You want the speaker to make a noise — a beep, a sample, an MP3. |
| [README-sdcard.md](README-sdcard.md) | You want to read or write files on a microSD card. |
| [README-board-detection.md](README-board-detection.md) | Your sketch runs on more than one of these boards and needs to know which one it's on — for the red LED's pin, or whether GPIO4 is the speaker switch. |
| [README-engineering.md](README-engineering.md) | You want the electronics: the schematic, the parts, the measurements and the reasoning behind everything above. |

---

## License

MIT — see [LICENSE](LICENSE).

The ILI9341 initialization/gamma values (the ST7789 needs none beyond its defaults) are standard panel-datasheet register settings
(as also used by Adafruit_ILI9341, BSD-3); the ST7796 values are the ones TFT_eSPI uses (MIT). `lv_conf.h` is based on LVGL's template (LVGL
is MIT-licensed).
