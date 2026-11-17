/*
 * raster.h - The raster task: sends the ball area to the panel in step with
 * its scan, and LVGL's strip in the time left over.
 *
 * Three tasks share the work:
 *   - The RASTER task (highest priority, on the SPI interrupt's core) owns
 *     the bus.  For each frame it waits for the moment the scan model says
 *     is right, then sends the ball area's chunks from the bounce buffers as
 *     the render task fills them.  After the frame it reads the scan line
 *     (0x45) to correct the model, then uses what is left of the frame for
 *     LVGL's pending rows and a touch sample (on boards whose touch shares
 *     the bus).
 *   - The RENDER task (one priority lower; balls.cpp) draws each chunk into a
 *     free bounce buffer and queues it.  A chunk that isn't ready by the
 *     time it must go out is not waited for: the rest of that frame is
 *     dropped, and those rows keep showing the previous frame's pixels.
 *   - LVGL (loop()) renders the control strip.  Its flush hands the rows to
 *     the raster task and waits until they have all gone out, which may
 *     take several frames: LVGL is subordinate to the balls.
 *
 * Copyright (c) 2026 chipguyhere
 * MIT License
 */

#pragma once

#include <Arduino.h>
#include "scan.h"

// Rows per chunk, and the number of chunk-sized bounce buffers: see config.h.
#include "config.h"

// The render task <-> raster task protocol.  The render task reports every
// chunk it is asked for, one way or the other: drawn (a buffer to send), or
// abandoned (it saw g_abortFrame and gave the rest of the frame up).  The
// raster task's watchdog counts on those reports.
struct RasterChunk {
    uint8_t  buf;      // which bounce buffer; RASTER_NO_BUF with an abandonment
    uint16_t chunk;    // which chunk of the frame, or RASTER_ABANDONED
    uint32_t frame;    // which frame
};
#define RASTER_ABANDONED 0xFFFF
#define RASTER_NO_BUF    0xFF
extern uint16_t        *g_bounce[RASTER_BUFS];   // DMA-capable, CHUNK_ROWS rows each
extern QueueHandle_t    g_freeQ;     // uint8_t: buffers free to draw into
extern QueueHandle_t    g_readyQ;    // RasterChunk: drawn, big-endian, ready to send
extern volatile int32_t g_abortFrame;   // the raster task gave up on this frame
// The frame the raster task is sending (or about to).  A render task behind
// it abandons what it has and jumps to it, so the two can't drift apart.
extern volatile uint32_t g_rasterFrame;

// Bounce buffers and queues for a panel `width` wide.
bool raster_alloc(int width);

// Time sending `rows` full-width rows from the bounce buffers, as the raster
// task will (window command, then chunk DMAs).  Overwrites those rows on
// screen.  Call with LVGL idle.
bool raster_measure(int width, int rows, double &rowUs, double &windowUs);

// What the measurements add up to.
struct RasterPlan {
    int    W = 0, H = 0, ballRows = 0;
    bool   scanKnown = false;  // 0x45 readable and the model fitted
    bool   tearFree = false;   // the ball area can be sent between two scans of each row
    int    periods = 0;        // refreshes per frame when tearFree (1 or 2)
    bool   stripInStep = false;// rows sent right after the balls stay in step too
    double rowUs = 0, windowUs = 0;  // measured transfer
    double transferUs = 0;     // the whole ball area
    double spanUs = 0;         // how far the write drifts against the scan over the area
    double guardUs = 0;        // timing margin kept each side
    double startOffsetUs = 0;  // start this long after the scan reaches line 0
    // Half rate, paced (where periods is 1): chunks spread over most of the
    // window a frame may take without tearing, rather than sent as fast as
    // the bus goes.  0 if pacing gives nothing (the bus is already slower).
    double pacedRowUs = 0;     // one row's share of the paced frame
    double pacedStartOffsetUs = 0;
    ScanModel model;
};
void raster_plan(RasterPlan &p);

// Start the raster task with that plan.
void raster_start(const RasterPlan &p, int core, int priority);

// The watchdog on the render task.  The raster task expects the render task
// to report each chunk in time.  If it goes 250 ms without a report while a
// frame is waiting, or 20 frames in a row are late, the render task is taken
// to be running overtime at a priority above LVGL's and starving it.  The
// raster task then switches the render task's priority above and below
// LVGL's every 0.3 s, so the balls and the controls freeze by turns -- an
// unmistakable symptom -- and says so on Serial every 5 s.  60 good frames
// in a row restore the normal priority.
//   render:      the render task; normalPrio its priority
//   lvglPrio:    the priority of the task that runs LVGL (loop()'s)
void raster_watch(TaskHandle_t render, UBaseType_t normalPrio, UBaseType_t lvglPrio);
// On by default.  raster_watchdog(false): leave the render task alone.
void raster_watchdog(bool on);

// Half rate.  Where the plan sends one frame per refresh, a sketch can drop to
// one frame every two refreshes, still tear-free, and come back.  At half
// rate the chunks are paced: sent evenly over most of the time a frame may
// take, so the render task has that long to compose them, spread evenly.
// Not the full two refreshes: every row must land between two passes of the
// scan over it, so a frame can take at most one refresh plus the scan's
// time across the ball rows (about 1.6 refreshes for 240 rows of 320).  The
// pacing uses 90 % of that; the rest goes to LVGL and the scan reading.  Takes
// effect from the next frame; the physics follows.  False (and no change) if
// the plan can't do one frame per refresh, or isn't timed to the scan at all.
bool raster_set_half_rate(bool on);
// Refreshes per frame now: 1 or 2; 0 when not timed to the scan.
int raster_periods();
// Off by default.  On: drop to half rate by itself if 3 frames come out late,
// or miss their start, within 60 frames.  It doesn't come back by itself: call
// raster_set_half_rate(false) for that.  (The watchdog trips at 20 late in a
// row; this acts well before.)
void raster_auto_half_rate(bool on);

// Send rows y1..y2 (full width, big-endian RGB565) through the raster task,
// and wait until they're out.  The LVGL flush calls this.
void raster_send_rows(const uint16_t *px, int y1, int y2);

struct RasterStats {
    uint32_t frames;       // frames sent whole
    uint32_t late;         // frames dropped part-way (a chunk wasn't drawn in time)
    uint32_t slips;        // frames whose start was missed (sent a refresh later)
    uint32_t touchSamples; // opportunistic touch samples taken
    float    scanErrRms;   // scan readings against the model, lines
    float    hz;           // the model's refresh rate
    uint32_t lvglRows;     // LVGL rows sent
    uint32_t lvglForced;   // gaps where LVGL, starved, was sent rows past the deadline
    uint32_t scanSkips;    // scan readings skipped to give LVGL the time
};
// Since the last call (counts), or now (rates).
void raster_take_stats(RasterStats &s);
