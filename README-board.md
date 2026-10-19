# Resistive-touch CYD boards — the rest of the board

What is on these boards besides the display: the connectors, the LED, the buttons, the
pins the library already owns, and what is left for you. There is no
`chipguy_*_board` library for them — the display library is the only one — so this
file lives here. Power is in [README-power.md](README-power.md), sound in
[README-sound.md](README-sound.md), the card slot in [README-sdcard.md](README-sdcard.md),
how a sketch tells the boards apart in [README-board-detection.md](README-board-detection.md),
and the electronics behind any figure here in [README-engineering.md](README-engineering.md).

> **Coverage.** The connectors, LED and free-pin sections below describe the
> **ESP32-32E 2.8"**, the board documented in full. What is known about the others —
> and how they differ — is in [Other boards](#other-boards).

## Which boards these are

All are a classic **ESP32** (not S3) with **4 MB flash, no PSRAM**. Build for
"ESP32 Dev Module".

| Board | How to recognise it | Wiring | Panel |
|---|---|---|---|
| **ESP32-2432S028** | the original CYD; micro-USB (some have USB-C too); "ESP32-2432S028" on the back | own touch | ILI9341 or ST7789, 240×320 |
| **ESP32-32E 2.8"** | silkscreen *2.8" LCD Display / ESP32-32E 240x320 / Resistance Touch*; single USB-C; 2-pin battery connector by the card slot | own touch | ILI9341 or ST7789, 240×320 |
| **ESP32-32E 3.2"** | silkscreen *3.2" LCD Display / ESP32-32E 240x320 / Resistance Touch* | shared touch | ST7789, 240×320 |
| **ESP32-32E 3.5"** | silkscreen *3.5" LCD Display / ESP32-32E 320x480 / Resistance Touch* | shared touch | ST7796, 320×480 |
| **ESP32-32E 4.0"** | silkscreen *4.0" LCD Display / ESP32-32E 320x480 / Resistance Touch* | shared touch | ST7796, 320×480 |
| **ESP32-024** | 2.4"; "ESP32-024" in the copper on the back; battery connector; three buttons (RST, BOOT, SW1) | shared touch | ST7789, 240×320 |

What else each one has, by `display.model()` (the `BoardDetect` example shows it):

| `model()` | Battery support | Battery reading | Light sensor | Amplifier | Amplifier power switch |
|---|---|---|---|---|---|
| `CYDR_MODEL_2432S028` | no | N/A | **yes**, GPIO34 | yes | no — always on |
| `CYDR_MODEL_32E_28` | **yes** | **GPIO34** = cell ÷ 2 | no | yes | **yes**, GPIO4, LOW = on |
| `CYDR_MODEL_32E_32` | **yes** | **GPIO34** = cell ÷ 2 | no | yes | **yes**, GPIO4, LOW = on |
| `CYDR_MODEL_32E_35_40` | **yes** | **GPIO34** = cell ÷ 2 | no | yes | **yes**, GPIO4, LOW = on |
| `CYDR_MODEL_024` | **yes** | none | **yes**, GPIO34 | yes | no — always on |

All four 32E boards read the same battery the same way: one charged cell moved to
connector to connector read 2.07 V on GPIO34 on each, about 4.14 V at the cell; with no
cell they read 2.08–2.12 V, the charger's own output (bench, 2026-10-18). The amplifier
switch was heard working on the 32E 2.8", 3.2" and 3.5"; on the 4.0" it is inferred from
the identical GPIO4 reading.

**The light sensors only see darkness.** On both boards that have one, GPIO34 reads 0
in room light and under a flashlight alike, and rises only when the board is covered:
to about 1.67 V on the 2432S028 and 0.64 V on the ESP32-024 (one unit each,
2026-10-18). Treat it as a "is it dark?" input, read with `analogReadMilliVolts(34)` at
the default attenuation. [Why](README-engineering.md#light-sensor).

The ESP32-32E silkscreen is three lines — size, board name and resolution, *Resistance
Touch* — shown here with `/` between them.

**Own touch** means the XPT2046 touch chip has its own pins and the backlight is on
GPIO21. **Shared touch** means the touch chip sits on the display's SPI bus and the
backlight is on GPIO27. Capacitive-touch CYDs (e.g. ESP32-2432S032C) are **not**
supported — see [README-board-detection.md](README-board-detection.md#capacitive-boards-fool-these-checks).

**USB-C cables (32E 2.8").** The board's USB-C socket has a wiring shortcut that makes it
look like an audio accessory to "smart" (e-marked) cables, which then refuse to power
it. Plain USB-A-to-C cables and cheap C-to-C cables work; 100 W / Thunderbolt / USB4 /
laptop charger cables usually don't. A dead board on a good cable is this, not a fault.
[Why](README-engineering.md#usb-c-cc-wiring). **TODO (bench):** whether the 3.2" / 3.5" /
4.0" share it.

### Screen variants

Four panel setups turn up across these boards:

| Panel | Size | Found on | Notes |
|---|---|---|---|
| ILI9341 | 240×320 | 2432S028, 32E 2.8" | BGR color order |
| ST7789 | 240×320 | 2432S028, 32E 2.8", ESP32-024 | RGB, columns the other way; color inversion **off** |
| ST7789 | 240×320 | 32E 3.2" | as above, but color inversion **on** |
| ST7796 | 320×480 | 32E 3.5", 4.0" | BGR |

Driving one with another's settings gives a mirror-image picture with red and blue
swapped, or inverse video (white background, cyan for red). `display.begin()` works
out both the wiring and the panel, so none of this needs setting by hand:

- **Wiring**, from which backlight pin is held LOW by a resistor on the board — GPIO21
  (own touch) or GPIO27 (shared touch). Nothing is driven.
- **Panel**, by asking it for its ID on GPIO12, the panel's data-out on every board.
  The panel alone decides the screen size.

Both take well under a millisecond, and rotation, color and touch are then set up to
match. [How it works and what a sketch can ask](README-board-detection.md).

```cpp
display.panel();       // CYDR_PANEL_ILI9341 / _ST7789 / _ST7796, after begin()
display.panelName();   // "ILI9341" / "ST7789" / "ST7796"
display.board();       // CYDR_BOARD_OWN_TOUCH or CYDR_BOARD_SHARED_TOUCH
display.nativeWidth(); // 240 or 320 (nativeHeight(): 320 or 480)
display.redLedPin();   // 4 (2432S028, ESP32-024) or 22 (every ESP32-32E board)
display.greenLedPin(); // 16, or 17 on the ESP32-024
display.blueLedPin();  // 17, or 16 on the ESP32-024
display.model();       // CYDR_MODEL_024, _2432S028, _32E_28, _32E_32, _32E_35_40
display.modelName();   // "ESP32-024 2.4\"", ...
display.setPanel(CYDR_PANEL_ST7789);   // before begin(): skip panel detection, force a type
```

A force is only needed if detection gets it wrong. If no ID is recognised, the library
prints the bytes on Serial and assumes ILI9341 on an own-touch board, ST7796 on a
shared-touch one — which would be wrong for a 3.2" whose panel stayed silent. None like
that is known.

Checked on the bench, one board of each, 2026-10-10 to 2026-10-13: panel detection on
all of them; all four rotations and touch on the 32E 2.8" (both panels); touch, color
and calibration on the 3.2"; touch on the 2432S028 and the 4.0". On 2026-10-18, all six
boards (2432S028, 32E 2.8" ST7789, 3.2", two 3.5"/4.0", ESP32-024) at once: model
detection, corner colors and touch targets in rotations 0 and 1, and an SD mount —
all passed. **TODO (bench):** rotations 2 and 3 on the 3.2", 3.5", 4.0" and ESP32-024;
`TouchCalibrate` on the ESP32-024.

**GPIO12 is not a display reset** on the 2.8" boards: holding it LOW did nothing to
either panel. The driver issues a software reset instead. Where the panel's hardware
reset goes is **TODO (bench)** — on the 2432S028 it's the ESP32's EN, so the RST button
resets the screen too. Details in
[README-engineering.md](README-engineering.md#display-panel-variants).

## Connectors

These are the **ESP32-32E 2.8"**'s. Every connector on the board — the three 4-pin headers, the speaker and the battery —
is 1.25 mm-pitch **Molex PicoBlade** or compatible (sold as "MX1.25", sometimes
"JST 1.25"; **not** JST GH, whose housings don't fit). Nothing ships plugged in: no
battery, no battery lead, no speaker. Buy or make MX1.25 pigtails. Pins are listed
**from pin 1** (the silkscreen dot).

| Connector | Pins, from pin 1 | Notes |
|---|---|---|
| **UART** (4-pin) | 5V, GND, TXD (GPIO1), RXD (GPIO3) | **5V is bidirectional:** feed 5 V in here and the board runs and charges as if on USB (reverse polarity is blocked); with USB plugged in, it *outputs* USB 5 V for an external device. On battery alone it is dead. Don't power USB and this pin from two supplies at once. **RXD does not work** — the USB bridge chip drives the same line harder, on battery too. TXD out is fine. |
| **SPI** (4-pin) | IO23 (MOSI), IO19 (MISO), IO18 (SCK), **IO27 (CS)** | The microSD card's bus with a spare chip-select — the way to add an SPI device. See [README-sdcard.md](README-sdcard.md#bus-sharing). **If you aren't using the SD card, these are four free GPIOs** — the place for an I²C sensor (below). IO27 has no pull-up on the board and is shared with nothing else. No power pins on this header; take 3.3V and GND from the unlabeled one. |
| unlabeled (4-pin) | 3.3V, IO35, (blank), GND | IO35 is a free **input-only** pin with no pull-up on the board and no internal pull-up available — good for an analog input or a signal something else drives; a switch to GND needs its own pull-up resistor. 3.3V is the regulator's output; don't feed 3.3 V *in* here. |
| **speaker** (2-pin) | SP+, SP− | Amplifier output; 4–8 Ω speaker. Neither pin is ground. See [README-sound.md](README-sound.md). |
| **battery** (2-pin) | **BAT+**, GND | One lithium cell. **Pin 1 (positive) is the pin nearer the card slot.** Cheap pigtails come wired both ways — check before plugging in; there is no reverse-polarity protection on the battery input. See [README-power.md](README-power.md). |

## LED, buttons and other on-board I/O

| What | GPIO | Notes |
|---|---|---|
| RGB LED, red | **22** | Common-anode, **active LOW** — `LOW` lights it, `HIGH` turns it off. (Confirmed. On the 2432S028 and ESP32-024 red is GPIO4; `display.redLedPin()` returns the right one.) |
| RGB LED, green / blue | 16 / 17 | Same polarity. (Confirmed.) Neither pin is shared with anything else. On the ESP32-024 they are the other way round; `display.greenLedPin()` / `blueLedPin()` return the right ones. |
| Audio amplifier enable | **4** | **LOW = amplifier on.** A pull-up holds it off at reset. See [README-sound.md](README-sound.md). |
| Battery voltage | **34** | Analog input, input-only: reads half the cell voltage. See [README-power.md](README-power.md#battery-measurement). |
| BOOT button | 0 | Pulled up; reads LOW when pressed. Usable as a UI button and as a deep-sleep wake source. It is also the strapping pin: holding it through a reset enters the bootloader instead of your sketch. |
| RST button | — | Resets the chip (EN). Not visible to software. |
| Touch interrupt | 36 | The touch controller's pen-down line, LOW while the screen is pressed. The library reads it as an input; a sketch can also use it as a deep-sleep wake source. See [README-power.md](README-power.md#shutting-down-sleeping-waking). |
| Backlight | 21 | Driven by `display.setBacklight(0..100)`. (GPIO27 on the shared-touch boards.) |

At reset the three LED pins float and the LED may glow dimly; a sketch that cares
drives all three `HIGH` first thing in `setup()`.

## Which pins are already taken

On the 32E 2.8", the library and the board's fixed hardware own nearly every GPIO on a
classic ESP32. The touch row is what changes on the shared-touch boards (see
[Other boards](#other-boards)):

| Owner | GPIOs |
|---|---|
| Display (this library, HSPI) | 13 (MOSI), 14 (SCK), 15 (CS), 2 (DC), 21 (backlight), and 12 (panel data-out: MISO on the HSPI bus, an input only) |
| Touch (this library, bit-banged) | 25 (CLK), 32 (MOSI), 33 (CS), 39 (MISO), 36 (pen-down, input). Shared-touch boards: only 33 (CS) and 36, on the display's bus |
| microSD slot (VSPI — the default `SPI` object) | 5 (CS), 18, 19, 23 |
| SPI header | the same 18, 19, 23, plus 27 (CS) |
| Sound | 26 (DAC out), 4 (amp enable) |
| Battery sense | 34 |
| RGB LED | 22, 16, 17 |
| USB serial | 1 (TX), 3 (RX) |
| BOOT | 0 |

**What that leaves.** The SPI header, IO35, and TXD. The ways to add hardware are:

- **An I²C sensor** — the most common thing to hang off a board like this, and there
  is no I²C header, but **the SPI header's pins are ordinary GPIOs when the SD card
  isn't in use**, and the ESP32 can put I²C on any two of them. Use **IO23 as SDA and
  IO19 as SCL**: both already have 10 kΩ pull-ups on the board (they are the card's),
  which is what I²C needs. Power the sensor from the unlabeled header's 3.3V and GND.

  ```cpp
  #include <Wire.h>
  Wire.begin(/*sda*/ 23, /*scl*/ 19);    // then use the sensor's library as normal
  ```

  Leave the SD card out of the slot while doing this, and don't call `SD.begin()`.
  Most sensor breakouts carry their own pull-ups too; that's fine.
- **An SPI device on the SPI header** — shares the card's bus, its own CS on IO27.
  This is the header's labeled purpose. Both of the ESP32's general-purpose SPI hosts
  are already in use (display on one, card on the other), so a new device joins the
  card's bus rather than getting a host of its own — see
  [README-sdcard.md](README-sdcard.md#spi-hosts-already-in-use). IO27 has no pull-up,
  so drive it HIGH early in `setup()` so the device isn't selected while the card
  mounts.
- **Plain GPIOs on the SPI header** — with no card and no SPI device, IO23, IO19, IO18
  and IO27 are four free bidirectional pins: outputs, buttons (internal pull-ups
  available), a one-wire sensor, a second serial port, a deep-sleep wake source.
  IO27 is also a touch-sense pin. Their ADCs are on ADC2, so no analog reads while
  WiFi is on.
- **IO35 on the unlabeled header** — one analog or digital *input*.
- **TXD on the UART header** — a one-way serial output to something else (the CH340
  will see it too). No I²C bus is brought out anywhere.
- **GPIO25** is *not* available even though it is the ESP32's second DAC: it is the
  touch clock. Nothing that drives it can coexist with touch.

## Other boards

Only screen, touch, sound and the card slot have been tried on the boards below; the
rest is what probing the pins showed, one unit each (2026-10-13; the ESP32-024
2026-10-18). The library treats
everything here exactly as on the 32E 2.8" except where the table says otherwise.

| | ESP32-2432S028 | ESP32-32E 3.2" | ESP32-32E 3.5" / 4.0" | ESP32-024 |
|---|---|---|---|---|
| Backlight | 21 | **27** | **27** | **27** |
| Touch pins | 25 / 32 / 39 / 33, IRQ 36 (own) | display bus 13 / 14 / 12, CS 33, IRQ 36 | display bus 13 / 14 / 12, CS 33, IRQ 36 | display bus 13 / 14 / 12, CS 33, IRQ 36 |
| RGB LED red / green / blue | **4** / 16 / 17 | 22 / 16 / 17 | 22 / 16 / 17 | **4** / **17** / **16** — green and blue the other way round |
| GPIO4 | red LED | amplifier enable, LOW = on (MP3Player works) | amplifier enable (inferred from its pull-up) | red LED |
| Speaker amplifier | always on, no enable | switchable | **TODO (bench)** | always on, no enable — `MP3Player` plays cleanly |
| GPIO34 | light sensor (0 in light; ≈1.67 V in the dark) | battery, cell ÷ 2 (confirmed) | battery, cell ÷ 2 (confirmed on both) | light sensor (0 in light; ≈0.64 V in the dark) |
| microSD | GPIO5 / 18 / 19 / 23, as on the 32E 2.8" — `MP3Player` mounts it | same — `MP3Player` mounts it | same — `MP3Player` mounts it on both. See [README-sdcard.md](README-sdcard.md#every-board-the-same-four-pins) | same — `MP3Player` mounts it |
| GPIO25, 32, 39 | touch | not used by the library; 25 and 32 have pull-ups on the board — **TODO (bench):** where they go | not used by the library; **TODO (bench)** | not used by the library. 32 and 33 carry 4.7 kΩ pull-ups and 25 a reset line, all for a capacitive-touch connector not fitted on resistive boards (schematic). 39 is unconnected |
| GPIO21, 22, 35 | 21 backlight; 22 and 35 on headers | 21 floats — **TODO (bench):** brought out anywhere? | **TODO (bench)** | **CN1** (labelled for a temperature / humidity sensor): 3.3V, IO21, IO22, GND, with a 10 kΩ pull-up on IO21 — an I²C sensor port, `Wire.begin(21, 22)` (add a pull-up on IO22). **P3**: IO21, IO22, IO35, GND (schematic) |
| Battery, charger | none — USB or the VIN pin only | yes; GPIO34 battery reading confirmed | yes; GPIO34 battery reading confirmed | battery connector and an FM5324GA charger / 5 V booster; no battery reading — see [README-power.md](README-power.md) |
| Third button | — | — | — | **SW1**, the FM5324GA's power key, not wired to the ESP32 (schematic; no GPIO changed when pressed) |

**ESP32-2432S028 in short.** No battery and no power management: it runs from USB or
5 V on its P1 header's VIN pin, and its regulators and USB chip keep drawing several mA
even in deep sleep. Its headers are P1 (VIN, TX, RX, GND), P3 (IO21 — the backlight —
IO22, IO35, GND) and CN1 (3.3V, IO27, IO22 on some revisions, GND); IO27 has a 10 kΩ
pull-up there. Sound uses the same DAC pin (GPIO26) and the same MP3Player recipe, with
an amplifier that is always on. It has a light sensor on GPIO34 instead of a battery
reading.

**ESP32-024 in short.** The 3.2"'s display and touch wiring with the 2432S028's LED and
sound: backlight on GPIO27, touch on the display bus, red LED on GPIO4, an amplifier
that is always on. The library tells it apart from the 3.2" by GPIO4
([how](README-board-detection.md)), and sets its screen and touch up differently
(inversion off, touch X and Y swapped). Its copper reads "ESP32-024"; sellers call it
ESP32-2432S024. It replaces the older `chipguy_cyd2432S024_display` library. Its
**CN1** header is an I²C port on IO21 / IO22 with 3.3V, so an I²C sensor plugs straight in;
nothing answered with the header empty. The panel's reset is tied to the ESP32's, so
RST resets the screen too.

**The shared-touch boards free three pins** the own-touch boards spend on touch:
GPIO25, 32 and 39. Whether they reach a header — and so whether they are actually
useful — is **TODO (bench)** on each board.

## The ESP32's own peripherals, allocated

On the 32E 2.8":

| Peripheral | Used by | Free for a sketch? |
|---|---|---|
| HSPI (`SPI2_HOST`) | display | no |
| VSPI (`SPI3_HOST`) | card slot + SPI header | shared — add devices with their own CS |
| DAC channel 1 (GPIO26) | speaker | for sound only |
| DAC channel 0 (GPIO25) | **touch clock** | **no** — enabling it kills touch |
| ADC1 | battery (ch 6 / GPIO34), IO35 (ch 7), touch pins | yes, alongside WiFi |
| ADC2 | IO27 (ch 7); IO23 / 19 / 18 aren't ADC pins | unusable while WiFi is on (ESP32 limitation) |
| UART0 | USB serial | TX also on the header |
| I²C | nothing | yes — on the SPI header (IO23 SDA / IO19 SCL) when the card isn't used |
