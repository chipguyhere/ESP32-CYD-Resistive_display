/*
 * scene.h - The two speedometers, drawn a chunk at a time into the raster
 * task's bounce buffers:
 *   0 "old"  what a typical TFT_eSPI sketch shows: the 5 x 7 font, one-pixel
 *            lines, a needle and a number that are erased and redrawn where
 *            you can see it happen.
 *   1 "new"  the raster speedometer: chrome bezel, smoked-glass face with a
 *            warp starfield behind it, a lit arc and sector that follow the
 *            needle, numbers that light up and grow, a needle with a shadow
 *            and glow, a glass reflection, and a digital read-out.
 *
 * The picture is landscape -- 320 x 240 on a 240 x 320 panel, 480 x 320 on
 * a 320 x 480 one -- but the panel scans in its native portrait order: each
 * 16-row chunk of the panel is 16 COLUMNS of the landscape picture.
 *
 * Pure C++ (no Arduino, no FreeRTOS), so tools/host_preview.cpp can build it
 * on a computer and render frames to image files.
 *
 * Copyright (c) 2026 chipguyhere
 * MIT License
 */

#pragma once

#include <stdint.h>

#define SCENE_MAX_W  480          // landscape width  = panel rows used
#define SCENE_MAX_H  320          // landscape height = panel width
#define SCENE_CHUNK  16           // landscape columns per chunk (= CHUNK_ROWS)
#define SCENE_WORKERS 2           // tasks that may draw chunks at the same time

// Whether a panel `panelW` wide (native, portrait) is one there are assets
// for: 240 (a 320 x 240 picture) or 320 (480 x 320).
bool scene_supports(int panelW, int panelH);

// flip: false shows the picture turned a quarter-turn clockwise from the
// panel's portrait (its top along the portrait right edge, the library's
// rotation 1); true the other way (rotation 3).
void scene_init(int panelW, bool flip);

int scene_width(void);                  // landscape: 320 or 480
int scene_height(void);                 // 240 or 320
int scene_chunks(void);                 // scene_width() / SCENE_CHUNK

// Advance by dt seconds (one frame): scene_step(), or, to share the work
// between two tasks, scene_step_begin() and then scene_step_main() and
// scene_step_helper() at the same time (both done before any drawing).
void scene_step(float dt);
void scene_step_begin(float dt);
void scene_step_main(void);
void scene_step_helper(void);

int scene_min_periods(void);            // fewest panel refreshes a frame: 2

// Draw chunk c (0 .. scene_chunks()-1, in panel order) into dst: SCENE_CHUNK
// panel rows of scene_height() pixels, big-endian RGB565, ready to send.
// worker (0 .. SCENE_WORKERS-1): which task; two may draw at once, but not
// while a step runs.
void scene_render_chunk(int c, uint16_t *dst, int worker);

float scene_brightness(void);           // the fade, 0..1, for the backlight

// One panel row of the notice shown when the board has no speed-test
// record (native little-endian RGB565, for flushRegion()).  Usable before
// scene_init().
void scene_notice_row(int panelW, bool flip, int r, uint16_t *row);

int  scene_phase(void);                 // 0 old, 1 new
const char *scene_phase_name(void);
void scene_next_phase(void);            // fade out now and go on (Serial 'n')
float scene_speed_mph(void);            // for the status line

// Touch control.  scene_hold_speed(): the raster speedometer shows this speed
// (0 .. scene_hold_max(), clamped; it eases there) with everything that goes
// with it -- lights, numbers, stars, glow, the red-zone pulse, the warp
// flash near the top -- and the run stands still at the moment it would
// reach that speed.  Called during the old speedometer, it switches over at
// once.  scene_hold_lifted(): the finger has left the screen (the glass
// glints a few seconds later, not while the finger moves).  scene_resume():
// the run carries on from that moment.  All may be called from another
// task; they take effect at the next step.
void scene_hold_speed(float mph);
void scene_hold_lifted(void);
void scene_resume(void);
bool scene_holding(void);
float scene_hold_max(void);             // 225

// CPU cycles spent since the last call, all workers, by stage.
enum { SCENE_PROF_BG, SCENE_PROF_STARS, SCENE_PROF_DIAL, SCENE_PROF_LIGHT,
       SCENE_PROF_NEEDLE, SCENE_PROF_TEXT, SCENE_PROF_STEP, SCENE_PROF_N };
void scene_take_profile(uint32_t out[SCENE_PROF_N]);
