/*
 * ui.h - Application UI entry points for the LvglClaudeCodeStub example.
 *
 * This is a hand-written stub.  It declares the minimal entry points the
 * sketch calls.  Build your own LVGL application by editing ui.cpp, adding
 * more screens/widgets and declaring their helpers here as needed.
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

// Builds the initial UI.  Called once from the sketch's setup().
void ui_init(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif
