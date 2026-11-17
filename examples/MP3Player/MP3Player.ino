/*******************************************************************************
 * MP3Player — MP3 player for the resistive-touch CYD boards
 *
 * Plays *.mp3 from the root of the microSD card through the board's speaker,
 * with an LVGL touch UI: a track list, prev/play-pause/next/stop, a volume
 * slider, and (classic ESP32 only) a hidden dither A/B screen behind a
 * long-press of the "8" in the click-wheel view's corner.
 *
 * Builds for both chips the library supports.  The files are either shared or
 * belong to one chip; a chip-specific file compiles to nothing on the other
 * chip, and its header comment says which chip it is for.
 *
 *   Shared
 *     MP3Player.ino        this file
 *     player.h/.cpp        card scan + libmad decoder in its own task
 *     ui.h/.cpp            LVGL screens, run from loop()
 *     id3scan.h/.cpp       title / artist from ID3 tags, quickly
 *     CydAudioOut.h        the output interface the player drives
 *     platform.h           the chip-specific pieces, as functions
 *     lv_setup.hpp         display/touch/LVGL glue from the chipguy library
 *     lv_conf.h            LVGL configuration
 *
 *   Classic ESP32 (ESP32-2432S028, ESP32-32E, ESP32-024, ESP32-035, ...)
 *     platform_ESP32.cpp   card on its own VSPI bus via Arduino's SD library
 *     AudioOutputCydDac.h  8-bit DAC on GPIO26, channel 1 ONLY (the library's
 *                          own AudioOutputInternalDAC grabs GPIO25, the touch
 *                          clock), with the ESP32-32E's amplifier switch
 *                          (GPIO4) sequenced around it.  README-sound-ESP32.md
 *                          is the recipe this follows.
 *
 *   ESP32-C5 (NM-CYD-C5)
 *     platform_ESP32C5.cpp card on the display's SPI bus via ESP-IDF's
 *                          SD-over-SPI driver (README-sdcard-ESP32-C5.md)
 *     AudioOutputC5Pdm.h   the I2S peripheral's PCM-to-PDM converter on
 *                          GPIO26: 16-bit samples, no DAC needed
 *                          (README-sound-ESP32-C5.md)
 *
 *   ESP32-S3 (ES3C28P)
 *     platform_ESP32S3.cpp card on SDMMC via Arduino's SD_MMC
 *                          (README-sdcard-ESP32-S3.md)
 *     AudioOutputES8311S3.h I2S into the ES8311 codec, volume in the codec,
 *                          amplifier switch on GPIO1 (README-sound-ESP32-S3.md)
 *
 * Files whose name starts with "." are ignored — macOS leaves "._x.mp3"
 * AppleDouble files beside everything it copies to a FAT card.
 *
 * Requires:
 *   - chipguy_ESP32-CYD-Resistive_display  (this library)
 *   - lvgl 9
 *   - ESP8266Audio  (Library Manager) — its MP3 decoder and file source.
 *     Its own DAC / I2S outputs are NOT used.
 *
 * Board: "ESP32 Dev Module" (the image is ~910 KB, which fits the default
 * 1.2 MB app slot), "ESP32C5 Dev Module" with PSRAM enabled, or "ESP32S3 Dev
 * Module" with USB CDC On Boot enabled and OPI PSRAM (ES3C28P).
 ******************************************************************************/

#include "lv_conf.h"
#include "lvgl.h"
#include "lv_setup.hpp"

#include "player.h"
#include "ui.h"

void setup() {
    Serial.begin(115200);

    // Display + touch + LVGL first.  On the classic ESP32 this claims GPIO25
    // for the touch clock, so the audio side must never enable DAC channel 0;
    // on the ESP32-C5 it sets up the SPI bus the card shares.
    lv_setup.begin();
    Serial.printf("LVGL initialized with %dx%d touchscreen\n",
                  display.width(), display.height());

    // The card (platform-specific mount), playlist scan, and the decoder task.
    bool sdOk = player_begin();

    ui_init(sdOk);
}

void loop() {
    lv_timer_handler();
    delay(5);
}
