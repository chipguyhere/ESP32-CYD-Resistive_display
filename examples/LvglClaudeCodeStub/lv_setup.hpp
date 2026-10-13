/*
 * lv_setup.hpp - The bridge between LVGL and your display + touch hardware.
 *
 * THIS is the file that wires LVGL up to a specific board.  Right now it just
 * pulls in the ready-made setup that ships inside the
 * chipguy_ESP32-CYD-Resistive_display library, so you don't have to maintain a copy:
 *
 *   - It creates a global `display`     (the ILI9341 / ST7789 driver)
 *   - It creates a global `lv_setup`    (an lv_setup_class instance)
 *   - Calling lv_setup.begin(rotation)  initializes display + touch + LVGL
 *
 * --------------------------------------------------------------------------
 *  TARGETING A DIFFERENT BOARD
 *  Point the #include below at a different library's setup header and the same
 *  sketch (ui.cpp / ui.h, etc.) drives a different board — as long as that
 *  header exposes the same `display` / `lv_setup` shape.
 *
 *  CUSTOMIZING HOW LVGL IS WIRED UP
 *  Because this file lives in your sketch folder, it's the easy place to take
 *  control.  Open the real implementation at:
 *
 *      <Arduino libraries>/chipguy_ESP32-CYD-Resistive_display/src/lv_setup_CYD_resistive.hpp
 *
 *  copy its entire contents over the #include line below, and edit freely —
 *  buffer size, color format, rotation handling, the touch read callback, etc.
 *  Nothing else in the sketch needs to change.
 * --------------------------------------------------------------------------
 *
 * Include this file after lv_conf.h and lvgl.h in your sketch.
 *
 *
 * For reference, the setup class this pulls in looks like:
 *
 *   // A global instance named `lv_setup` is created for you.
 *   class lv_setup_class {
 *   public:
 *       // Initialize display, touch, and LVGL.  Call once from setup().
 *       // rotation: 0 = portrait (240x320), 1 = landscape (320x240),
 *       //           2 = portrait flipped, 3 = landscape flipped.
 *       //           Degrees (0/90/180/270) work as aliases for 0/1/2/3.
 *       void begin(uint16_t rotation = 0);
 *
 *       // The touch driver, e.g. to apply calibration from setup():
 *       //   lv_setup.touch().setCalibration(xMin, xMax, yMin, yMax);
 *       chipguy_CYD_resistive_touch &touch();
 *   };
 *
 *   // Also provided as a global: the display driver, handy for resolution:
 *   //   display.width(), display.height()
 *   extern chipguy_CYD_resistive_display display;
 *
 * Tip: resistive touch varies between panels.  If touch feels off, flash the
 * bundled TouchCalibrate example once; begin() loads the saved calibration
 * automatically.
 */

#pragma once

#include <lv_setup_CYD_resistive.hpp>
