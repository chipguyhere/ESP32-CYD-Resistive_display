/*
 * ui.h - LVGL UI for the CYD MP3 player.
 */

#pragma once

#include "lvgl.h"

// Builds the player screen on lv_screen_active().  Call once from setup()
// after lv_setup.begin() and player_begin().
void ui_init(bool sdOk);
