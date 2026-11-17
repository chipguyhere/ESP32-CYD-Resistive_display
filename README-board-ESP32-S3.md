# ES3C28P (ESP32-S3) — the rest of the board

This page covers what is on the black 2.8" ESP32-S3 board besides the display: the
connectors, the LED, the buttons, the pins the library already owns, and what is left
for you. Power is in [README-power-ESP32-S3.md](README-power-ESP32-S3.md), sound in
[README-sound-ESP32-S3.md](README-sound-ESP32-S3.md) and the card slot in
[README-sdcard-ESP32-S3.md](README-sdcard-ESP32-S3.md). The electronics behind any
figure here are in [README-engineering.md](README-engineering.md#es3c28p-esp32-s3),
and the schematic, narrated part by part, is in
[schematic-description-ES3C28P.md](schematic-description-ES3C28P.md). The classic
ESP32 boards are in [README-board-ESP32.md](README-board-ESP32.md).

Facts marked *(bench)* were checked on 2026-11-09 on two units: MAC
`14:c1:9f:d1:d5:04` (whose stock firmware is backed up) and `cc:68:c7:8e:61:60`
(the interactive checks).

## Which board this is

| | |
|---|---|
| Silkscreen | *2.8" LCD Display / ESP32-S3 240x320 / Capacitive Touch*, in the same three-line style and font as the ESP32-32E boards, on a **black** PCB |
| Vendor name | **ES3C28P** (with touch). The **ES3N28P** is the same board without the touch panel. The vendor's package calls both "2.8inch ESP32-S3 Display". |
| Chip | **ESP32-S3R8**: dual-core 240 MHz, **8 MB octal PSRAM inside the chip**, plus a **16 MB** QSPI flash chip (25VQ128) beside it; Wi-Fi 4 and BLE 5 |
| Screen | 2.8", 240×320, **ILI9341V** IPS panel (answers `00 93 41`), **FT6336G** capacitive touch |
| Look for | one USB-C port at the **bottom centre**; a small **microphone hole** through the PCB at the **top right** (the microphone sits behind it, on the back); the ESP32-S3 and its antenna are on the **back**, at the top right, so unlike most CYDs the antenna can't be seen from the front; a 2-pin **BAT** connector, a 2-pin **SPEAKER** connector, RESET and BOOT buttons, **no SW1 power key** |

**Detection.** A build for the ESP32-S3 runs the S3's own checks instead of the
classic boards'. `display.begin()` reads the panel's ID on MISO (GPIO13) and looks on
the I²C bus (SDA 16 / SCL 15) for the touch chip (FT6336, 0x38) and the audio codec
(ES8311, 0x18). ILI9341 + codec + touch is the ES3C28P; without the touch chip it is
the ES3N28P. `display.model()` returns `CYDR_MODEL_ES3C28P` (or `_ES3N28P`), and
`modelName()` returns `ES3C28P 2.8" S3`. Any other combination is reported on
`Serial` as an unseen S3 variant, with what was found. *(bench: ES3C28P on both units)*

**Arduino IDE settings.** Board **"ESP32S3 Dev Module"**, then:

| Option | Set to | Why |
|---|---|---|
| **USB CDC On Boot** | **Enabled** | Required. The board has no USB-serial chip, so `Serial` must go to the S3's own USB. The library refuses to build with it Disabled. |
| **PSRAM** | **OPI PSRAM** (or Disabled) | The library refuses "QSPI PSRAM", which fails on this octal part. The examples run either way. |
| Flash Size | 16MB | The default 4MB works but wastes the rest. |
| Partition Scheme | any; e.g. "16M Flash (3MB APP/9.9MB FATFS)" | MP3Player (~990 KB) fits the default scheme too. |
| Upload Mode / USB Mode | defaults (UART0 / Hardware CDC, "Hardware CDC and JTAG") | Uploads over the USB-C port reset into the bootloader by themselves *(bench)*. |

FQBN: `esp32:esp32:esp32s3:CDCOnBoot=cdc,PSRAM=opi,FlashSize=16M`.

The board shows up on a Mac as `cu.usbmodem…`, usually with a `1` on the end of the
name (for example `usbmodem211201`).

**Orientation.** Rotation 0 is portrait with the USB-C port at the **bottom centre**
and the microphone hole at the top right. The panel's columns run the other way from
the classic ILI9341 modules', which the library allows for. *(bench)*

## LED, buttons and other on-board I/O

| Thing | Pin | Notes |
|---|---|---|
| **RGB LED** | GPIO42 | A **WS2812**: one addressable LED. Use the core's `rgbLedWrite(42, r, g, b)`; `display.rgbLedPin()` returns 42. It runs from 5 V and is driven directly from the 3.3 V pin. **TODO (bench):** colour order. |
| **BOOT** button | GPIO0 | Pressed = LOW (10 k pull-up). Usable as a normal button after boot; held while the board resets, it enters the bootloader. |
| **RESET** button | EN (CHIP_PU) | Not readable. Also resets the panel, whose reset line is on EN. |
| Microphone | via the ES8311 | A MEMS microphone (LMA2718B381) on the codec's input. The codec sends its data to GPIO6. Not used by the library. |
| Battery reading | GPIO9 | Half the battery voltage: [README-power-ESP32-S3.md](README-power-ESP32-S3.md). |
| Speaker connector | via the ES8311 and an amplifier | [README-sound-ESP32-S3.md](README-sound-ESP32-S3.md). |

There is no light sensor and no power key.

## Connectors

Four 4-pin 1.25 mm connectors, as on the ESP32-32E boards. Pin numbers are the
schematic's. **TODO (bench):** which end of each connector is pin 1 on the board.

| Connector | Pin 1 | Pin 2 | Pin 3 | Pin 4 | Meant for |
|---|---|---|---|---|---|
| **UART** (P2) | 5 V | GND | GPIO43 (TX0), via 100 Ω | GPIO44 (RX0), via 100 Ω | A serial device, or a USB-serial adapter |
| **IO** (P3) | GPIO2 | GPIO3 | GPIO14 | GPIO21 | Spare I/O (no power pin on this one) |
| **I²C** (P4) | 3.3 V | GND | GPIO15 (SCL) | GPIO16 (SDA) | I²C sensors, on the **same bus as the touch chip and the codec** |
| **BAT** (JP1) | battery + | battery − | | | A single lithium cell |
| **SPEAKER** (JP3) | speaker − | speaker + | | | An 8 Ω speaker |

The UART header's pin 1 reaches the 5 V rail through a P-channel MOSFET whose gate is
tied to ground, so it passes 5 V both ways (inferred from the schematic):
it can supply a device, and 5 V fed in there probably powers the board.
**TODO (bench).**

## Which pins are already taken

| GPIO | Used for | Owned by |
|---|---|---|
| 0 | BOOT button | yours (input) |
| 1 | Amplifier SHUTDOWN (10 k pull-up; LOW = on) | yours; MP3Player drives it |
| 4, 5, 7, 8 | I²S to the codec: MCLK, BCLK, WS, data out | yours; MP3Player uses them |
| 6 | I²S data in from the codec (the microphone) | yours |
| 9 | Battery reading (half the cell voltage) | yours (ADC1) |
| 10, 11, 12, 13 | Panel CS, MOSI, SCK, MISO — the S3's FSPI pins | this library |
| 15, 16 | I²C SCL / SDA: touch, codec, the I²C connector (4.7 k pull-ups) | the touch driver starts `Wire` on them; share it |
| 17 | Touch INT (not used: the library polls) | yours if you need it |
| 18 | Touch RST | this library (held HIGH) |
| 19, 20 | USB D− / D+ | the S3's USB (`Serial`) |
| 38, 39, 40, 41, 47, 48 | The card slot (SDMMC, 10 k pull-ups) | yours: [README-sdcard-ESP32-S3.md](README-sdcard-ESP32-S3.md) |
| 42 | WS2812 LED | yours |
| 43, 44 | UART0 TX / RX on the UART connector | free while `Serial` is on USB |
| 45 | Backlight (active HIGH, PWM through `display.setBacklight()`) | this library |
| 46 | Panel DC | this library |
| 26–37 | Flash and PSRAM | not usable |

**Strapping pins.** GPIO0 (BOOT), GPIO3 (JTAG source), GPIO45 (flash voltage) and
GPIO46 (boot messages) are read at reset. The board holds GPIO45 low with the
backlight's pull-down, and GPIO46 is the panel's DC line. Don't pull GPIO3 (IO
connector) or GPIO45 high at reset.

**Free for your own use:** GPIO2, 3, 14 and 21 on the IO connector, and GPIO43 / 44
on the UART connector while `Serial` stays on USB. GPIO1, 4–8 and 17 are free too if
you don't use sound or the touch interrupt, but they have no connector.

## Touch

The FT6336G reports screen pixels, so it needs no calibration. The library reads it
over I²C every time LVGL asks; its INT line isn't used. Touch matches the picture with
no mirroring in every rotation *(bench)*. The library keeps the chip in its
fast-scanning mode. TouchCalibrate's settings screen offers only the panel and touch
fields on this board.

The touch chip, the audio codec and the I²C connector share one bus, on the `Wire`
object. The touch driver starts it with `Wire.begin(16, 15, 400000)`, so a sketch
that talks to the codec or a sensor can use `Wire` as it is. Calling
`Wire.begin(16, 15)` again does no harm.

## Adding your own hardware

- **An I²C sensor:** plug it into the **I²C** connector and use `Wire` — it's already
  running after `display.begin()` (SDA 16, SCL 15, pull-ups on the board). Avoid
  addresses 0x18 (codec) and 0x38 (touch).
- **A button:** GPIO2, 3, 14 or 21 on the IO connector with `pinMode(pin,
  INPUT_PULLUP)`, the button to GND. Don't hold GPIO3 low at reset. GPIO0 (BOOT)
  is already a button.
- **An extra output:** GPIO2, 3, 14 or 21 (an LED, a relay driver, …).
- **A UART device:** the **UART** connector — `Serial1.begin(9600, SERIAL_8N1, 44,
  43);` (RX 44, TX 43). It also gives the device 5 V.
- **An SPI device:** the display has the S3's FSPI controller to itself, so a second
  device gets the other one: `SPIClass spi(HSPI); spi.begin(14, 2, 21, 3);` (SCK 14,
  MISO 2, MOSI 21, CS 3) on the IO connector.

## The ESP32-S3's own peripherals, allocated

| Peripheral | Used by |
|---|---|
| SPI2 (FSPI) | the display, alone |
| SPI3 (HSPI) | free |
| I²C0 (`Wire`) | touch; shared with the codec and the I²C connector |
| I²C1 (`Wire1`) | free |
| I²S0 | MP3Player's codec output; free otherwise |
| SDMMC | free; the card slot's pins |
| One LEDC channel | the backlight (`analogWrite`) |
| RMT | `rgbLedWrite()` while the LED is in use |
| USB Serial/JTAG | `Serial` |
| UART0, UART1, UART2 | free (UART0's pins are on the UART connector) |
| PSRAM (8 MB, with OPI PSRAM selected) | 2 MB for LVGL's heap in the LVGL examples; the rest free |
