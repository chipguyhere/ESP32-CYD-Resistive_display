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
| **ESP32-32E 2.8"** | silkscreen *2.8" LCD Display, ESP32-32E 240x320, Resistance Touch*; single USB-C; 2-pin battery connector by the card slot | own touch | ILI9341 or ST7789, 240×320 |
| **ESP32-32E 3.2"** | silkscreen *3.2" LCD Display, ESP32-32E 240x320, Resistance Touch* — the 2.8"'s wording with only the size changed | shared touch | ST7789, 240×320 |
| **ESP32-32E 3.5" / 4.0"** | silkscreen *3.5" (4.0") LCD Display, ESP32-32E 320x480, Resistance Touch* | shared touch | ST7796, 320×480 |

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
| ST7789 | 240×320 | 2432S028, 32E 2.8" | RGB, columns the other way; color inversion **off** |
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
display.redLedPin();   // 4 (2432S028) or 22 (every ESP32-32E board)
display.setPanel(CYDR_PANEL_ST7789);   // before begin(): skip panel detection, force a type
```

A force is only needed if detection gets it wrong. If no ID is recognised, the library
prints the bytes on Serial and assumes ILI9341 on an own-touch board, ST7796 on a
shared-touch one — which would be wrong for a 3.2" whose panel stayed silent. None like
that is known.

Checked on the bench, one board of each, 2026-10-10 to 2026-10-13: panel detection on
all of them; all four rotations and touch on the 32E 2.8" (both panels); touch, color
and calibration on the 3.2"; touch on the 2432S028 and the 4.0".
**TODO (bench):** all four rotations on the 3.2", 3.5" and 4.0".

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
| RGB LED, red | **22** | Common-anode, **active LOW** — `LOW` lights it, `HIGH` turns it off. (Confirmed. On the 2432S028 red is GPIO4; `display.redLedPin()` returns the right one.) |
| RGB LED, green / blue | 16 / 17 | Same polarity. (Confirmed.) Neither pin is shared with anything else. |
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
rest is what probing the pins showed, one unit each (2026-10-13). The library treats
everything here exactly as on the 32E 2.8" except where the table says otherwise.

| | ESP32-2432S028 | ESP32-32E 3.2" | ESP32-32E 3.5" / 4.0" |
|---|---|---|---|
| Backlight | 21 | **27** | **27** |
| Touch pins | 25 / 32 / 39 / 33, IRQ 36 (own) | display bus 13 / 14 / 12, CS 33, IRQ 36 | display bus 13 / 14 / 12, CS 33, IRQ 36 |
| RGB LED red / green / blue | **4** / 16 / 17 | 22 / 16 / 17 (inferred) | 22 / 16 / 17 (inferred) |
| GPIO4 | red LED | amplifier enable, LOW = on (MP3Player works) | amplifier enable (inferred from its pull-up) |
| Speaker amplifier | always on, no enable | switchable | **TODO (bench)** |
| GPIO34 | light sensor (≈0.14 V in room light) | 2.1 V on USB — likely the 32E's battery divider; **TODO (bench)** | **TODO (bench)** |
| microSD | GPIO5 / 18 / 19 / 23, as on the 32E 2.8" | same — mounted and played MP3s | **TODO (bench)** |
| GPIO25, 32, 39 | touch | not used by the library; 25 and 32 have pull-ups on the board — **TODO (bench):** where they go | not used by the library; **TODO (bench)** |
| GPIO21 | backlight | floats — **TODO (bench):** brought out anywhere? | **TODO (bench)** |
| Battery, charger | none — USB or the VIN pin only | **TODO (bench)** | **TODO (bench)** |

**ESP32-2432S028 in short.** No battery and no power management: it runs from USB or
5 V on its P1 header's VIN pin, and its regulators and USB chip keep drawing several mA
even in deep sleep. Its headers are P1 (VIN, TX, RX, GND), P3 (IO21 — the backlight —
IO22, IO35, GND) and CN1 (3.3V, IO27, IO22 on some revisions, GND); IO27 has a 10 kΩ
pull-up there. Sound uses the same DAC pin (GPIO26) and the same MP3Player recipe, with
an amplifier that is always on. It has a light sensor on GPIO34 instead of a battery
reading.

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
