/*******************************************************************************
 * Resistive-touch CYD LVGL Claude Code Stub
 *
 * For the resistive-touch "Cheap Yellow Display" boards: ESP32-2432S028 and
 * ESP32-32E 2.8", 3.2", 3.5", 4.0" (XPT2046 touch; panel detected at start-up).
 *
 * A minimal starting point for building an LVGL application from scratch on
 * this hardware.  The UI lives in a tiny ui.h / ui.cpp pair in the sketch
 * folder that draws a simple "Hello, world!" screen.
 *
 * The intent is to hand this to Claude Code (or any developer) as a clean
 * scaffold: the display, touch, and LVGL plumbing is already wired up, so you
 * can focus on writing your own UI in ui.cpp.  Just replace the contents of
 * ui_init() with your own widgets and grow from there.
 *
 * Orientation: lv_setup.begin(rotation) accepts 0 = portrait (240x320, the
 * panel's native orientation), 1 = landscape (320x240), 2 = portrait
 * flipped, 3 = landscape flipped (degrees 90/180/270 work as aliases for
 * 1/2/3).  This sketch defaults to portrait; change the argument below to
 * rotate.  Display and touch rotate together.
 *
 * Demonstrates touchscreen, LVGL and a hand-written UI.
 *
 * Compatible with LVGL 9; the bundled lv_conf.h is based on LVGL 9.3.  If your
 * installed LVGL differs, the API is generally close enough to adapt.
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

// Fonts
// The Montserrat font is built into LVGL in multiple sizes, each size takes up memory.
// We enable the 14 point font as a default font.
#define LV_FONT_MONTSERRAT_14 1
// need other sizes?  You'll know if you see compiler errors after trying to
// use them.  Just edit lv_conf.h where FONTs are turned off (0) and turn them on (1)

// The UI for this example lives in ui.h / ui.cpp in this sketch folder.
// It is a hand-written "Hello, world!" screen meant as a starting point.
// Build your own application by editing ui_init() in ui.cpp.
#include "ui.h"

void setup() {
    Serial.begin(115200);

    // Initialize display, touch, and LVGL.  Defaults to portrait; pass a
    // rotation to change it (0/2 = portrait, 1/3 = landscape; see header).
    lv_setup.begin();
    Serial.printf("LVGL initialized with %dx%d touchscreen\n",
                  display.width(), display.height());

    // Resistive touch varies between panels.  If touch lands in the wrong
    // place, gather raw values with lv_setup.touch().readRaw() and feed the
    // corners into lv_setup.touch().setCalibration(xMin, xMax, yMin, yMax).

    // Start the application's own setup
    ui_init();
}


void loop() {
    // Give loop control to LVGL objects created by the application
    lv_timer_handler();
    delay(5);
}
