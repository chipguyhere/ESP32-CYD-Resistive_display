/*
 * scene.h - The three scenes (fire with smoke, a thunderstorm at sunset,
 * snowfall), drawn a chunk at a time into the raster task's bounce buffers.
 *
 * The picture is landscape -- 320 x 240 on a 240 x 320 panel, 480 x 320 on
 * a 320 x 480 one -- but the panel scans in its native portrait order: each
 * 16-row chunk of the panel is 16 COLUMNS of the landscape picture.  So a
 * chunk is drawn column-major into a 16-column tile in landscape coordinates
 * and turned into panel rows on the way out.
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

#define SCENE_PHASE_SECONDS 10.0f
#define SCENE_FADE_SECONDS  1.0f

// Whether a panel `panelW` wide (native, portrait) is one the scenes have
// backdrops for: 240 (a 320 x 240 picture) or 320 (480 x 320).
bool scene_supports(int panelW, int panelH);

// panelW: the panel's native width (240 or 320); the picture is
// (panelW * 4 / 3, or 480) x panelW.
// flip: false shows the picture turned a quarter-turn clockwise from the
// panel's portrait (its top along the portrait right edge, the library's
// rotation 1); true the other way (rotation 3).
// Allocates its tables (about 36 KB on the larger panels) from the heap.
void scene_init(int panelW, bool flip);
// Optional: a buffer of scene_sky_bytes() to hold the current backdrop in
// RAM (faster than drawing it from flash).
unsigned scene_sky_bytes(int panelW);
void scene_use_sky_ram(uint16_t *buf);

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

// The fewest panel refreshes a frame the current scene wants: 2, or 3 for
// one with more to draw (the snow's zoom).
int scene_min_periods(void);

// Draw chunk c (0 .. scene_chunks()-1, in panel order) into dst: SCENE_CHUNK
// panel rows of scene_height() pixels, big-endian RGB565, ready to send.
// worker (0 .. SCENE_WORKERS-1): which task; two may draw at once, but not
// while scene_step() runs.
void scene_render_chunk(int c, uint16_t *dst, int worker);

// The fade, 0..1, for the backlight.
float scene_brightness(void);

// One panel row (0 .. picture width - 1) of the notice shown when the board
// has no speed-test record, for flushRegion(); usable before scene_init().
void scene_notice_row(int panelW, bool flip, int r, uint16_t *row);

int  scene_phase(void);                 // 0 fire, 1 storm, 2 snow
const char *scene_phase_name(void);
void scene_next_phase(void);            // fade out now and go on (Serial 'n')

// CPU cycles spent since the last call, all workers, by stage.
enum { SCENE_PROF_SKY, SCENE_PROF_GROUND, SCENE_PROF_OVER, SCENE_PROF_STEP,
       SCENE_PROF_LOGS, SCENE_PROF_PUFFS, SCENE_PROF_FLAMES,   // parts of OVER, fire
       SCENE_PROF_SKYCOPY, SCENE_PROF_CLOUDSUN, SCENE_PROF_MOUNTAINS,   // parts of SKY
       SCENE_PROF_N };
void scene_take_profile(uint32_t out[SCENE_PROF_N]);
