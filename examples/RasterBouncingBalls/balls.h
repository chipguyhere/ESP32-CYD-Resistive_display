/*
 * balls.h - Bouncing balls drawn a chunk of rows at a time into the raster
 * task's bounce buffers, not by LVGL.
 *
 * The render task (balls_start()) runs one priority below the raster task:
 * for each frame it advances the physics once, then draws the ball area's
 * chunks in order, each into whichever bounce buffer the raster task has
 * finished sending, and hands it back (see raster.h).
 *
 * Copyright (c) 2026 chipguyhere
 * MIT License
 */

#pragma once

#include <Arduino.h>
#include <stdint.h>

#define BALLS_MAX 100

// The ball area: width x height pixels, from row 0.
void balls_init(int width, int height);

// Start the render task on `core` at `priority`; returns it (for the
// raster task's watchdog).
TaskHandle_t balls_start(int core, int priority);

// For trying the watchdog: make every chunk take this much longer (busy,
// not sleeping), as an overrunning renderer would.  0 = off.
void balls_set_overrun_us(uint32_t us);

// How much time one frame stands for, in microseconds: the physics advances
// by exactly this much per frame, whether or not a frame was late.
void balls_set_frame_us(float us);

// Controls, safe from any task; read once per frame.
void balls_set_count(int n);          // 1..BALLS_MAX
void balls_set_speed(int v);          // 1..100; 100 is the LVGL demo's speed
void balls_set_see_through(bool on);  // 75 % opaque balls

// The longest time drawing one chunk took since the last call, in us.
uint32_t balls_take_max_chunk_us(void);
