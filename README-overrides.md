# Overrides — when detection gets a board wrong

`display.begin()` works out the board, the panel and how to drive them
([README-board-detection.md](README-board-detection.md)). Every board in the
[supported list](README-board.md#which-boards-these-are) is detected correctly. A new
variant might not be: a panel with its color order the other way, touch that runs
backwards, LED pins swapped. Overrides let you correct that **without changing the
library**. You set them once per board in the `TouchCalibrate` example. They are saved
in the ESP32's NVS (the flash area that survives reflashing), and every sketch using the
library on that board picks them up at `begin()`.

If a board needs an override, please also report it: it means detection has met a
variant it doesn't know.

## Setting them

1. Flash **`TouchCalibrate`** and get to its drawing test (calibrate, or press BOOT to
   skip when a calibration is already saved).
2. **Hold BOOT for 2 seconds** — or the on-screen **BOOT** button at the bottom, if an
   enclosure hides the real one. The **Settings** screen names the board, panel and
   touch chip as detected.
3. Choose **Options** or **Advanced**. **Options** holds the settings that can't break
   the screen or touch: screen size, the LED pins, GPIO34 and the power key.
   **Advanced** holds the rest — model, panel, colors, mirroring, SPI clock,
   backlight and touch — behind a warning screen.
4. Each list shows its settings as **`auto (what was detected)`** or a forced value.
   Tap one to open its own page, then tap a choice in the list. **Next** goes to the
   next setting; **Back** returns to the list.
5. Choices marked "Shown at once" take effect immediately, so you can see whether the
   change is right. The rest take effect after the restart.
6. Back on the Settings screen, **Save & restart** stores them. **Cancel** restarts
   without saving, and **Clear all** removes every override.

The band around the screen's edge is for navigation. **Next** is drawn lower right, but
every corner works. **Back** is drawn bottom center, but the top center and the middle of
each side work too. So the buttons still work when the touch axes are mirrored or
swapped. Every touch is drawn on these screens, so you can see where your presses
land. The screens also work with **BOOT alone**: a short press moves to the next item,
and a long press selects it.

After changing a display mirror or any touch setting, **calibrate again**:
`TouchCalibrate` does that on the restart.

## The settings

Each one is *auto* (use detection) or one of the values listed (two for most). "What you see when it's wrong"
is how to tell that a variant needs it.

| Setting | Values | What you see when it's wrong |
|---|---|---|
| Model | any supported board | `display.modelName()` names the wrong board, or detection can't tell two boards apart. Forcing a model also sets what follows from it: the backlight pin, which bus the XPT2046 is on, the red LED pin, the touch chip (XPT2046, GT911 or CST820), inversion, touch directions, screen size and power key. The panel is still detected. The rows below can still adjust any of these. |
| Screen size | 2.4" / 2.8" / 3.2" / 3.5" / 4.0" | `display.diagonalTenths()` is wrong or 0. The 32E 3.5" and 4.0" are built alike, so detection reports 0 for them. Set this to name yours, and `modelName()` then says "ESP32-32E 4.0\"" (or 3.5"). It changes no pins and no drawing. |
| Panel | ILI9341 / ST7789 / ST7796 | Garbled or skewed picture, wrong size. **Only offered when the panel didn't identify itself** (`display.panelGuessed()`); an ID the panel answered is never overridden. |
| Color order | RGB / BGR | Red and blue swapped. |
| Inversion | off / on | Inverse video: white background, cyan where red belongs. |
| Mirror X (MX) | clear / set | Picture mirrored left-to-right. The panel's own MX bit in rotation 0; the other rotations follow. |
| Mirror Y (MY) | clear / set | Picture upside down and mirrored. The MY bit in rotation 0. |
| SPI clock | 27 MHz / 20 MHz | Speckles, shifted or smeared pixels (auto is 40 MHz). `TouchCalibrate` itself always runs at 20 MHz. |
| Backlight | GPIO21 / GPIO27 | Screen black, or the backlight doesn't dim. `TouchCalibrate` lights both pins unless a GT911 touch chip answers on I²C (where GPIO21 could be its interrupt line), so it stays visible either way — check the result in another sketch. |
| Touch bus | own pins / display bus | No touch at all (XPT2046 boards only). |
| Touch mirror X | off / on | Touch runs backwards left-to-right. |
| Touch mirror Y | off / on | Touch runs backwards top-to-bottom. |
| Touch swap X/Y | off / on | Moving your finger sideways moves the dot up and down. |
| Red LED | GPIO4 / GPIO22 | `display.redLedPin()` names the wrong pin. With GPIO22, GPIO4 is taken to be the amplifier switch. |
| Green / blue LED | 16 / 17 or 17 / 16 | Green and blue swapped. |
| GPIO34 | light sensor / battery | `display.gpio34IsBattery()` gives the wrong answer (as `BoardDetect` shows). |
| Power key (SW1) off | absent / hidden, press twice, hold 10 s, press twice or hold | `display.powerOffHint()` gives the wrong gesture to turn the board off on battery. Choose **absent / hidden** if the board's enclosure covers SW1: it is then reported as having no power key. See [README-power.md](README-power.md#the-power-key-on-the-sw1-boards). |

None of the pin choices can short anything. On every known board the "other" pin of
each pair either floats or has a resistor to a rail, and no chip drives it. A wrong
choice just leaves a feature not working.

## If an override leaves the screen unusable

**Press RST, then straight away press and hold BOOT for a second or two.**
`TouchCalibrate` then starts with every override ignored and opens the Settings screen,
with a note that the saved overrides aren't in effect. From there you can fix the bad
setting or **Clear all**. (Holding BOOT *while* pressing RST is different: it puts the ESP32
in its bootloader, waiting for an upload.)

This only works when `TouchCalibrate` is the sketch on the board. With another sketch
on it, flash `TouchCalibrate` first. Its recovery start doesn't depend on anything the
overrides set.

As a last resort, erase the board's NVS from a computer:

```sh
esptool --port /dev/cu.<port> erase_region 0x9000 0x5000
```

That's where the NVS partition sits on the standard ESP32 partition schemes. It also
erases the saved touch calibration and anything else the sketch keeps in NVS, such as
WiFi credentials. `esptool erase_flash` works too, but also erases the sketch.

## In a sketch

Most sketches never touch any of this: `begin()` applies the overrides by itself. For a
sketch that wants to look or take over:

```cpp
display.useOverrides(false);           // before begin(): ignore the saved overrides
display.setSpiClock(20000000);         // before begin(): fix the panel clock
lv_setup.begin();

const cydr_overrides_t &o = display.overrides();   // what's in effect (0 = auto)
cydr_overrides_t detected = {};
display.autoValues(detected);          // what detection chose for the display fields
lv_setup.touch().autoValues(detected); // ...and the touch fields

cydr_overrides_t mine = {};
mine.colorOrder = 2;                   // BGR
cydr_saveOverrides(mine);              // applies from the next begin()
cydr_clearOverrides();
```

Each field holds `CYDR_OVR_AUTO` (0) or one of the values listed in
`chipguy_CYD_resistive_display.h`: 1 or 2 for most, 1–3 for the panel, a
`cydr_model_t` value for the model, 1–5 for the size (an index into
`cydr_sizeTenths`), and 1–4 for the power key (`cydr_power_key_t` values). For live previews,
`display.setColorOrder()`, `setInversion()`, `setMirror()` and
`touch.setAxes()` change the hardware without saving anything. Storage is one NVS blob,
namespace `cydrforce`, key `v1`. New fields are only ever added at the end. A blob saved by
an earlier library version still loads, and the fields it lacks read as auto. When an override is in effect, `begin()` lists it on
Serial.
