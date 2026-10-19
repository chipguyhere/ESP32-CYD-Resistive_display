/*******************************************************************************
 * Resistive-touch CYD LVGL Calculator
 *
 * For the resistive-touch "Cheap Yellow Display" boards: ESP32-2432S028,
 * ESP32-32E 2.8", 3.2", 3.5", 4.0", and ESP32-024 2.4" (XPT2046 touch; board
 * and panel detected at start-up).
 *
 * A working four-function calculator with a touch keypad, styled after the
 * familiar dark iOS calculator: a black background, dark-gray number keys, a
 * light-gray top function row (backspace / C / %) and an orange operator
 * column (/ x - + =).
 *
 * Resolution independence is the point of this example.  The whole UI is laid
 * out from the live display resolution in ui.cpp — button diameter, gaps,
 * margins and font sizes are all derived from width()/height() at runtime.
 * Nothing is hard-coded to 240x320, so the identical ui.cpp lays out a
 * proportionate keypad on a larger 320x480 panel without edits.  See the
 * layout math in build_keypad() in ui.cpp.
 *
 * Orientation: lv_setup.begin(rotation) accepts 0 = portrait (240x320, the
 * panel's native orientation), 1 = landscape (320x240), 2 = portrait
 * flipped, 3 = landscape flipped (degrees 90/180/270 work as aliases for
 * 1/2/3).  A calculator wants portrait, so this sketch defaults to 0.
 *
 * Requires the following libraries:
 *   - chipguy_ESP32-CYD-Resistive_display  (display + touch driver, this library)
 *   - lvgl 9
 *
 * Arduino IDE Board Settings:
 *   - Board: "ESP32 Dev Module" (in Tools > Board > esp32).
 *   - The defaults work; the sketch fits the default partition scheme.
 *
 ******************************************************************************/

// The bundled lv_conf.h is based on LVGL 9.3 (LVGL 9 compatible).
// It's important for the config and library versions to be compatible.
#include "lv_conf.h"
#include "lvgl.h"
#include "lv_setup.hpp"

// The calculator UI lives in ui.h / ui.cpp in this sketch folder.
#include "ui.h"

void setup() {
    Serial.begin(115200);

    // Initialize display, touch, and LVGL.  Portrait (240x320) by default;
    // pass a rotation to change it (see header).  The UI adapts to whatever
    // resolution the chosen rotation reports.
    lv_setup.begin();
    Serial.printf("LVGL initialized with %dx%d touchscreen\n",
                  display.width(), display.height());

    // Resistive touch varies between panels.  If taps land on the wrong key,
    // flash the bundled TouchCalibrate example once to calibrate the panel.

    // Build the calculator screen.
    ui_init();
}

void loop() {
    // Give loop control to LVGL.
    lv_timer_handler();
    delay(5);
}
