# Resistive-touch CYD boards — TF / microSD card slot

The card slot's wiring, so a sketch can mount it with whichever driver it prefers. This
library does not drive the slot and does not include a filesystem. The board has a
microSD (TF) slot wired for **SPI mode only**.

## Every board: the same four pins

**All six boards this library supports put the card on the same pins — CS GPIO5, SCK
GPIO18, MISO GPIO19, MOSI GPIO23 — and the same call mounts it on every one of them.**
That is tested, not inferred: the bundled **`MP3Player`** example mounts the card and
plays MP3s from it on each board, with no per-board code. Its mount is in
`examples/MP3Player/player.cpp` (`player_begin()`), and is the reference for any sketch
that wants the card:

```cpp
#include <SPI.h>
#include <SD.h>
// After lv_setup.begin() (or before — the card's bus is separate from the display's).
// The default SPI object is VSPI on 18/19/23; no setPins(), no board check.
if (!SD.begin(/*cs*/ 5, SPI, 20000000)) {     // 20 MHz for audio streaming
    SD.end();
    SD.begin(5, SPI, 4000000);                // fall back to the 4 MHz default
}
```

| Board | Card pins (CS / SCK / MISO / MOSI) | Mount tested with `MP3Player` | Pull-ups, SPI header |
|---|---|---|---|
| ESP32-32E 2.8" | 5 / 18 / 19 / 23 | **yes** (ILI9341 and ST7789) | CS, MISO, MOSI pulled up on the board, SCK not (bench), on both panel versions; header documented below |
| ESP32-2432S028 | 5 / 18 / 19 / 23 | **yes** | **TODO (bench)** |
| ESP32-32E 3.2" | 5 / 18 / 19 / 23 | **yes** | CS, MISO, MOSI pulled up on the board, SCK not (bench); header **TODO (bench)** |
| ESP32-32E 3.5" | 5 / 18 / 19 / 23 | **yes** | CS, MISO, MOSI pulled up on the board, SCK not (bench); header **TODO (bench)** |
| ESP32-32E 4.0" | 5 / 18 / 19 / 23 | **yes** | CS, MISO, MOSI pulled up on the board, SCK not (bench); header **TODO (bench)** |
| ESP32-024 | 5 / 18 / 19 / 23 | **yes** | CS, MISO, MOSI pulled up on the board, SCK not (bench, two units); header **TODO (bench)** |
| ESP32-035 | 5 / 18 / 19 / 23 | **yes** | CS, MISO, MOSI pulled up on the board, SCK not (bench); header **TODO (bench)** |

"(bench)": with the card out, each pin was checked on one unit of each board (two of the
32E 3.5" / 4.0" and two ESP32-024s, 2026-10-23). CS, MISO and MOSI read HIGH even with the ESP32's own
pull-down switched on, and snap back HIGH within a fraction of a microsecond when
released. SCK holds whatever level it was left at. So the card's lines idle correctly
before `SD.begin()` and nothing needs enabling in software.
[How it was measured](README-engineering.md#card-slot).

A sketch that runs on more than one of these boards does **not** need to ask
[which board it's on](README-board-detection.md) before mounting the card.

What differs between boards is only *around* the slot. On the 3.2", 3.5", 4.0", ESP32-024 and ESP32-035 the
touch chip shares the display's HSPI bus rather than being bit-banged — which changes
nothing for the card, since the card is on VSPI. But **GPIO27 is their backlight**, so
the 2.8"'s "IO27 as the SPI header's chip-select" below does not carry over; what their
headers carry is **TODO (bench)**. The rest of this file — the header and the pinout's
schematic detail — was written from the **ESP32-32E 2.8"**'s schematic; its pull-ups
match what the bench found on every board in the table above.

## Pinout (from the ESP32-32E 2.8" schematic)

| Slot pin | GPIO | SPI role | Notes |
|---|---|---|---|
| CS (DAT3) | **GPIO5** | CS | Pull-up on the board. A strapping pin, but harmless here — the pull-up keeps CS high through reset |
| DI (CMD) | **GPIO23** | MOSI | Pull-up on the board |
| DO (DAT0) | **GPIO19** | MISO | Pull-up on the board |
| CLK | **GPIO18** | SCK | No pull-up (correct) |
| DAT1, DAT2 | — | — | Not routed; pulled up on the board so the card never sees them float |
| CD (switch) | — | — | Not routed — **no card-detect** |

Source: this board's schematic, which matches the ESP32-2432S028 sibling's slot (the
ESP32 Arduino core's `variants/jczn_2432s028r/pins_arduino.h`, `CYD_SD_*`, `VSPI`) and
the widely-published CYD pin maps. These four are the classic ESP32's **VSPI pins** —
the ones Arduino's default `SPI` object already uses — and run at full speed. **The
pull-ups are on the board**, so nothing needs enabling in software, and a card sitting
in the slot before the sketch initialises SPI sees a properly idle bus.

**SDMMC mode is not an option** on this board: use `SD`, `SdFat`, or an IDF `sdspi`
mount — never `SD_MMC`. ([Why](README-engineering.md#card-slot).)

## Bus sharing

**Shared with the SPI header — by design.** The board's 4-pin header labeled **SPI**
is, from pin 1, **IO23 (MOSI), IO19 (MISO), IO18 (SCK), IO27 (CS)**: the card's three
data lines brought out with a separate chip-select. An external SPI device on that
header is a second device on VSPI beside the card, which is exactly what the Arduino
`SPI` object is built for — same bus, one CS each, and both work as long as neither CS
is left asserted. Two consequences:

- With nothing on the header there is no sharing at all; the card is alone on
  GPIO5/18/19/23, and IO27 is a free GPIO (see
  [README-board.md](README-board.md#which-pins-are-already-taken)).
- IO27 is shared with nothing else on the board, so a device on the header has no side
  effects. It has **no pull-up**, so drive it HIGH early in `setup()` — before
  `SD.begin()` — so the external device isn't selected while the card mounts. Keep
  each CS HIGH whenever the other device is being addressed.
- **Not using the card at all?** Then the header's four pins are plain GPIOs, and the
  usual thing to do with them is I²C — see
  [README-board.md](README-board.md#which-pins-are-already-taken).

Nothing else touches the card's pins: the display is on HSPI (GPIO13/14/15/2, with 12 as its MISO),
the touch is bit-banged on GPIO25/32/33/39/36, and the LED, amplifier, speaker, battery
sense, backlight and boot button are elsewhere — the full allocation is in
[README-board.md](README-board.md#which-pins-are-already-taken).

## SPI hosts already in use

| Host | Arduino name | Used by | For |
|---|---|---|---|
| `SPI2_HOST` | `HSPI` | this library | ILI9341 / ST7789 panel, write-only after a one-time ID read, DMA |
| `SPI3_HOST` | `VSPI` | — | **free — and it's where the card and the SPI header are.** The default `SPI` object on a classic ESP32 |
| SPI0/SPI1 | — | ESP-IDF | flash — never touch |

Touch uses no host (bit-banged), so there is nothing to coordinate with it. Both
general-purpose hosts are therefore spoken for: anything else that wants hardware SPI
joins VSPI through the SPI header (CS per device, same host) or bit-bangs.

## Starting points

```cpp
// SPI on VSPI — Arduino core. The default `SPI` object already *is* VSPI on
// GPIO18/19/23 for a classic ESP32, so no setPins() is needed.
#include <SPI.h>
#include <SD.h>
bool ok = SD.begin(/*cs*/ 5);
```

```cpp
// Same, but explicit about the pins — for readers, or if SPI was re-pinned elsewhere
SPIClass sdSPI(VSPI);
sdSPI.begin(/*sck*/ 18, /*miso*/ 19, /*mosi*/ 23, /*cs*/ 5);
bool ok = SD.begin(5, sdSPI);
```

```cpp
// A second device on the SPI header, sharing the bus with the card.  Its CS is
// IO27; keep both CS lines high until each device is addressed.
pinMode(27, OUTPUT); digitalWrite(27, HIGH);
SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
digitalWrite(27, LOW);  /* ... transfer ... */  digitalWrite(27, HIGH);
SPI.endTransaction();
```

Call `SD.begin` after `lv_setup.begin()` or before — the two buses are independent.
For audio streaming mount at 20 MHz (`SD.begin(5, SPI, 20000000)`); if a mount fails
intermittently with a long ribbon of wiring on the SPI header, try `SD.begin(5, SPI,
4000000)` to slow the clock. None of these calls is a supported API of this library;
they are the calls that match the wiring.
