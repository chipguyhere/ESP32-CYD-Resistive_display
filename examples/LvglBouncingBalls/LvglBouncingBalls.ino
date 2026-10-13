/*******************************************************************************
 * Resistive-touch CYD LVGL Bouncing Balls
 *
 * For the resistive-touch "Cheap Yellow Display" boards: ESP32-2432S028 and
 * ESP32-32E 2.8", 3.2", 3.5", 4.0" (XPT2046 touch; panel detected at start-up).
 *
 * A resolution-independent demo: 1..20 circular balls bounce under gravity,
 * each a random color, re-launching to a random height on every floor bounce.
 * Two sliders at the bottom select the number of balls and the speed.
 *
 * The whole UI is laid out from the live display resolution at runtime (see
 * ui.cpp), so the identical ui.cpp / ui.h drop unchanged onto any board whose
 * lv_setup.hpp follows the same shape, in any orientation.
 *
 * Orientation: lv_setup.begin(rotation) accepts 0 = portrait (240x320, the
 * panel's native orientation), 1 = landscape (320x240), 2 = portrait
 * flipped, 3 = landscape flipped (degrees 90/180/270 work as aliases for
 * 1/2/3).  The demo adapts to whatever resolution the chosen rotation reports.
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

// The bouncing-balls UI lives in ui.h / ui.cpp in this sketch folder.
#include "ui.h"

void setup() {
    Serial.begin(115200);

    // Initialize display, touch, and LVGL.  Portrait (240x320) by default;
    // pass a rotation to change it (see header).  The demo adapts to whatever
    // resolution the chosen rotation reports.
    lv_setup.begin();
    Serial.printf("LVGL initialized with %dx%d touchscreen\n",
                  display.width(), display.height());

    // Resistive touch varies between panels.  If the sliders feel off, flash
    // the bundled TouchCalibrate example once to calibrate the panel.

    // Build the bouncing-balls screen.
    ui_init();
}

void loop() {
    // Give loop control to LVGL.
    lv_timer_handler();
    delay(5);
}
