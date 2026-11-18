/*
 * raster.h - The raster task: sends the frame to the panel in step with its
 * scan, so the picture never tears.
 *
 * The same scheme as the RasterBouncingBalls example of
 * chipguy_ESP32-CYD-Resistive_display (whose raster.cpp this is cut down
 * from), with the whole screen as the animated area and no LVGL:
 *   - The RASTER task (high priority, on the SPI interrupt's core) owns the
 *     bus.  For each frame it waits for the moment the scan model says is
 *     right, then sends the frame's chunks from the bounce buffers as the
 *     render task fills them, then reads the scan line (0x45) to correct
 *     the model.
 *   - The RENDER task (scene.cpp, on the other core) draws each chunk into
 *     a free bounce buffer and queues it.  A chunk that isn't ready by the
 *     time it must go out is not waited for: the rest of that frame is
 *     dropped, and those rows keep the previous frame.
 *
 * Differences from the example:
 *   - As many bounce buffers as RAM allows (up to a whole frame), so the
 *     render task can use the time between frames, not only the transfer.
 *   - Chunks may arrive out of order (two render workers): early ones wait
 *     for their turn.
 *   - The frame period is a whole number of refreshes that ADAPTS: when
 *     frames are late it takes one more refresh per frame; when the render
 *     task has plenty of time to spare, one fewer (never fewer than the
 *     transfer needs, nor faster than MAX_FPS).  If frames are still late at
 *     4 refreshes, the per-chunk deadlines are dropped: chunks are waited
 *     for, and the picture may tear, rather than not appear at all.
 *
 * Copyright (c) 2026 chipguyhere
 * MIT License
 */

#pragma once

#include <Arduino.h>
#include "scan.h"

#define CHUNK_ROWS        16     // panel rows per chunk (= landscape columns)
#define RASTER_MAX_BUFS   30     // a whole 480-row frame

struct RasterChunk {
    uint8_t  buf;      // which bounce buffer; RASTER_NO_BUF with an abandonment
    uint16_t chunk;    // which chunk of the frame, or RASTER_ABANDONED
    uint32_t frame;
};
#define RASTER_ABANDONED 0xFFFF
#define RASTER_NO_BUF    0xFF

extern uint16_t        *g_bounce[RASTER_MAX_BUFS];   // DMA-capable, CHUNK_ROWS rows each
extern int              g_nBufs;
extern QueueHandle_t    g_freeQ;     // uint8_t: buffers free to draw into
extern QueueHandle_t    g_readyQ;    // RasterChunk: drawn, big-endian, ready to send
extern volatile int32_t g_abortFrame;    // the raster task gave up on this frame
extern volatile uint32_t g_rasterFrame;  // the frame being sent (or about to be)

// Bounce buffers for a panel `width` wide: as many as fit (up to maxBufs)
// while leaving `keepFree` bytes of internal heap.  Returns how many (0 on
// failure; at least 3 are needed).
int raster_alloc(int width, int maxBufs, size_t keepFree);

// Time sending `rows` full-width rows from the bounce buffers.  Overwrites
// those rows on screen.
bool raster_measure(int width, int rows, double &rowUs, double &windowUs);

struct RasterPlan {
    int    W = 0, H = 0, rows = 0;   // panel width, native height, rows animated
    bool   scanKnown = false;
    bool   tearFree = false;
    int    periods = 0;              // the fewest refreshes per frame the transfer allows
    double rowUs = 0, windowUs = 0;
    double transferUs = 0, spanUs = 0, guardUs = 0, startOffsetUs = 0;
    ScanModel model;
};
void raster_plan(RasterPlan &p);

// Start the raster task.  maxFps caps the adaptive frame rate.
void raster_start(const RasterPlan &p, int core, int priority, float maxFps);

// The time one frame stands for, in microseconds (the render task's step).
float raster_frame_us(void);

// The load has changed (a new scene): go back to the fastest tear-free
// frame rate and adapt again from there.
void raster_retry(int minPeriods);   // and never fewer than this many refreshes a frame
// While the screen is dark (the fade between scenes, where a new scene's
// setup makes a frame or two late), late frames don't count; when it's
// visible again the frame period starts over from the scene's floor.
void raster_quiet(bool quiet);

// The render task's busy time for its last frame, in microseconds, for the
// adaptive frame period.
void raster_report_render_us(uint32_t us);

struct RasterStats {
    uint32_t frames, late, slips;
    float    scanErrRms, hz;
    int      periods;
    bool     relaxed;          // waiting for chunks (may tear)
    uint32_t scanRejected;     // scan readings too slow to use
    uint32_t spanMax;          // the slowest reading, us
};
void raster_take_stats(RasterStats &s);
