# ES3C28P (ESP32-S3) — TF / microSD card slot

How to open the card on the black 2.8" ESP32-S3 board. The classic boards are in
[README-sdcard-ESP32.md](README-sdcard-ESP32.md).

## The short version

The slot is wired for **4-bit SDMMC**, the fast native SD interface, on its own
pins. Nothing about it is shared with the display or touch, so use Arduino's
**`SD_MMC`** library, not `SD`:

```cpp
#include <SD_MMC.h>

SD_MMC.setPins(38, 40, 39, 41, 48, 47);   // CLK, CMD, D0, D1, D2, D3
if (!SD_MMC.begin("/sdcard", false)) {    // false = 4-bit
    Serial.println("no card");
}
```

**MP3Player** and **BoardDetect** both open the card this way *(bench, 2026-11-09: a
card with 25 MP3s mounted and played)*.

A sketch meant for more than one board can ask the library for the card's CS pin
instead of writing the number in: `display.sdCsPin()` (or `cydr_sdCsPin()`) returns 5
on the classic boards, 10 on the NM-CYD-C5, and −1 on the ES3C28P, whose card is
SDMMC and has no CS. It is fixed per chip and works before `begin()`.

## Interface and pinout

| Card pin | Signal | GPIO | Pull-up |
|---|---|---|---|
| 5 | CLK | 38 | 10 k on the board |
| 3 | CMD | 40 | 10 k |
| 7 | DAT0 | 39 | 10 k |
| 8 | DAT1 | 41 | 10 k |
| 1 | DAT2 | 48 | 10 k |
| 2 | CD / DAT3 | 47 | 10 k |

There is no card-detect switch. The pull-ups are on the board (R17, R18, R34–R37),
so no software pull-ups are needed. Schematic nets: `SD_CLK`, `SD_CMD`, `SD_D0`–`SD_D3`.

## Bus sharing

**None.** The card has its own six pins and the S3's SDMMC controller. The display
is on SPI2 (GPIO10–13), and touch and the codec are on I²C. Card reads and screen
drawing don't wait for each other.

## Hosts already in use

| Host | Used by |
|---|---|
| SPI2 (FSPI) | this library: the display |
| SPI3 | free |
| SDMMC slot 1 | free: the card (through the GPIO matrix) |

`SD_MMC` uses slot 1 on the ESP32-S3; these pins aren't slot 0's fixed ones.

## Strapping and reset

None of GPIO38–41, 47 or 48 is a strapping pin, so a card can be in the slot at reset.

## Speed

The default `SD_MMC.begin()` runs at 20 MHz, 4-bit. That's plenty for MP3s.
`SDMMC_FREQ_HIGHSPEED` (40 MHz) is the next option. **TODO (bench):** whether a card
runs at 40 MHz on this board's traces.
