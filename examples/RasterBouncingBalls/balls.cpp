/*
 * balls.cpp - Bouncing balls rasterized into the bounce buffers, a chunk of
 * rows at a time.
 *
 * Adapted from the RasterBouncingBalls example of chipguy_JC8012P4A1C_display
 * (and that from the chipguy_Waveshare-ESP32-S3-Touch-LCD-7-7B_display one).
 * The drawing is the same; what differs:
 *   - One render task instead of a raster callback split across two cores:
 *     the SPI panel takes its pixels at the ESP32's pace, so a chunk only
 *     has to be ready before the raster task wants it, and the render task
 *     runs ahead into the free bounce buffers.
 *   - Each chunk is byte-swapped after drawing: the SPI panels take RGB565
 *     big-endian, and the blending is done on native (little-endian) words.
 *   - The physics runs at the start of each frame, in the render task.
 *
 *   - Span ends are anti-aliased from the circle's exact half-width, looked
 *     up in a table, so no square roots are taken while drawing.
 *   - The background gradient is ordered-dithered (4x4 Bayer).
 *   - See-through balls are 75 % opaque: 3/4 of the ball's colour, worked out
 *     once per frame, plus 1/4 of what's under it -- one shift, mask and add
 *     per two pixels.
 *
 * Copyright (c) 2026 chipguyhere
 * MIT License
 */

// The drawing is the hot path; the core builds at -Os.
#pragma GCC optimize ("O2")

#include "balls.h"
#include "raster.h"
#include <Arduino.h>
#include <math.h>
#include <esp_random.h>
#include <esp_timer.h>

#define MAX_D   40          // largest diameter the half-width table covers
#define MAX_W   480         // widest ball area supported
#define MAX_H   480

struct Ball {
    float x, y;     // top-left, pixels
    float vx, vy;   // pixels per 16 ms
    int   d;        // diameter
    uint16_t color; // RGB565
};

// What a chunk needs to draw one ball, fixed for the frame.
struct DrawBall {
    uint32_t c2;    // colour, two pixels
    uint32_t c34;   // 3/4 of the colour, two pixels (see quarter())
    int32_t  cx8;   // centre x, 1/256 pixel
    int16_t  top;   // first row
    int16_t  d;     // diameter (rows, and the half-width table to use)
    uint32_t fg;    // colour, spread for blending (see blend())
};

struct Snapshot {
    DrawBall b[BALLS_MAX];
    int      n;
    bool     seeThrough;
};

static Ball      s_balls[BALLS_MAX];
static Snapshot  s_snap;
static uint16_t  s_hw[MAX_D + 1][MAX_D];   // half-width per diameter and row, 1/256 px
static uint32_t  s_bg[MAX_H][2];           // background per row: the 4-pixel dither pattern

static int       s_W = 0, s_H = 0;
static float     s_gravity = 0;
static int       s_minD = 0, s_maxD = 0;
static volatile float    s_stepTicks = 1;  // one frame, in 16 ms ticks
static volatile int      s_count = 30;
static volatile int      s_speed = 20;
static volatile bool     s_seeThrough = true;
static volatile uint32_t s_maxChunkUs = 0;
static volatile uint32_t s_overrunUs = 0;
void balls_set_overrun_us(uint32_t us) { s_overrunUs = us; }

void balls_set_count(int n) { s_count = n < 1 ? 1 : n > BALLS_MAX ? BALLS_MAX : n; }
void balls_set_speed(int v) { s_speed = v; }
void balls_set_see_through(bool on) { s_seeThrough = on; }
void balls_set_frame_us(float us) { s_stepTicks = us / 16000.0f; }
uint32_t balls_take_max_chunk_us(void) { uint32_t v = s_maxChunkUs; s_maxChunkUs = 0; return v; }

/* ----------------------------------------------------------------- setup */

static inline float frand(float a, float b) { return a + (esp_random() / 4294967296.0f) * (b - a); }
static inline int irand(int a, int b) { return a + (int)(esp_random() % (uint32_t)(b - a + 1)); }

static inline uint16_t rgb565(int r, int g, int b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Upward speed that tops out between 30 % and 100 % of the area's height.
static float launch_speed(void) {
    return sqrtf(2.0f * s_gravity * frand(0.30f, 1.0f) * (float)s_H);
}

static void init_ball(Ball *b) {
    b->d  = irand(s_minD, s_maxD);
    b->x  = frand(0, (float)(s_W - b->d));
    b->y  = frand(0, (float)(s_H - b->d));
    b->vx = frand(s_W * 0.003f, s_W * 0.010f) * ((esp_random() & 1) ? 1.0f : -1.0f);
    b->vy = -launch_speed();
    b->color = rgb565(64 + esp_random() % 192, 64 + esp_random() % 192, 64 + esp_random() % 192);
}

// A quarter of each RGB565 field, two pixels per word: shift right two, and
// clear the bits that fell in from the field above.  c - quarter(c) is three
// quarters, and 3/4 a + 1/4 b never overflows a field.
#define QUARTER_MASK 0x39E739E7u
static inline uint32_t quarter(uint32_t c) { return (c >> 2) & QUARTER_MASK; }

void balls_init(int width, int height) {
    s_W = width > MAX_W ? MAX_W : width;
    s_H = height > MAX_H ? MAX_H : height;
    // The same physics as the LVGL BouncingBalls demo, whose units are per
    // 16 ms tick.
    s_gravity = s_H * 0.0010f;
    int base = s_W < s_H ? s_W : s_H;
    s_minD = base / 16; if (s_minD < 10) s_minD = 10;
    s_maxD = base / 9;  if (s_maxD > MAX_D) s_maxD = MAX_D;
    if (s_maxD < s_minD + 2) s_maxD = s_minD + 2;

    for (int d = 1; d <= MAX_D; d++) {
        float r = d * 0.5f;
        for (int row = 0; row < d; row++) {
            float dy = row + 0.5f - r;
            float h2 = r * r - dy * dy;
            s_hw[d][row] = h2 > 0 ? (uint16_t)(sqrtf(h2) * 256.0f + 0.5f) : 0;
        }
    }
    // A vertical gradient, dark blue at the top to near black at the floor,
    // ordered-dithered so its 565 steps don't show as bands.
    static const uint8_t bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 },
                                         { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
    for (int y = 0; y < s_H; y++) {
        float t = (float)y / (float)(s_H - 1);
        float r = 5.0f - 3.5f * t, g = 9.0f - 6.0f * t, b = 17.0f - 13.0f * t;
        uint16_t px[4];
        for (int x = 0; x < 4; x++) {
            float th = (bayer[y & 3][x] + 0.5f) / 16.0f;
            int r5 = (int)(r + th), g6 = (int)(g + th), b5 = (int)(b + th);
            px[x] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
        }
        s_bg[y][0] = px[0] | ((uint32_t)px[1] << 16);
        s_bg[y][1] = px[2] | ((uint32_t)px[3] << 16);
    }
    for (int i = 0; i < BALLS_MAX; i++) init_ball(&s_balls[i]);
}

/* ----------------------------------------------------------------- physics */

// Advance one frame -- always exactly one frame's worth of time, so a late
// or dropped frame can't make the motion jump -- and snapshot what to draw.
static void step_into(Snapshot *out) {
    const int n = s_count;
    const float s = s_stepTicks * s_speed * 0.01f;
    for (int i = 0; i < n; i++) {
        Ball *b = &s_balls[i];
        b->vy += s_gravity * s;
        b->x  += b->vx * s;
        b->y  += b->vy * s;
        if (b->x < 0)                { b->x = 0;              b->vx = -b->vx; }
        if (b->x + b->d > s_W)       { b->x = s_W - b->d;     b->vx = -b->vx; }
        if (b->y < 0)                { b->y = 0; if (b->vy < 0) b->vy = -b->vy; }
        if (b->y + b->d > s_H)       { b->y = s_H - b->d;     b->vy = -launch_speed(); }

        DrawBall *o = &out->b[i];
        uint32_t c2 = b->color | ((uint32_t)b->color << 16);
        o->c2  = c2;
        o->c34 = c2 - quarter(c2);
        o->cx8 = (int32_t)((b->x + b->d * 0.5f) * 256.0f);
        o->top = (int16_t)(b->y + 0.5f);
        o->d   = (int16_t)b->d;
        o->fg  = c2 & 0x07E0F81F;
    }
    out->n = n;
    out->seeThrough = s_seeThrough;
}

/* ----------------------------------------------------------------- drawing */

// fg (already spread) over bg at alpha a/32.
static inline uint16_t blend(uint32_t fg, uint16_t bg, uint32_t a) {
    uint32_t b = (bg | ((uint32_t)bg << 16)) & 0x07E0F81F;
    uint32_t r = ((((fg - b) * a) >> 5) + b) & 0x07E0F81F;
    return (uint16_t)(r | (r >> 16));
}

static inline uint32_t mix3(const DrawBall &ball, uint32_t b) { return ball.c34 + quarter(b); }

// The solid inside of a span, pixels [x, xe).
static inline void inside(uint16_t *row, int x, int xe, const DrawBall &b, bool see) {
    const uint32_t c2 = b.c2;
    uint16_t *p = row + x;
    int n = xe - x;
    if (n <= 0) return;
    if ((uintptr_t)p & 2) { *p = (uint16_t)(see ? mix3(b, *p) : c2); p++; n--; }
    uint32_t *q = (uint32_t *)p;
    int w = n >> 1;
    if (see) { for (int i = 0; i < w; i++) q[i] = mix3(b, q[i]); }
    else     { for (int i = 0; i < w; i++) q[i] = c2; }
    if (n & 1) { uint16_t *t = (uint16_t *)(q + w); *t = (uint16_t)(see ? mix3(b, *t) : c2); }
}

// One anti-aliased span, [l8, r8) in 1/256 pixel.
static inline void span(uint16_t *row, int32_t l8, int32_t r8, const DrawBall &b, bool see) {
    const int32_t lim = s_W << 8;
    if (l8 < 0) l8 = 0;
    if (r8 > lim) r8 = lim;
    if (r8 <= l8) return;
    const uint32_t k = see ? 3 : 4;                   // coverage scale, in quarters
    int xl = l8 >> 8, xr = r8 >> 8;
    if (xl == xr) {
        row[xl] = blend(b.fg, row[xl], ((uint32_t)(r8 - l8) * k) >> 5);
        return;
    }
    row[xl] = blend(b.fg, row[xl], ((256 - (l8 & 255)) * k) >> 5);
    if (r8 & 255) row[xr] = blend(b.fg, row[xr], ((uint32_t)(r8 & 255) * k) >> 5);
    inside(row, xl + 1, xr, b, see);
}

// Draw rows y .. y+rows-1 into dst, then byte-swap them for the panel.
static void draw_rows(const Snapshot *sn, uint16_t *dst, int y, int rows) {
    const int W = s_W;
    uint8_t sel[BALLS_MAX];
    int ns = 0;
    for (int i = 0; i < sn->n; i++) {
        const DrawBall &b = sn->b[i];
        if (b.top < y + rows && b.top + b.d > y) sel[ns++] = (uint8_t)i;
    }
    const bool see = sn->seeThrough;
    for (int r = 0; r < rows; r++) {
        const int py = y + r;
        uint16_t *row = dst + (size_t)r * W;
        uint32_t *q = (uint32_t *)row;                 // rows are 4-byte aligned
        const uint32_t w0 = py < s_H ? s_bg[py][0] : 0, w1 = py < s_H ? s_bg[py][1] : 0;
        for (int k = 0; k < W / 2; k += 2) { q[k] = w0; q[k + 1] = w1; }
        if (py >= s_H) continue;
        for (int k = 0; k < ns; k++) {                // balls, in order: later on top
            const DrawBall &b = sn->b[sel[k]];
            int br = py - b.top;
            if ((unsigned)br >= (unsigned)b.d) continue;
            int32_t h = s_hw[b.d][br];
            span(row, b.cx8 - h, b.cx8 + h, b, see);
        }
    }
    // Big-endian for the panel, two pixels a word.
    uint32_t *q = (uint32_t *)dst;
    for (int i = 0, n = rows * W / 2; i < n; i++) {
        uint32_t v = q[i];
        q[i] = ((v >> 8) & 0x00FF00FFu) | ((v << 8) & 0xFF00FF00u);
    }
}

/* ----------------------------------------------------------------- render task */

// Tell the raster task the rest of this frame is given up.
static void report_abandoned(uint32_t frame) {
    RasterChunk rc = {RASTER_NO_BUF, RASTER_ABANDONED, frame};
    xQueueSend(g_readyQ, &rc, portMAX_DELAY);
}

static void render_task(void *) {
    const int chunks = s_H / CHUNK_ROWS;
    // Given up on: dropped by the raster task, or already behind it.
    auto given_up = [](uint32_t frame) {
        return g_abortFrame == (int32_t)frame || (int32_t)(g_rasterFrame - frame) > 0;
    };
    for (uint32_t frame = 0;; frame++) {
        // Fallen behind the raster task (after an overrun): skip to its frame.
        uint32_t rf = g_rasterFrame;
        if ((int32_t)(rf - frame) > 0) frame = rf;
        step_into(&s_snap);
        for (int c = 0; c < chunks; c++) {
            if (given_up(frame)) { report_abandoned(frame); break; }
            uint8_t b;
            xQueueReceive(g_freeQ, &b, portMAX_DELAY);
            if (given_up(frame)) {
                xQueueSendToFront(g_freeQ, &b, 0);
                report_abandoned(frame);
                break;
            }
            int64_t t = esp_timer_get_time();
            draw_rows(&s_snap, g_bounce[b], c * CHUNK_ROWS, CHUNK_ROWS);
            if (s_overrunUs) while (esp_timer_get_time() - t < (int64_t)s_overrunUs) {}
            uint32_t us = (uint32_t)(esp_timer_get_time() - t);
            if (us > s_maxChunkUs) s_maxChunkUs = us;
            RasterChunk rc = {b, (uint16_t)c, frame};
            xQueueSend(g_readyQ, &rc, portMAX_DELAY);
        }
    }
}

TaskHandle_t balls_start(int core, int priority) {
    TaskHandle_t h = nullptr;
    xTaskCreatePinnedToCore(render_task, "balls", 4096, nullptr, priority, &h, core);
    return h;
}
