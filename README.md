# chipguy_ESP32-CYD-Resistive_display

A ready-made **display + touch** driver for the **resistive-touch "Cheap Yellow
Display" boards** — the family of ESP32 boards with a touch screen on the front, sold
by many vendors. One library runs all of these:

| Board | Screen | Look for |
|---|---|---|
| **ESP32-2432S028** | 2.8", 240×320 | the original CYD; "ESP32-2432S028" on the back; micro-USB |
| **ESP32-32E 2.8"** | 2.8", 240×320 | *2.8" LCD Display / ESP32-32E 240x320 / Resistance Touch* on the back; USB-C; a tiny 2-pin battery connector |
| **ESP32-32E 3.2"** | 3.2", 240×320 | *3.2" LCD Display / ESP32-32E 240x320 / Resistance Touch* on the back |
| **ESP32-32E 3.5"** | 3.5", 320×480 | *3.5" LCD Display / ESP32-32E 320x480 / Resistance Touch* on the back |
| **ESP32-32E 4.0"** | 4.0", 320×480 | *4.0" LCD Display / ESP32-32E 320x480 / Resistance Touch* on the back |
| **ESP32-024** | 2.4", 240×320 | "ESP32-024" on the back; a battery connector; three buttons (RST, BOOT, SW1) |
| **ESP32-035** | 3.5", 320×480 | "ESP32-035" on the back; a light sensor; three buttons (RST, BOOT, SW1) |

On the ESP32-32E boards that writing is three lines on the back of the board: the
size, then the board name and resolution, then *Resistance Touch*.

You don't tell the library which one you have: it checks when it starts and sets up
the screen and touch to match. A sketch that cares — say, to light the RGB LED, whose
pins differ between boards — can ask it, and can ask which
board it is.
[How to ask](README-board-detection.md).

The library handles the screen and the touch panel for you, so you don't have to
think about either. What's left for you is the fun part: writing your app — and
first, choosing which of [two ways to drive the screen](#two-ways-to-drive-the-screen--pick-one)
suits it.

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

## Two ways to drive the screen — pick one

This library supports two quite different ways of putting things on the screen. A
project uses **one or the other**, so it's worth choosing first.

| | **LVGL** — a graphical app | **Text terminal** — like a console |
|---|---|---|
| What you write | Buttons, sliders, labels, screens | `print()` and `printf()` |
| Looks like | A phone app | A computer's text console, in color |
| Orientation | Any — portrait or landscape | Portrait only (either way up) |
| Scrolling | Any direction, but every frame is redrawn | Up and down only, and extremely fast |
| Extra library | LVGL 9, from the Library Manager | None |
| Memory it takes | Tens of KB of RAM for drawing, more as screens grow | About 5 KB of RAM, fixed — it never grows |
| Good for | Control panels, dashboards, sensor displays, games | A stream of text: logs, event feeds, a console |
| Guide | **[README-lvgl.md](README-lvgl.md)** | **[README-terminal.md](README-terminal.md)** |

**LVGL** is the choice for a real graphical interface. You lay out widgets, by hand,
with an AI assistant, or visually in SquareLine Studio, and LVGL draws them. A screen
that *shows* things — a temperature, a set of readings that update in place, buttons
and settings — is an LVGL app.

**The text terminal** is the choice when your project *streams* text: a log of what's
happening, a feed of events as they arrive, the kind of output you'd otherwise watch
on the serial monitor. New lines come in at the bottom and the rest scrolls up, very
fast, because the screen's own hardware does the scrolling. It has colors, symbols, a
blinking cursor, touch, and pictures for a header or footer — but it only works with
the screen upright (portrait). For a sideways (landscape) screen, use LVGL.

**Its memory use is small and never changes.** The terminal takes about 5 KB of RAM,
all set aside when it starts, and asks for no more however much you print. These
boards have no extra memory chip (no PSRAM), so that matters when the same sketch
also runs Wi-Fi, a web server, MQTT or anything else that needs memory: the screen
can't be the thing that runs the board out of it, and how much is left stays
predictable.

### Scrolling: the biggest difference

Both can scroll, but in very different ways.

**LVGL** scrolls anything — a list, a page, a picture — in any direction, with the
screen any way round. To do it, it redraws everything that moved and sends it to the
screen again. With these boards' small screens that is still quick. But imitating a
console in LVGL — a label or list that keeps growing, old lines removed as new ones
arrive — means creating and freeing objects all the time, and redrawing whatever the
text sits on top of; that costs memory and time the terminal doesn't spend.

**The text terminal** doesn't redraw at all when it scrolls. The screen's own
hardware slides the picture up, and the board only draws the one new line coming in
at the bottom. That makes it extremely fast — over 100 lines a second on the 2.4",
2.8" and 3.2" boards, about 90 on the 3.5" and 4.0" — but the hardware can only slide
the picture along the screen's long side. So the terminal scrolls up and down, with
the board upright, and nothing else.

Each guide lists its own examples. They all open from **File ▸ Examples ▸
chipguy_ESP32-CYD-Resistive_display**: the `Lvgl…` ones are LVGL, the `Terminal…`
ones are the text terminal. All of them run as soon as you upload them, so you can
check your board works before changing a line.

### Two tools that work either way

**BoardDetect — which board is this?** Upload it to see what the library found: the
board's name, its screen, which pin the RGB LED is on, whether touch has been
calibrated, the SD card, and whether the board has battery support, a light sensor and
a switch for its speaker amplifier. The bottom line shows the light sensor or the
battery live. Then draw on the screen with a finger to check touch. The calls it uses
are explained in [README-board-detection.md](README-board-detection.md). (It is built
with LVGL, so it needs LVGL installed.)

**TouchCalibrate — a one-time setup tool.** Not a template — a tool you upload once to
calibrate the touch panel and tune its noise filters. It saves the result on the board
so your real sketch picks it up automatically. See
[Touch calibration](#touch-calibration--filtering) below. (Also built with LVGL.)

---

## Touch calibration & filtering

You don't calibrate touch in code. Upload the **`TouchCalibrate`** example once, run
it, then upload your real sketch over the top. It walks through three steps on screen:

1. **Calibration** — tap a series of crosshairs with a stylus.
2. **Filter settings** — two sliders set the touch noise filters: a **minimum
   pressure** (ignore feather-light or phantom touches) and a **jump filter** (drop
   glitchy readings that teleport across the screen), with a live readout to help you
   pick values. A **Save** button stores them.
3. **Drawing test** — scribble to confirm it tracks cleanly.

Everything it produces is saved in a small area of the ESP32's flash that survives
reboots *and* uploading a new sketch. An LVGL app picks it up **automatically** when
`lv_setup.begin()` runs; a text-terminal sketch loads it with two lines, shown in
[README-terminal.md](README-terminal.md#touch). Do it once per board. (A board already calibrated with an
earlier version of this library, or a 3.5" / 4.0" calibrated with
`chipguy_ESP32-32E_CYD_4inch_display`, keeps that calibration. An ESP32-024 calibrated
with the older `chipguy_cyd2432S024_display` needs calibrating again.)

---

## What else is on the board

Besides the screen, these boards have an **RGB LED**, a **BOOT button** you can use as
a regular button, a few **small connectors**, a **speaker connector** and a **microSD
slot**; the ESP32-32E 2.8" and the ESP32-024 also have a **battery connector with a
charger**. This
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

Every board has a microSD slot, and **it's wired the same way on all six boards**, so
the same code reads the card on every one. The library doesn't touch it and doesn't
include a file system, but **nothing about it conflicts with the screen** — the card
has its own pins, and they're the ones Arduino's standard `SD` library expects, so
`SD.begin(5)` just works alongside this library. The **MP3Player** example reads its
songs from the card this way and has been tried on every board — copy its card code
if you need one that's known to work. Details in **[README-sdcard.md](README-sdcard.md)**.

### Power and battery

On the **ESP32-32E** boards, plug in a single lithium cell and the board **charges it whenever USB is connected**
and **runs from it when USB is unplugged** — nothing to set up. It is **required** that you
use a lithium battery that includes a chip to automatically turn off the battery when
it reaches 0%, otherwise this board will overdischarge the battery and ruin it.
There is **no power switch** and no charge light, and the board can't turn itself off —
the best it can do is sleep, slowing the battery discharge. How to read the battery
voltage, what the reading means, how to sleep and wake, and how to make a charge
last are in **[README-power.md](README-power.md)** (written for the 32E 2.8"; the other
32E boards read their battery the same way). The 2432S028 has no battery connector.
The **ESP32-024** has one, with a different charger: on battery alone it stays off
until you press **SW1** once, and two quick presses turn it off. Your sketch can't see
SW1 or read the battery on that board.

### Sound

Every board has a small amplifier and a 2-pin speaker connector — good for beeps,
voice and simple music (it's not hi-fi). On the ESP32-32E boards the amplifier is
**off until your sketch turns it on**; on the 2432S028 and the ESP32-024 it is always
on.
Not every audio library plays nicely with this board's touch screen, so start from the
**MP3Player** example (an LVGL one), which shows a working setup, rather than a
generic ESP32 audio tutorial. The recipe, the traps and a copy-and-paste beep are in
**[README-sound.md](README-sound.md)**.

---

## Where to go next

| File | Who it's for |
|---|---|
| [README-lvgl.md](README-lvgl.md) | You're building a graphical app with LVGL: the examples, the `lv_setup` setup code, rotation. |
| [README-terminal.md](README-terminal.md) | You're using the text terminal: printing, colors, scrolling, touch, fonts, and the examples. |
| [README-board.md](README-board.md) | You want to use the LED, the button, the connectors, or add your own hardware, and need to know which pins are free. |
| [README-power.md](README-power.md) | You want to run on battery, read the battery level, or make the board sleep and wake. |
| [README-sound.md](README-sound.md) | You want the speaker to make a noise — a beep, a sample, an MP3. |
| [README-sdcard.md](README-sdcard.md) | You want to read or write files on a microSD card — same pins on every board. |
| [README-board-detection.md](README-board-detection.md) | Your sketch runs on more than one of these boards and needs to know which one it's on — for the LED's pins, or whether GPIO4 is the speaker switch. |
| [README-engineering.md](README-engineering.md) | You want the electronics: the schematic, the parts, the measurements and the reasoning behind everything above. |

---

## License

MIT — see [LICENSE](LICENSE).

The ILI9341 initialization/gamma values (the ST7789 needs none beyond its defaults) are standard panel-datasheet register settings
(as also used by Adafruit_ILI9341, BSD-3); the ST7796 values are the ones TFT_eSPI uses (MIT). `lv_conf.h` is based on LVGL's template (LVGL
is MIT-licensed).

The text terminal's fonts are drawn from freely redistributable fonts:
[Inconsolata](https://github.com/googlefonts/Inconsolata) and
[Noto Sans CJK](https://github.com/notofonts/noto-cjk) (SIL Open Font License 1.1),
[DejaVu Sans Mono](https://dejavu-fonts.github.io/) (Bitstream Vera license / public
domain), and [Font Awesome Free](https://fontawesome.com/license/free) 5.15.4 for the
LVGL-compatible symbols (icons CC BY 4.0, Fonticons, Inc.; the `LV_SYMBOL_*` names are
LVGL's). The licenses are in [extras/fontgen/](extras/fontgen/).
