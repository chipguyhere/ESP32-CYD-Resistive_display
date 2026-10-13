/*******************************************************************************
 * MP3Player — MP3 player for the resistive-touch CYD boards
 *
 * The reference implementation for README-sound.md's "recipe" section: how to
 * drive this board's speaker cleanly (DAC channel 1 only, default clock, DMA
 * from its own task) alongside the LVGL touch UI.
 *
 * Plays *.mp3 from the root of the microSD card through the board's speaker
 * (8-bit DAC on GPIO26 -> amplifier, switched on via GPIO4 on the ESP32-32E
 * boards; always on on the ESP32-2432S028), with an
 * LVGL touch UI: a track
 * list, prev/play-pause/next/stop, a volume slider, and — on a screen behind a
 * long-press of the "8" in the click-wheel view's corner — a checkbox that switches
 * dither on the 8-bit quantizer on and off for A/B listening.
 *
 * Structure:
 *   AudioOutputCydDac.h  ESP8266Audio output onto DAC channel 1 only.  The
 *                        library's own AudioOutputInternalDAC grabs GPIO25,
 *                        which is the touch clock on this board.  Also owns
 *                        the amplifier enable (GPIO4, active LOW), switching
 *                        it on only once the DAC is parked at mid-scale.
 *   player.h/.cpp        SD scan + libmad decoder in a task on core 0
 *   ui.h/.cpp            LVGL screen, runs on core 1 with loop()
 *   lv_setup.hpp         display/touch/LVGL glue from the chipguy library
 *   lv_conf.h            LVGL configuration
 *
 * Files whose name starts with "." are ignored — macOS leaves "._x.mp3"
 * AppleDouble files beside everything it copies to a FAT card.
 *
 * Requires:
 *   - chipguy_ESP32-CYD-Resistive_display  (this library)
 *   - lvgl 9
 *   - ESP8266Audio  (Library Manager) — its MP3 decoder and SD file source.
 *     Its own internal-DAC output is NOT used; see AudioOutputCydDac.h.
 *
 * Board: "ESP32 Dev Module".  The image is ~910 KB, which fits the default
 * 1.2 MB app slot; "Huge APP" gives more headroom if it grows.
 ******************************************************************************/

#include "lv_conf.h"
#include "lvgl.h"
#include "lv_setup.hpp"

#include "player.h"
#include "ui.h"

void setup() {
    Serial.begin(115200);

    // Display + touch + LVGL.  This claims GPIO25 for the touch clock, so it
    // goes first; the audio side must never enable DAC channel 0.
    lv_setup.begin();
    Serial.printf("LVGL initialized with %dx%d touchscreen\n",
                  display.width(), display.height());

    // SD card on VSPI (independent of the display's HSPI), playlist scan,
    // and the decoder task.
    bool sdOk = player_begin();

    ui_init(sdOk);
}

void loop() {
    lv_timer_handler();
    delay(5);
}
