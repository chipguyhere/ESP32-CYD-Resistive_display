/*
 * ui.h - Calculator UI entry point for the LvglCalculator example.
 *
 * The sketch calls ui_init() once from setup() after the display, touch and
 * LVGL have been brought up.  Everything else (layout, styling, and the
 * calculator logic) lives in ui.cpp.
 */

#pragma once

#if defined __has_include
  #if __has_include("lvgl.h")
    #include "lvgl.h"
  #elif __has_include("lvgl/lvgl.h")
    #include "lvgl/lvgl.h"
  #else
    #include "lvgl.h"
  #endif
#else
  #include "lvgl.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Builds the calculator UI on the active LVGL screen.  Called once from setup().
void ui_init(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif
