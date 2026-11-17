# NM-CYD-C5 (ESP32-C5) — TF / microSD card slot

The card slot's wiring, and how to mount it next to this library. The library does not
include a file system; this is what you need to bring one up. The classic ESP32
boards, whose card has its own bus, are in [README-sdcard-ESP32.md](README-sdcard-ESP32.md).

## The short version

The card **shares the display's SPI bus**, so it must be mounted as a second device
on that bus through ESP-IDF's SD-over-SPI driver — **not** with Arduino's `SD` library,
which brings up its own `SPIClass` on the same controller. The code below mounted a
card and the display kept drawing correctly afterwards *(bench, 2026-11-09: a 120 MB
card at 20 MHz)*. The **MP3Player** example mounts it this way
(`platform_ESP32C5.cpp`) and plays 44.1 kHz MP3s from it while LVGL redraws, without a
dropout.

## Interface and pinout

SPI mode only (the slot's DAT1 / DAT2 are not connected to the C5).

| Card signal | SPI role | GPIO | Shared with |
|---|---|---|---|
| CLK | SCK | 6 | panel, touch |
| CMD | MOSI | 7 | panel, touch |
| DAT0 | MISO | 2 | panel SDO, touch DOUT |
| DAT3 / CD | CS | **10** | — |
| card detect switch | — | not connected | |

All four card lines have 10 k pull-ups to 3.3 V on the board (RN2 and R13), so no
software pull-ups are needed. GPIO10 is also on FPC2 pin 4.

## Bus sharing

- The display (CS 23) and the XPT2046 touch controller (CS 1) are on the same three
  wires. The display driver owns the bus: `display.begin()` initializes it as
  `SPI2_HOST`, the C5's only general-purpose SPI controller, and holds GPIO10 HIGH so
  the card can't answer during the panel's ID reads.
- Mount the card **after** `display.begin()`, as another device on `SPI2_HOST`. The
  ESP-IDF SPI master driver then takes turns between the three devices, one
  transaction at a time.
- Card reads hold the bus while they run, so the display waits for them, and the other
  way round. Large reads during a busy animation will cost frames. Touch is sampled
  between display writes and is not affected.
- **Don't use Arduino's `SD` / `SPI` libraries here.** `SPI.begin()` sets up the same
  SPI2 controller with its own driver, underneath the ESP-IDF driver the display uses
  (inferred from the core's source; not tried).

## SPI hosts already in use

| Host | Used by |
|---|---|
| `SPI2_HOST` | this library: the panel at 40 MHz, touch at 2 MHz; add the card here |

There is no other general-purpose SPI controller on the ESP32-C5.

## Strapping and reset

Of the card's pins only **GPIO7** (CMD / MOSI) is an ESP32-C5 strapping pin: it picks
the JTAG signal source during early boot, has no internal pull, and is held HIGH here
by the card's 10 k pull-up. CS (GPIO10) and DAT0 (GPIO2) are not strapping pins. A card
left in the slot did not stop the board booting *(bench)*.

A sketch meant for more than one board can ask the library for the card's CS pin
instead of writing the number in: `display.sdCsPin()` (or `cydr_sdCsPin()`) returns 5
on the classic boards, 10 on the NM-CYD-C5, and −1 on the ES3C28P, whose card is
SDMMC and has no CS. It is fixed per chip and works before `begin()`.

## Starting point (not a supported API)

```cpp
#include <esp_vfs_fat.h>
#include <driver/sdspi_host.h>

// After display.begin(): the card as a second device on the display's bus.
// Files are then under "/sd" with the C stdio / POSIX calls (fopen, opendir, ...).
bool mountCard(sdmmc_card_t **card) {
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = 20000;                 // 20 MHz: tested
    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.gpio_cs = GPIO_NUM_10;
    dev.host_id = SPI2_HOST;
    esp_vfs_fat_sdmmc_mount_config_t mc = {};
    mc.max_files = 4;
    mc.allocation_unit_size = 16 * 1024;
    display.waitFlush();                       // no pixel DMA in flight
    return esp_vfs_fat_sdspi_mount("/sd", &host, &dev, &mc, card) == ESP_OK;
}
// Unmount: esp_vfs_fat_sdcard_unmount("/sd", card);
```

With no card in the slot the mount fails after about a second with `ESP_ERR_TIMEOUT`
(0x107), and the display is unaffected *(bench)*.

**TODO (bench):** the highest card clock that works on this shared bus (40 MHz is the
SD-over-SPI maximum); the cost to an LVGL animation's frame rate of reading a file while
it runs.
