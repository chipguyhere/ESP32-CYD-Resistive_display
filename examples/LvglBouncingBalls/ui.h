/*
 * ui.h - Bouncing-balls UI entry point.
 *
 * The sketch calls ui_init() once from setup() after the display, touch and
 * LVGL have been brought up.  Everything else (the balls, the sliders, and the
 * physics) lives in ui.cpp, and is laid out from the live display resolution.
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

// Builds the bouncing-balls UI on the active LVGL screen.  Called once from setup().
void ui_init(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif
