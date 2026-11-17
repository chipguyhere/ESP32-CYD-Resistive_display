# NM-CYD-C5 (ESP32-C5) — the rest of the board

What is on the NM-CYD-C5 besides the display: the connectors, the LED, the buttons,
the pins the library already owns, and what is left for you. Power is in
[README-power-ESP32-C5.md](README-power-ESP32-C5.md), sound in
[README-sound-ESP32-C5.md](README-sound-ESP32-C5.md), the card slot in
[README-sdcard-ESP32-C5.md](README-sdcard-ESP32-C5.md), and the electronics behind any
figure here in [README-engineering.md](README-engineering.md#nm-cyd-c5-esp32-c5). The
schematic, narrated part by part, is
[schematic-description-NM-CYD-C5.md](schematic-description-NM-CYD-C5.md). The classic
ESP32 boards are in [README-board-ESP32.md](README-board-ESP32.md).

Facts marked *(bench)* were checked on 2026-11-09 on two units, both chip revision v1.0
with ST7789 panels: one with the multicolour silkscreen that labels each part of the
board (MAC `3c:dc:75:99:8d:70`), one with the plain look of a classic CYD
(`38:44:be:27:02:88`). Interactive checks (touch, sound, the LED) were on the first.

## Which board this is

| | |
|---|---|
| Maker | RockBase IoT / NMTech, sold as **NM-CYD-C5** (also a "Colorful" edition and an external-antenna "-Ant" edition) |
| Chip | **ESP32-C5** on an ESP32-C5-WROOM-1-**N16R8** module: single-core RISC-V at 240 MHz, **16 MB flash, 8 MB PSRAM**, dual-band (2.4 + 5 GHz) Wi-Fi 6, BLE 5, 802.15.4 (Thread / Zigbee) |
| Screen | 2.8", 240×320, **ST7789** (the panel answers with its ID; an ILI9341 panel fits the same connector and is detected too), resistive touch (XPT2046) |
| Look for | the classic CYD's size and layout, **two USB-C ports**, one at the bottom-left corner and one on the bottom edge, a 12-pin FPC connector, and an RGB LED that is a single WS2812 |

**Detection.** The build names the board: a sketch compiled for the ESP32-C5 gets
`display.model() == CYDR_MODEL_NM_CYD_C5`, and `display.begin()` asks only the panel
for its ID. None of the classic boards' probing (backlight pull-downs, GPIO4's
voltage, the I²C touch probe) runs on the C5 — those pins don't exist or mean
something else here.

**Arduino IDE settings.** Board **"ESP32C5 Dev Module"**, then:

| Option | Set to | Why |
|---|---|---|
| **PSRAM** | **Enabled** | Required: the library refuses to build without it, and the LVGL examples put LVGL's memory there. The core's default is Disabled. |
| Flash Size | 16MB | The module has 16 MB; the default 4MB works but wastes the rest. |
| Partition Scheme | any; e.g. "16M Flash (3MB APP/9.9MB FATFS)" | |
| Upload Speed | 921600 works | Uploading at 921600 through the CH340 port verified cleanly *(bench)*. Reading back from the board (esptool `read-flash`) is a different matter — see [USB ports](#usb-ports). |

FQBN: `esp32:esp32:esp32c5:PSRAM=enabled,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB`.

**Orientation.** Rotation 0 is portrait with the two USB ports at the **bottom**
(the module on the back is then at the top). *(bench)*

## USB ports

Two USB-C sockets, with the board in rotation 0 (USB at the bottom): one at the bottom left, one on the bottom edge. Either one powers the board.

| Port | Goes to | Shows up on the Mac as | Use it for |
|---|---|---|---|
| Bottom-left | CH340C USB-serial chip → UART0 (GPIO11 TX / GPIO12 RX), with the usual DTR/RTS auto-reset into the bootloader | `cu.wchusbserial…` | Uploading, and `Serial` with **USB CDC On Boot: Disabled** (the default) |
| Bottom (the other) | The C5's own USB (GPIO13 D− / GPIO14 D+): USB Serial/JTAG | `cu.usbmodem…` | `Serial` with **USB CDC On Boot: Enabled**; JTAG debugging |

The stock firmware did not let esptool reset it into the bootloader over the
C5's own port; the CH340 port did *(bench)*. Uploading at 921600 through the CH340
works, but reading the flash back through it failed at 921600 and 460800 and was
unreliable at 230400; 115200 was clean *(bench)* — the limit looks like the Mac's CH340
driver's receive side, so back up a board's flash at 115200.

## LED, buttons and other on-board I/O

| Thing | Pin | Notes |
|---|---|---|
| **RGB LED** | GPIO27 | A **WS2812** (one addressable LED), not three separate colours. Use the core's `rgbLedWrite(27, r, g, b)`; `display.rgbLedPin()` returns 27. Red, green and blue came out in that order *(bench)*. Its data line is level-shifted to 5 V by a MOSFET, so the LED needs 5 V — it works on USB power. |
| **BOOT** button | GPIO28 | Pressed = LOW (10 k pull-up on the board). Usable as a normal button after boot *(bench)*; held while the board resets it enters the bootloader. The CH340's auto-reset circuit also drives this pin while a serial program toggles DTR/RTS. |
| **RST** button | EN | Not readable. Also resets the panel (its reset line is on EN). |
| Temperature / humidity sensor (AHT20, U28) | I²C, GPIO9 SDA / GPIO8 SCL | **Not fitted.** The schematic has one, but U28 is an empty footprint on both units seen (one with the multicolour "Colorful" silkscreen, one plain), and nothing answers on GPIO8/9 *(bench)*. The I²C pull-ups are fitted. |
| Speaker connector | GPIO26 | Through an always-on amplifier: [README-sound-ESP32-C5.md](README-sound-ESP32-C5.md). |

There is no light sensor, no battery input and no GPIO34 on this board.

## Connectors

All small connectors are 4-pin, 1.25 mm pitch JST-style ("HCZZ0001-4").

| Connector | Pin 1 | Pin 2 | Pin 3 | Pin 4 | Meant for |
|---|---|---|---|---|---|
| **CN1** | 3.3 V | GPIO9 (SDA) | GPIO8 (SCL) | GND | I²C sensors |
| **P1** | GPIO4 | GPIO8 | GPIO26 | GND | Spare I/O (GPIO26 is also the speaker; GPIO8 also CN1's SCL) |
| **P5** | 5 V | GPIO4 (C5's RX), via 100 Ω | GPIO5 (C5's TX), via 100 Ω | GND | A GPS module or other UART device, on the C5's LP UART pins |

**FPC2** (12-pin, 0.5 mm, "NM extend IO") carries the SPI bus and the spare pins for
RockBase's add-on boards: 1 GPIO2 (MISO), 2 GPIO6 (SCK), 3 GPIO7 (MOSI), 4 GPIO10
(the TF card's CS), 5 GND, 6 GPIO4, 7 GPIO8, 8 GPIO5, 9 GPIO9, 10 USB D−, 11 USB D+,
12 GND.

## Which pins are already taken

The display, touch and TF card **share one SPI bus**.

| GPIO | Used for | Owned by |
|---|---|---|
| 1 | Touch CS (XPT2046) | this library |
| 2 | SPI MISO: panel SDO, touch DOUT, card DAT0 (10 k pull-up) | this library (bus) |
| 3 | Touch IRQ (PENIRQ, 10 k pull-up; LOW while pressed) | this library |
| 6 | SPI SCK | this library (bus) |
| 7 | SPI MOSI | this library (bus) |
| 10 | TF card CS (10 k pull-up) | held HIGH by the library; yours to give to an SD driver |
| 11, 12 | UART0 TX / RX to the CH340 | `Serial` |
| 13, 14 | USB D− / D+ | the C5's USB |
| 23 | Panel CS | this library |
| 24 | Panel DC | this library |
| 25 | Backlight (active HIGH, PWM through `display.setBacklight()`) | this library |
| 26 | Speaker (amplifier input) | yours |
| 27 | WS2812 LED | yours |
| 28 | BOOT button | yours (input) |

**Strapping pins** — sampled at reset, so what you connect to them matters then:
GPIO26, 27 and 28 choose the boot mode (only BOOT, GPIO28, held LOW at reset changes
it), GPIO27 also decides whether the ROM prints its boot messages, GPIO7 (MOSI)
chooses the JTAG source, and GPIO25 and GPIO3 the SDIO clock edges. On this board they
are all held by the circuit already (the backlight's gate pull-down, the touch IRQ's and
card's pull-ups, the LED's level shifter, BOOT's pull-up), so the board boots normally.
Adding something to GPIO26 (the speaker, P1 pin 3) that pulls it hard either way is
harmless unless BOOT is also held.

**Free for your own use:** GPIO0 (no connector — a pad on the module only), GPIO4 and
GPIO5 (P5, P1, FPC2), GPIO8 and GPIO9 (CN1, P1, FPC2; already pulled up for I²C), and
GPIO26 if you don't want sound. GPIO15–22 don't exist on the module's pins (they are
its flash and PSRAM).

## Touch

The XPT2046 shares the display's bus (CS 1, IRQ 3). Raw X runs opposite the panel's
columns and raw Y with its rows, with no swap; the library applies that. A
reading near the bottom edge lands about 20 pixels high until the panel is calibrated,
so run the **TouchCalibrate** example once: one unit's calibration was X 284–3844,
Y 180–3651 against the defaults of 200–3900 *(bench)*. TouchCalibrate's settings
screen offers only the panel and touch fields on this board.

## Adding your own hardware

- **An I²C sensor:** plug it into **CN1** — `Wire.begin(9, 8);` (SDA 9, SCL 8). The
  pull-ups are already on the board (4.7 k, and 10 k more on SDA).
- **A button:** GPIO4 or GPIO5 with `pinMode(pin, INPUT_PULLUP)`, the button to GND.
  GPIO28 (BOOT) is already a button.
- **An extra output:** GPIO4, GPIO5 or GPIO0 (for an LED, a relay driver, …).
- **A UART device (GPS):** **P5** — `Serial1.begin(9600, SERIAL_8N1, 4, 5);` (RX 4,
  TX 5). P5 also gives the device 5 V.
- **Another SPI device:** it goes on the **same bus** as the display (SCK 6, MOSI 7,
  MISO 2 on FPC2), with its own CS on a free pin. Add it as another ESP-IDF device on
  `SPI2_HOST` with `spi_bus_add_device()` — don't call Arduino's `SPI.begin()`, which
  would try to take over the bus the display already owns. The C5 has only this one
  general-purpose SPI controller. The TF card does exactly this:
  [README-sdcard-ESP32-C5.md](README-sdcard-ESP32-C5.md).

## The ESP32-C5's own peripherals, allocated

| Peripheral | Used by |
|---|---|
| SPI2 (the only general-purpose SPI) | the display, touch and TF card, all on one bus |
| One LEDC channel | the backlight (`analogWrite`) |
| UART0 | `Serial` through the CH340 |
| RMT | `rgbLedWrite()` while the LED is in use |
| I²C, LP I²C, UART1, LP UART | free |
| PSRAM (8 MB) | 2 MB for LVGL's heap in the LVGL examples; the rest free |
| Internal RAM | about 288 KB free at boot without LVGL, about 248 KB with an LVGL example running *(bench)* |
