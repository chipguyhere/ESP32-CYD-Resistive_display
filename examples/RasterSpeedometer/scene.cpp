/*
 * scene.cpp - The old way and the raster way of drawing a speedometer.  See
 * scene.h.
 *
 * Every frame is drawn whole, a column at a time, into a small per-worker
 * column buffer in native byte order, then written out byte-swapped into the
 * bounce buffer.  So nothing is ever "erased": each frame is simply the
 * picture as it should look at that moment.  The old way's flicker and
 * top-to-bottom painting are imitated on purpose -- they're what a sketch
 * that draws straight onto the screen looks like.
 *
 * The raster speedometer, back to front, per column:
 *   background  black space with a blue glow round the vanishing point that
 *               strengthens with speed: a separable Gaussian, so a pixel is
 *               one look-up in a 256-entry colour table built each frame
 *               (four of them, for a 2 x 2 dither)
 *   stars       points fixed in 3D space that the camera flies through;
 *               each is drawn as a streak from where it was a "shutter time"
 *               ago to where it is, so they stretch into warp lines as the
 *               speed climbs.  Anti-aliased with a tent filter.
 *   dial        pre-rendered (assets.h): premultiplied colour plus how much
 *               of what's behind shows through -- the smoked-glass face lets
 *               the stars through, dimmed
 *   light       the LED arc, its glow, and the sector of the face behind
 *               the needle: per pixel an angle and a role from assets.h,
 *               lit by two 1024-entry tables built each frame (the needle's
 *               position, a bright head, a glossy reflection that slides
 *               along), added on
 *   numbers     0..200, stored 1.4x size, drawn scaled (bilinear) -- they
 *               light up and swell as the speed reaches them
 *   needle      analytic: a tapered blade with a bevel, a glow and a soft
 *               shadow, rasterized per column over just the rows it covers
 *   hub         a chrome cap (assets.h)
 *   read-out    big digits with a glow, km/h underneath
 *   glass       a fixed reflection across the top-left of the glass, and a
 *               glint that sweeps across at each gear change
 *
 * Copyright (c) 2026 chipguyhere
 * MIT License
 */

#pragma GCC optimize ("O2")

#include "scene.h"
#include "assets.h"
#include "tft_espi_glcdfont.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#if defined(ARDUINO)
#include <esp_attr.h>
#include <esp_cpu.h>
#define HOT IRAM_ATTR
#define CYC() esp_cpu_get_cycle_count()
#else
#define HOT
#define CYC() 0u
#endif

static volatile uint32_t s_prof[SCENE_WORKERS][SCENE_PROF_N];

/* ================================================================ colour */

static uint8_t BAYER2[2][2] = { { 0, 2 }, { 3, 1 } };

static inline int clampi(int v, int a, int b) { return v < a ? a : v > b ? b : v; }
static inline float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static inline float smooth(float e0, float e1, float x) {
    float t = clampf((x - e0) / (e1 - e0), 0, 1);
    return t * t * (3 - 2 * t);
}
static inline uint16_t pack(int r, int g, int b) {
    r = clampi(r, 0, 255); g = clampi(g, 0, 255); b = clampi(b, 0, 255);
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
// RGB565 "spread" into a 32-bit word with room above each field.
#define SMASK 0x07E0F81Fu
static inline uint32_t spread(uint16_t c) { return (c | ((uint32_t)c << 16)) & SMASK; }
static inline uint16_t unspread(uint32_t s) { return (uint16_t)(s | (s >> 16)); }
static inline uint32_t spreadRGB(int r, int g, int b) { return spread(pack(r, g, b)); }
static inline uint16_t blend(uint16_t bg, uint32_t fgS, uint32_t a) {     // a 0..32
    uint32_t b = spread(bg);
    return unspread(((((fgS - b) * a + 0x02008010u) >> 5) + b) & SMASK);
}
static inline uint32_t sat(uint32_t s) {
    uint32_t oBR = s & 0x00010020u, oG = s & 0x08000000u;
    return (s | (oBR - (oBR >> 5)) | (oG - (oG >> 6))) & SMASK;
}
static inline uint32_t scaleS(uint32_t s, uint32_t a) { return ((s * a) >> 5) & SMASK; }   // a 0..32
static inline uint16_t addS(uint16_t c, uint32_t s) { return unspread(sat(spread(c) + s)); }
static inline uint16_t SW(uint16_t v) { return __builtin_bswap16(v); }

static uint32_t s_rng = 0x2545F491u;
static inline uint32_t rnd() { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng; }
static inline float frand(float a, float b) { return a + (rnd() >> 8) * (1.0f / 16777216.0f) * (b - a); }

/* ================================================================ layout */

static const SpeedoAsset *A = &ASSET;
static int   LW = 320, LH = 240, CX = 160, CY = 120;
static float R = 113;
static float K = 1;                 // 1, or 4/3 on the larger panels
static bool  s_flip = false;
static int   s_dir = -1;

static __thread uint16_t *s_dst;
static __thread int       s_x0;
static uint16_t s_col[SCENE_WORKERS][SCENE_MAX_H];   // the column being composed, native order

static inline uint16_t *colp(int x) {
    int xi = x - s_x0;
    return s_flip ? s_dst + (SCENE_CHUNK - 1 - xi) * LH : s_dst + xi * LH + (LH - 1);
}

/* ================================================================ timeline */

#define OLD_SECONDS   12.5f
#define OLD_VMAX      120
#define NEW_FADE_IN   1.0f
#define LAUNCH        1.6f
#define SHIFT_S       0.30f
#define NGEARS        6
static const float GEAR_V[NGEARS + 1] = { 0, 48, 88, 124, 156, 184, 216 };
static const float GEAR_S[NGEARS]     = { 2.1f, 2.2f, 2.4f, 2.6f, 2.7f, 2.6f };
static float NEW_END_CLIMB;          // when the last gear tops out
static float FLASH_AT, NEW_SECONDS;

static int   s_phase = 0;
static float s_pt = 0;               // seconds into the phase
static float s_dt = 0;
static float s_clock = 0;            // seconds, never paused: what pulses and buzzes runs on it
static volatile bool s_skip = false;
static const char *NAMES[2] = { "old", "new" };

// Touch: the speed is the user's, and the timeline stands at the moment the
// run would reach that speed (scene_hold_speed()), until scene_resume().
// 225: the warp flash is full from about 224.7, and from 225.6 the run's
// fade-out would begin.
#define HOLD_MAX     225.0f
#define HOLD_SMOOTH  0.10f           // seconds: takes the jitter out of a resistive touch
#define GLINT_GAP    4.0f            // seconds: the glass glint comes no oftener than this
#define GLINT_AFTER_LIFT 3.0f        // seconds after the finger lifts, while held: one glint
static volatile float s_holdReq = -1;   // the latest touched speed, or -1
static volatile bool  s_resumeReq = false;
static volatile bool  s_liftReq = false;
static float s_lastGlint = -100;     // s_clock at the last glint
static float s_glintAt = -1;         // s_clock at which a held dial glints, or -1
static bool  s_hold = false;
static float s_holdMph = 0;          // where the touch says, and where the hold has got to
static float s_holdNow = 0;

/* ================================================================ the old way */

// What a typical TFT_eSPI meter sketch does: draw the dial once, then every
// so often erase the needle and the number and draw them again.  Here each
// erase and draw is shown painting from top to bottom, exaggerated, the way
// the panel shows a slow redraw.
// TFT_eSPI colour names
#define TFT_BLACK   0x0000
#define TFT_WHITE   0xFFFF
#define TFT_RED     0xF800
#define TFT_GREEN   0x07E0
#define TFT_DARKGREY 0x7BEF

#define OLD_UPDATE   0.34f           // seconds between updates
#define OLD_ERASE_N  0.09f           // the needle's erase, then its redraw
#define OLD_ERASE_T  0.06f           // the number's erase, then its redraw
#define OLD_PAINT0   0.25f           // the first full-screen draw ...
#define OLD_PAINT1   1.35f           // ... painted over this time

struct OldState {
    float angA, angB;                // needle angles (deg, from up): A the old one, B the new
    int   aY0, aY1, bY0, bY1;        // rows where each is visible [y0, y1)
    int   numA, numB;                // numbers shown
    int   nA0, nA1, nB0, nB1;        // rows where each number is visible
    int   wipe;                      // first full draw: rows above this are drawn
    int   shownV;                    // the value last drawn
    int   endA[4], endB[4];          // the needles' line ends
    char  strA[4], strB[4];          // the numbers as drawn, "%3d"
};
#define OLD_NTICK (OLD_VMAX / 10 + 1)
struct OldTick { int x0, y0, x1, y1; uint16_t color; char label[4]; int lx, ly; };
static OldTick s_oldTick[OLD_NTICK];
static OldState s_old;
static int s_oldTs = 1, s_oldBig = 4, s_oldMph = 2;   // TFT_eSPI text sizes
static float s_oldR;

static float old_angle(float v) { return -135.0f + 270.0f * v / OLD_VMAX; }
static float old_speed(float t) {
    if (t < 1.6f) return 0;
    return 64.0f * (1 - expf(-(t - 1.6f) / 3.6f));
}

// The needle's line ends, in pixel coordinates (TFT_eSPI uses integers).
static void old_needle_ends(float ang, int &x0, int &y0, int &x1, int &y1) {
    float a = ang * 0.0174533f;
    x0 = CX; y0 = CY;
    x1 = CX + (int)lroundf(sinf(a) * s_oldR * 0.86f);
    y1 = CY - (int)lroundf(cosf(a) * s_oldR * 0.86f);
}

static void old_number_box(int &y0, int &y1) {
    y0 = CY + (int)(0.36f * R);
    y1 = y0 + 8 * s_oldBig;
}

static void old_step_core() {
    OldState &o = s_old;
    const float t = s_pt;
    o.wipe = t < OLD_PAINT0 ? 0 : t >= OLD_PAINT1 ? LH : (int)((t - OLD_PAINT0) / (OLD_PAINT1 - OLD_PAINT0) * LH);
    int ny0, ny1;
    old_number_box(ny0, ny1);
    if (t < OLD_PAINT1) {
        o.angA = o.angB = old_angle(0);
        o.aY0 = o.aY1 = 0; o.bY0 = 0; o.bY1 = LH;
        o.numA = o.numB = 0; o.nA0 = o.nA1 = 0; o.nB0 = 0; o.nB1 = LH;
        o.shownV = 0;
        return;
    }
    // Which update are we in, and how far through it?
    const float since = t - OLD_PAINT1;
    const int k = (int)(since / OLD_UPDATE);
    const float u = since - k * OLD_UPDATE;
    const int vOld = (int)lroundf(old_speed(OLD_PAINT1 + k * OLD_UPDATE - OLD_UPDATE));
    const int vNew = (int)lroundf(old_speed(OLD_PAINT1 + k * OLD_UPDATE));
    o.angA = old_angle(k == 0 ? 0 : vOld);
    o.angB = old_angle(vNew);
    o.numA = k == 0 ? 0 : vOld;
    o.numB = vNew;
    o.shownV = vNew;
    // The needle's rows: the span both lines cover.
    int ax0, ay0, ax1, ay1, bx0, by0, bx1, by1;
    old_needle_ends(o.angA, ax0, ay0, ax1, ay1);
    old_needle_ends(o.angB, bx0, by0, bx1, by1);
    int top = ay0 < ay1 ? ay0 : ay1, bot = ay0 > ay1 ? ay0 : ay1;
    if (by0 < top) top = by0; if (by1 < top) top = by1;
    if (by0 > bot) bot = by0; if (by1 > bot) bot = by1;
    top -= 2; bot += 3;
    const bool changed = vNew != (k == 0 ? 0 : vOld);
    if (!changed || u >= 2 * OLD_ERASE_N) {
        o.aY0 = o.aY1 = 0; o.bY0 = 0; o.bY1 = LH;
    } else if (u < OLD_ERASE_N) {          // erasing the old needle, top down
        int w = top + (int)((bot - top) * u / OLD_ERASE_N);
        o.aY0 = w; o.aY1 = LH; o.bY0 = o.bY1 = 0;
    } else {                               // drawing the new one, top down
        int w = top + (int)((bot - top) * (u - OLD_ERASE_N) / OLD_ERASE_N);
        o.aY0 = o.aY1 = 0; o.bY0 = 0; o.bY1 = w;
    }
    // Then the number: erased (fillRect) and drawn again, top down.
    const float u2 = u - 2 * OLD_ERASE_N;
    if (!changed || u2 >= 2 * OLD_ERASE_T) {
        o.nA0 = o.nA1 = 0; o.nB0 = 0; o.nB1 = LH;
    } else if (u2 < 0) {
        o.nA0 = 0; o.nA1 = LH; o.nB0 = o.nB1 = 0;
    } else if (u2 < OLD_ERASE_T) {
        o.nA0 = ny0 + (int)((ny1 - ny0) * u2 / OLD_ERASE_T); o.nA1 = LH; o.nB0 = o.nB1 = 0;
    } else {
        o.nA0 = o.nA1 = 0; o.nB0 = 0; o.nB1 = ny0 + (int)((ny1 - ny0) * (u2 - OLD_ERASE_T) / OLD_ERASE_T);
    }
}

static void old_step() {
    old_step_core();
    OldState &o = s_old;
    old_needle_ends(o.angA, o.endA[0], o.endA[1], o.endA[2], o.endA[3]);
    old_needle_ends(o.angB, o.endB[0], o.endB[1], o.endB[2], o.endB[3]);
    snprintf(o.strA, sizeof o.strA, "%3d", o.numA % 1000);
    snprintf(o.strB, sizeof o.strB, "%3d", o.numB % 1000);
}

static void old_init() {
    for (int i = 0; i < OLD_NTICK; i++) {
        const int v = i * 10;
        OldTick &t = s_oldTick[i];
        const float a = old_angle((float)v) * 0.0174533f;
        const float r0 = s_oldR * (v % 20 == 0 ? 0.80f : 0.87f), r1 = s_oldR * 0.94f;
        t.x0 = CX + (int)lroundf(sinf(a) * r0); t.y0 = CY - (int)lroundf(cosf(a) * r0);
        t.x1 = CX + (int)lroundf(sinf(a) * r1); t.y1 = CY - (int)lroundf(cosf(a) * r1);
        t.color = v >= 100 ? TFT_RED : TFT_WHITE;
        t.label[0] = 0;
        if (v % 20 == 0) {
            snprintf(t.label, sizeof t.label, "%d", v);
            const int tw = (int)strlen(t.label) * 6 * s_oldTs - s_oldTs;
            const float rn = s_oldR * 0.66f;
            t.lx = CX + (int)lroundf(sinf(a) * rn) - tw / 2;
            t.ly = CY - (int)lroundf(cosf(a) * rn) - 4 * s_oldTs;
        }
    }
}

// Rows of an aliased one-pixel line in column x (Bresenham-like).
static inline bool line_rows(int x, int x0, int y0, int x1, int y1, int &ya, int &yb) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
    if (x < x0 || x > x1) return false;
    if (x0 == x1) { ya = y0 < y1 ? y0 : y1; yb = y0 < y1 ? y1 : y0; return true; }
    const float m = (float)(y1 - y0) / (x1 - x0);
    if (fabsf(m) <= 1) { ya = yb = (int)lroundf(y0 + m * (x - x0)); return true; }
    float ylo = y0 + m * (x - 0.5f - x0), yhi = y0 + m * (x + 0.5f - x0);
    if (ylo > yhi) { float t = ylo; ylo = yhi; yhi = t; }
    const int mn = y0 < y1 ? y0 : y1, mx = y0 < y1 ? y1 : y0;
    ya = clampi((int)lroundf(ylo + 0.01f), mn, mx);
    yb = clampi((int)lroundf(yhi - 0.01f), mn, mx);
    if (yb < ya) yb = ya;
    return true;
}

static inline void fill_rows(uint16_t *col, int ya, int yb, uint16_t c, int clip0, int clip1) {
    if (ya < clip0) ya = clip0;
    if (yb >= clip1) yb = clip1 - 1;
    for (int y = ya; y <= yb; y++) col[y] = c;
}

// TFT_eSPI's drawString() in font 1 at size s, clipped to rows [c0, c1).
static void glcd_text_col(uint16_t *col, int x, int tx, int ty, int s, const char *str, uint16_t color,
                          int c0, int c1) {
    const int n = (int)strlen(str);
    const int dx = x - tx;
    if (dx < 0 || dx >= n * 6 * s) return;
    const int ci = dx / (6 * s), cx = (dx / s) % 6;
    if (cx >= 5) return;
    const uint8_t bits = TFT_GLCD_FONT[(uint8_t)str[ci] * 5 + cx];
    for (int r = 0; r < 8; r++)
        if (bits & (1 << r)) fill_rows(col, ty + r * s, ty + r * s + s - 1, color, c0, c1);
}


static void old_draw_needle(uint16_t *col, int x, const int *e, int y0c, int y1c) {
    if (y1c <= y0c) return;
    const int x0 = e[0], y0 = e[1], x1 = e[2], y1 = e[3];
    // Three one-pixel lines side by side, as the TFT_eSPI meter examples do.
    for (int k = -1; k <= 1; k++) {
        int ya, yb;
        if (line_rows(x - k, x0, y0, x1, y1, ya, yb)) fill_rows(col, ya, yb, TFT_RED, y0c, y1c);
    }
}

HOT static void old_column(int x, uint16_t *col) {
    const OldState &o = s_old;
    for (int y = 0; y < LH; y++) col[y] = TFT_BLACK;
    const int wipe = o.wipe;
    // drawCircle(), twice.
    for (int ring = 0; ring < 2; ring++) {
        const float r = s_oldR - ring * 2;
        const float dx = (float)(x - CX);
        const float adx = fabsf(dx);
        if (adx <= r) {
            float hHi = sqrtf(fmaxf(r * r - (adx - 0.5f) * (adx - 0.5f), 0));
            float hLo = sqrtf(fmaxf(r * r - (adx + 0.5f) * (adx + 0.5f), 0));
            if (adx < 0.5f) hHi = r;
            int a = (int)lroundf(hLo), b = (int)lroundf(hHi);
            if (b < a) b = a;
            fill_rows(col, CY - b, CY - a, TFT_WHITE, 0, wipe);
            fill_rows(col, CY + a, CY + b, TFT_WHITE, 0, wipe);
        }
    }
    // Ticks every 10, numbers every 20.
    for (int i = 0; i < OLD_NTICK; i++) {
        const OldTick &t = s_oldTick[i];
        int ya, yb;
        if (line_rows(x, t.x0, t.y0, t.x1, t.y1, ya, yb)) fill_rows(col, ya, yb, t.color, 0, wipe);
        if (t.label[0]) glcd_text_col(col, x, t.lx, t.ly, s_oldTs, t.label, TFT_WHITE, 0, wipe);
    }
    glcd_text_col(col, x, 4, 4, 1, "TFT_eSPI-style sketch", TFT_DARKGREY, 0, wipe);
    {
        const int tw = 3 * 6 * s_oldMph - s_oldMph;
        glcd_text_col(col, x, CX - tw / 2, CY + (int)(0.66f * R), s_oldMph, "MPH", TFT_WHITE, 0, wipe);
    }
    // The number: drawString(), right-aligned in three characters.
    int ny0, ny1;
    old_number_box(ny0, ny1);
    const int nw = 3 * 6 * s_oldBig;
    glcd_text_col(col, x, CX - nw / 2, ny0, s_oldBig, o.strA, TFT_GREEN, o.nA0 > 0 ? o.nA0 : 0, o.nA1 < wipe ? o.nA1 : wipe);
    glcd_text_col(col, x, CX - nw / 2, ny0, s_oldBig, o.strB, TFT_GREEN, o.nB0, o.nB1 < wipe ? o.nB1 : wipe);
    // The needle and its pivot.
    old_draw_needle(col, x, o.endA, o.aY0, o.aY1 < wipe ? o.aY1 : wipe);
    old_draw_needle(col, x, o.endB, o.bY0, o.bY1 < wipe ? o.bY1 : wipe);
    if (abs(x - CX) <= 3) {
        int h = (int)sqrtf(fmaxf(12.25f - (x - CX) * (x - CX), 0));
        fill_rows(col, CY - h, CY + h, TFT_WHITE, 0, wipe);
    }
}

/* ================================================================ the raster speedometer */

// --- speed and the needle
static float s_mph = 0;              // the car's speed
static float s_needle = -135;        // the needle's angle (deg), sprung
static float s_needleV = 0;
static float s_glint = -10;          // the glint's position, in R across the dial (off when outside)

// Start the glint across the glass, unless one came too recently.
static void glint(void) {
    if (s_clock - s_lastGlint < GLINT_GAP) return;
    s_lastGlint = s_clock;
    s_glint = -1.6f;
}
static float s_flash = 0;            // the warp flash at the end, 0..1
static int   s_gear = 0;

static float new_speed(float t, int &gear) {
    gear = 0;
    if (t < LAUNCH) return 0;
    float u = t - LAUNCH;
    for (int g = 0; g < NGEARS; g++) {
        if (u < GEAR_S[g]) {
            gear = g + 1;
            float p = u / GEAR_S[g];
            return GEAR_V[g] + (GEAR_V[g + 1] - GEAR_V[g]) * (1 - (1 - p) * (1 - p) * 0.85f - 0.15f * (1 - p));
        }
        u -= GEAR_S[g];
        if (g < NGEARS - 1) {
            if (u < SHIFT_S) { gear = -(g + 1); return GEAR_V[g + 1] - 2.5f * sinf(3.14159f * u / SHIFT_S); }
            u -= SHIFT_S;
        }
    }
    gear = NGEARS;
    return GEAR_V[NGEARS] + 6.0f * u;     // still pulling
}

// new_speed() backwards: the first moment the run reaches mph (just before
// the launch for 0).  Within a gear, v = 1 - 0.85 q^2 - 0.15 q of the way
// up, q = 1 - p, so q comes from the quadratic formula.
static float time_at_speed(float mph) {
    if (mph <= 0) return LAUNCH;
    float t = LAUNCH;
    for (int g = 0; g < NGEARS; g++) {
        if (mph <= GEAR_V[g + 1]) {
            const float y = (mph - GEAR_V[g]) / (GEAR_V[g + 1] - GEAR_V[g]);
            const float q = (-0.15f + sqrtf(0.0225f + 3.4f * (1 - y))) / 1.7f;
            return t + clampf(1 - q, 0, 1) * GEAR_S[g];
        }
        t += GEAR_S[g] + (g < NGEARS - 1 ? SHIFT_S : 0);
    }
    return t + (mph - GEAR_V[NGEARS]) / 6.0f;
}

static inline float needle_target(float mph) {
    float v = mph > 203 ? 203 : mph;
    return -135.0f + 270.0f * v / 200.0f;
}

// --- per-frame tables
static uint16_t s_bg[4][256];        // the background by glow level, per dither phase
static uint8_t  s_gx[SCENE_MAX_W], s_gy[SCENE_MAX_H];
static uint8_t  s_litA[1024], s_litP[1024];   // light level 0..32 by angle: arc, pie
static uint8_t  s_reflP[1024];       // the pie's fixed reflection
static uint32_t s_lcol[64];          // light colour by role (spread)
static uint32_t s_arcS;              // the arc's colour now (spread)

struct NumNow { float cx, cy, s, inv, hx, hy; int w, h; const uint8_t *d; uint32_t coreS, glowS; int glowL; int x0, x1, y0, y1; };
static NumNow s_num[16];

// --- needle geometry for this frame
struct Needle { float dx, dy, nx, ny; float L, T, hw0, hw1, hwt, G, Ssoft, shx, shy; int lit; };
static Needle s_nd;
static uint32_t s_ndRidge, s_ndLit, s_ndDark, s_ndTailL, s_ndTailD, s_ndGlow;
static int s_glowLvl;

// --- the glass
static int16_t s_shY0[SCENE_MAX_W], s_shY1[SCENE_MAX_W];
static uint16_t s_shI0[SCENE_MAX_W];  // the reflection's level at its top, 8.8
static int16_t s_faceY0[SCENE_MAX_W], s_faceY1[SCENE_MAX_W];   // inside the glass, per column
static const uint32_t WHITE_S = 0x07E0F81Fu;

// --- stars
#define MAX_STARS 360
struct Star { float X, Y, Z; };
struct Seg { float x0, y0, x1, y1; float xmin, xmax; float invDx, invDy, invSx; uint32_t headS, tailS; uint8_t b; };
static Star s_star[MAX_STARS];
static Seg  s_seg[MAX_STARS];
static int  s_nStars = 260, s_nSeg = 0;
static float s_starZ = 0;            // camera speed, world units a second
static float s_shutter = 0.03f;
static uint16_t s_segIdx[SCENE_WORKERS][MAX_STARS];   // per worker: the stars reaching its chunk

static void star_respawn(Star &s, bool anywhere) {
    const float ax = (float)LW / LH * 1.05f;
    do {
        s.X = frand(-ax, ax);
        s.Y = frand(-1.05f, 1.05f);
    } while (fabsf(s.X) < 0.04f && fabsf(s.Y) < 0.04f);
    s.Z = anywhere ? frand(0.08f, 1.0f) : frand(0.9f, 1.0f);
}

static void stars_init() {
    s_nStars = LH > 240 ? 340 : 240;
    for (int i = 0; i < s_nStars; i++) star_respawn(s_star[i], true);
}

static void stars_step(float dt) {
    const float F = LH * 0.5f;
    const float zNear = 0.035f;
    int n = 0;
    const float warm = s_flash;
    for (int i = 0; i < s_nStars; i++) {
        Star &s = s_star[i];
        s.Z -= s_starZ * dt;
        float hx = CX + s.X / s.Z * F, hy = CY + s.Y / s.Z * F;
        if (s.Z < zNear || hx < -8 || hx > LW + 8 || hy < -8 || hy > LH + 8) {
            star_respawn(s, false);
            continue;
        }
        float zt = s.Z + s_starZ * s_shutter;
        if (zt > 1.2f) zt = 1.2f;
        float tx = CX + s.X / zt * F, ty = CY + s.Y / zt * F;
        float near = 1 - s.Z;                          // 0 far .. 1 near
        float b = 0.12f + 0.88f * near * near;
        if (s.Z > 0.85f) b *= (1.0f - s.Z) / 0.15f;    // fade in from the far plane
        Seg &g = s_seg[n++];
        g.x0 = tx; g.y0 = ty; g.x1 = hx; g.y1 = hy;
        g.xmin = fminf(tx, hx) - 1.5f; g.xmax = fmaxf(tx, hx) + 1.5f;
        {   // reciprocals, so drawing has no divisions
            const float dx = hx - tx, dy = hy - ty;
            g.invDx = fabsf(dx) > 1e-4f ? 1.0f / dx : 0;
            g.invDy = fabsf(dy) > 1e-4f ? 1.0f / dy : 0;
            g.invSx = fabsf(dx) > 1e-4f ? dy / dx : 0;      // rows per column
        }
        int bi = clampi((int)(b * 32), 0, 32);
        g.b = (uint8_t)bi;
        g.headS = spreadRGB(190 + (int)(65 * warm), 215 + (int)(40 * warm), 255);
        g.tailS = spreadRGB(40 + (int)(150 * warm), 70 + (int)(140 * warm), 230);
    }
    s_nSeg = n;
}

static void glass_init() {
    const float r1 = 0.875f * R;
    const float ox2 = 0.24f * R, oy2 = 0.50f * R, r2 = 1.08f * R;
    for (int x = 0; x < LW; x++) {
        const float dx = x + 0.5f - CX;
        s_shY0[x] = s_shY1[x] = 0;
        s_faceY0[x] = s_faceY1[x] = 0;
        if (fabsf(dx) >= r1) continue;
        const float h1 = sqrtf(r1 * r1 - dx * dx);
        s_faceY0[x] = (int16_t)ceilf(CY - h1 - 0.5f);
        s_faceY1[x] = (int16_t)floorf(CY + h1 - 0.5f) + 1;
        if (fabsf(dx - ox2) >= r2) continue;
        const float top2 = CY + oy2 - sqrtf(r2 * r2 - (dx - ox2) * (dx - ox2));
        const int y0 = s_faceY0[x];
        const int y1 = (int)fminf(top2, CY + h1);
        if (y1 <= y0) continue;
        s_shY0[x] = (int16_t)y0;
        s_shY1[x] = (int16_t)y1;
        const float hx = 1 - smooth(-0.9f * R, 0.75f * R, dx);   // stronger to the left
        s_shI0[x] = (uint16_t)(256 * 6.5f * (0.35f + 0.65f * hx));
    }
}

static void new_init_tables() {
    // The background glow's shape: a Gaussian round the vanishing point.
    for (int x = 0; x < LW; x++) {
        float d = (x + 0.5f - CX) / (1.15f * R);
        s_gx[x] = (uint8_t)(255 * expf(-d * d));
    }
    for (int y = 0; y < LH; y++) {
        float d = (y + 0.5f - CY) / (1.0f * R);
        s_gy[y] = (uint8_t)(255 * expf(-d * d));
    }
    // The pie's glossy reflection: two soft highlights, as if from lights
    // above and to the right.
    for (int a = 0; a < 1024; a++) {
        float deg = -135.0f + 270.0f * a / 1023.0f;
        float h1 = (deg + 20) / 26, h2 = (deg - 75) / 11;
        s_reflP[a] = (uint8_t)clampi((int)(14 * expf(-h1 * h1) + 9 * expf(-h2 * h2)), 0, 32);
    }
    glass_init();
    stars_init();
}

static uint32_t speed_colour(float f, int &r, int &g, int &b) {
    // cyan -> violet -> magenta -> red, by the share of full scale
    struct { float f; int r, g, b; } st[] = {
        { 0.00f, 0, 200, 255 }, { 0.45f, 40, 150, 255 }, { 0.65f, 150, 80, 255 },
        { 0.78f, 255, 50, 190 }, { 0.86f, 255, 45, 40 }, { 1.0f, 255, 60, 20 } };
    int i = 0;
    while (i < 4 && f > st[i + 1].f) i++;
    float t = clampf((f - st[i].f) / (st[i + 1].f - st[i].f), 0, 1);
    r = (int)(st[i].r + (st[i + 1].r - st[i].r) * t);
    g = (int)(st[i].g + (st[i + 1].g - st[i].g) * t);
    b = (int)(st[i].b + (st[i + 1].b - st[i].b) * t);
    return spreadRGB(r, g, b);
}

static void new_step_main(float dt) {
    const float t = s_pt;
    int gear;
    s_mph = new_speed(t, gear);
    if (s_hold) {
        s_mph = s_holdNow;
        // Held: no glint while the finger moves; one, a while after it lifts.
        if (s_glintAt >= 0 && s_clock >= s_glintAt) { s_glintAt = -1; glint(); }
    } else {
        if (gear < 0 && s_gear > 0) glint();              // a shift: the glint sweeps
        if (t > 1.0f && t - dt <= 1.0f) glint();          // and once as the dial appears
    }
    s_gear = gear;
    if (s_glint > -5) { s_glint += dt * 4.2f; if (s_glint > 1.8f) s_glint = -10; }
    s_flash = smooth(FLASH_AT, FLASH_AT + 0.55f, t);

    // The needle on a spring, a little under-damped.
    const float target = needle_target(s_mph);
    const int sub = 4;
    const float h = dt / sub, w = 26.0f, z = 0.55f;
    for (int i = 0; i < sub; i++) {
        float acc = w * w * (target - s_needle) - 2 * z * w * s_needleV;
        s_needleV += acc * h;
        s_needle += s_needleV * h;
    }
    float ang = s_needle;
    if (s_mph > 203) ang += 0.6f * sinf(s_clock * 63.0f) + 0.4f * sinf(s_clock * 97.0f);   // pinned on the stop: it buzzes
    const float aRad = ang * 0.0174533f;
    Needle &n = s_nd;
    n.dx = sinf(aRad); n.dy = -cosf(aRad);
    n.nx = cosf(aRad); n.ny = sinf(aRad);
    n.L = 0.84f * R; n.T = 0.17f * R;
    n.hw0 = 0.034f * R; n.hw1 = 0.007f * R; n.hwt = 0.028f * R;
    n.G = 0.07f * R; n.Ssoft = 0.03f * R;
    n.shx = 0.035f * R; n.shy = 0.06f * R;
    // Which side of the blade faces the light (top-left)?
    n.lit = (n.nx * -0.45f + n.ny * -0.75f) > 0;

    const float f = clampf(s_mph / 200.0f, 0, 1.08f);
    int cr, cg, cb;
    s_arcS = speed_colour(fminf(f, 1), cr, cg, cb);
    s_ndRidge = spreadRGB(255, 215, 170);
    s_ndLit = spreadRGB(255, 100, 45);
    s_ndDark = spreadRGB(150, 25, 12);
    s_ndTailL = spreadRGB(90, 92, 105);
    s_ndTailD = spreadRGB(35, 36, 44);
    s_ndGlow = spreadRGB(255, 70, 25);
    s_glowLvl = (int)(10 + 14 * f);

    // The light tables.  na: the needle's place along the arc, 0..1023.
    const float na = clampf((ang + 135.0f) / 270.0f, 0, 1) * 1023.0f;
    const float on = smooth(0, 3, s_mph);
    // In the red zone the lights pulse, faded in so a touch hovering at the
    // line doesn't flicker it on and off.
    const float pulse = 1.0f - 0.18f * smooth(158, 168, s_mph) * (1 - sinf(s_clock * 18.0f));
    for (int a = 0; a < 1024; a++) {
        float d = na - a;                               // >0: behind the needle
        float edge = clampf((d + 3) / 6, 0, 1);        // soft leading edge
        float head = d > 0 ? fmaxf(0, 1 - d / 90.0f) : 0;
        float la = (0.72f + 0.28f * head) * edge * on * pulse;
        s_litA[a] = (uint8_t)clampi((int)(la * 32), 0, 32);
        float sweep = (d - 160) / 70.0f;                // a reflection that trails the needle
        float lp = (6 + s_reflP[a] + 9 * head + 7 * expf(-sweep * sweep)) * edge * on;
        s_litP[a] = (uint8_t)clampi((int)lp, 0, 32);
    }
    for (int k = 1; k < 64; k++) {
        if (k < 40) {
            float s = k / 39.0f;
            // the arc's core runs hot: towards white at full intensity
            float wht = s > 0.8f ? (s - 0.8f) * 2.5f : 0;
            s_lcol[k] = spreadRGB((int)((cr + (255 - cr) * wht * 0.6f) * s), (int)((cg + (255 - cg) * wht * 0.6f) * s),
                                  (int)((cb + (255 - cb) * wht * 0.6f) * s));
        } else {
            float s = (k - 39) / 24.0f;
            s_lcol[k] = spreadRGB((int)(cr * 0.55f * s), (int)(cg * 0.55f * s), (int)(cb * 0.55f * s));
        }
    }
    s_lcol[0] = 0;

    // The background: black, with the warp glow.
    const float gs = 0.08f + 0.6f * f * f + 0.6f * s_flash;
    for (int p = 0; p < 4; p++) {
        const float d = (BAYER2[p >> 1][p & 1] + 0.5f) / 4.0f - 0.5f;
        for (int q = 0; q < 256; q++) {
            float v = q / 255.0f;
            float r = 2 + (25 + 40 * f) * gs * v, g = 3 + 40 * gs * v, b = 8 + 150 * gs * v;
            int ri = clampi((int)(r / 8 + 0.5f + d), 0, 31), gi = clampi((int)(g / 4 + 0.5f + d), 0, 63),
                bi = clampi((int)(b / 8 + 0.5f + d), 0, 31);
            s_bg[p][q] = (uint16_t)(ri << 11 | gi << 5 | bi);
        }
    }

    // The numbers: lit as the speed reaches them, swelling as it passes.
    for (int i = 0; i < A->nNums && i < 16; i++) {
        const int32_t *m = A->numMeta + 5 * i;
        NumNow &u = s_num[i];
        const float v = 20.0f * i;
        float L = i == 0 ? on : smooth(v - 6, v + 1, s_mph);
        float bump = (s_mph - v - 1) / 9.0f;
        u.s = 1.0f + 0.10f * L + 0.28f * expf(-bump * bump) * (i == 0 ? on : 1);
        if (u.s > NUM_MAX_SCALE) u.s = NUM_MAX_SCALE;
        u.cx = m[0] / 16.0f; u.cy = m[1] / 16.0f;
        u.w = m[2]; u.h = m[3];
        u.d = A->numData + m[4];
        u.inv = NUM_MAX_SCALE / u.s;
        u.hx = u.w * 0.5f / u.inv; u.hy = u.h * 0.5f / u.inv;
        u.x0 = (int)floorf(u.cx - u.hx); u.x1 = (int)ceilf(u.cx + u.hx);
        u.y0 = clampi((int)floorf(u.cy - u.hy), 0, LH); u.y1 = clampi((int)ceilf(u.cy + u.hy), 0, LH);
        const bool red = v >= 160;
        int r0 = 150, g0 = 158, b0 = 172;
        int r1 = red ? 255 : 255, g1 = red ? 120 : 255, b1 = red ? 100 : 255;
        u.coreS = spreadRGB((int)(r0 + (r1 - r0) * L), (int)(g0 + (g1 - g0) * L), (int)(b0 + (b1 - b0) * L));
        int gr, gg, gb;
        if (red) { gr = 255; gg = 50; gb = 30; } else speed_colour(fminf(v / 200.0f, 1), gr, gg, gb);
        u.glowS = spreadRGB(gr, gg, gb);
        u.glowL = (int)(L * 30);
    }

    // The stars' speed.
    s_starZ = 0.015f + 2.4f * f * f + 1.5f * s_flash;
    s_shutter = 0.02f + 0.07f * f + 0.06f * s_flash;
}

/* ---------------------------------------------------------------- drawing */

HOT static void draw_background(int x, uint16_t *col) {
    const uint16_t *t0 = s_bg[(x & 1)], *t1 = s_bg[2 | (x & 1)];
    const uint32_t gx = s_gx[x];
    for (int y = 0; y < LH; y += 2) {
        col[y] = t0[(gx * s_gy[y]) >> 8];
        col[y + 1] = t1[(gx * s_gy[y + 1]) >> 8];
    }
}

HOT static void draw_stars(int x, uint16_t *col, const uint16_t *idx, int nIdx) {
    const float xc = x + 0.5f;
    for (int k = 0; k < nIdx; k++) {
        const Seg &g = s_seg[idx[k]];
        if (xc < g.xmin || xc > g.xmax) continue;
        const float dx = g.x1 - g.x0, dy = g.y1 - g.y0;
        const float adx = fabsf(dx), ady = fabsf(dy);
        if (adx < 0.7f && ady < 0.7f) {                // a point: a 2 x 2 splat
            float fx = 1 - fabsf(g.x1 - xc);
            if (fx <= 0) continue;
            float yy = g.y1 - 0.5f;
            int yi = (int)floorf(yy);
            float fy = yy - yi;
            int w0 = (int)(g.b * fx * (1 - fy)), w1 = (int)(g.b * fx * fy);
            if ((unsigned)yi < (unsigned)LH && w0) col[yi] = addS(col[yi], scaleS(g.headS, w0));
            if ((unsigned)(yi + 1) < (unsigned)LH && w1) col[yi + 1] = addS(col[yi + 1], scaleS(g.headS, w1));
            continue;
        }
        if (adx >= ady) {                               // mostly across: one or two rows in this column
            float t = (xc - g.x0) * g.invDx;
            if (t < 0 || t > 1) continue;
            float yy = g.y0 + t * dy - 0.5f;
            int yi = (int)floorf(yy);
            float fy = yy - yi;
            float bt = g.b * (0.15f + 0.85f * t);
            uint32_t c = t > 0.75f ? g.headS : g.tailS;
            int w0 = (int)(bt * (1 - fy)), w1 = (int)(bt * fy);
            if ((unsigned)yi < (unsigned)LH && w0) col[yi] = addS(col[yi], scaleS(c, w0));
            if ((unsigned)(yi + 1) < (unsigned)LH && w1) col[yi + 1] = addS(col[yi + 1], scaleS(c, w1));
        } else {                                        // mostly down: a run of rows
            // rows where the line's x is within 1 of this column's centre
            float ya, yb;
            if (g.invSx == 0) { ya = g.y0 < g.y1 ? g.y0 : g.y1; yb = g.y0 < g.y1 ? g.y1 : g.y0; }
            else {
                ya = g.y0 + (xc - 1 - g.x0) * g.invSx; yb = g.y0 + (xc + 1 - g.x0) * g.invSx;
                if (ya > yb) { float tt = ya; ya = yb; yb = tt; }
                float lo = g.y0 < g.y1 ? g.y0 : g.y1, hi = g.y0 < g.y1 ? g.y1 : g.y0;
                if (ya < lo) ya = lo;
                if (yb > hi) yb = hi;
            }
            int y0 = clampi((int)floorf(ya), 0, LH), y1 = clampi((int)ceilf(yb), 0, LH);
            const float inv = g.invDy;
            for (int y = y0; y < y1; y++) {
                float yc = y + 0.5f;
                float t = (yc - g.y0) * inv;
                if (t < 0 || t > 1) continue;
                float w = 1 - fabsf(g.x0 + t * dx - xc);
                if (w <= 0) continue;
                int wi = (int)(g.b * w * (0.15f + 0.85f * t));
                if (wi) col[y] = addS(col[y], scaleS(t > 0.75f ? g.headS : g.tailS, wi));
            }
        }
    }
}

HOT static void draw_dial(int x, uint16_t *col) {
    const int dc = x - A->dialX0;
    if ((unsigned)dc >= (unsigned)A->dialW) return;
    const int32_t *m = A->dialCol + 3 * dc;
    const int y0 = m[0], n = m[1];
    const uint16_t *rgb = A->dialRgb + m[2];
    const uint8_t *tt = A->dialT + m[2];
    uint16_t *o = col + y0;
    for (int i = 0; i < n; i++) {
        const uint32_t t = tt[i];
        if (t == 0) o[i] = rgb[i];
        else o[i] = unspread(sat(spread(rgb[i]) + scaleS(spread(o[i]), t)));
    }
}

HOT static void draw_light(int x, uint16_t *col) {
    const int dc = x - A->dialX0;
    if ((unsigned)dc >= (unsigned)A->dialW) return;
    const int first = A->lightCol[2 * dc], cnt = A->lightCol[2 * dc + 1];
    for (int r = 0; r < cnt; r++) {
        const int32_t *run = A->lightRun + 3 * (first + r);
        uint16_t *o = col + run[0];
        const uint16_t *d = A->light + run[2];
        for (int i = 0; i < run[1]; i++) {
            const uint32_t v = d[i], a = v >> 6, k = v & 63;
            const uint32_t lvl = k < 40 ? s_litA[a] : s_litP[a];
            if (lvl) o[i] = addS(o[i], scaleS(s_lcol[k], lvl));
        }
    }
}

HOT static void draw_numbers(int x, uint16_t *col) {
    const float xc = x + 0.5f;
    for (int i = 0; i < A->nNums; i++) {
        const NumNow &u = s_num[i];
        if (x < u.x0 || x >= u.x1) continue;
        const float su = (xc - u.cx) * u.inv + u.w * 0.5f - 0.5f;
        const int iu = (int)floorf(su);
        const float fu = su - iu;
        if (iu < -1 || iu >= u.w) continue;
        const uint8_t *c0 = (iu >= 0) ? u.d + iu * u.h : nullptr;
        const uint8_t *c1 = (iu + 1 < u.w) ? u.d + (iu + 1) * u.h : nullptr;
        const int wu1 = (int)(fu * 256), wu0 = 256 - wu1;
        const int32_t step = (int32_t)(u.inv * 65536);
        int32_t sv = (int32_t)(((u.y0 + 0.5f - u.cy) * u.inv + u.h * 0.5f - 0.5f) * 65536);
        for (int y = u.y0; y < u.y1; y++, sv += step) {
            const int iv = sv >> 16;                       // floor (arithmetic shift)
            if (iv < -1 || iv >= u.h) continue;
            const int wv1 = (sv >> 8) & 255, wv0 = 256 - wv1;
            uint32_t q00 = 0, q01 = 0, q10 = 0, q11 = 0;
            if (iv >= 0) { if (c0) q00 = c0[iv]; if (c1) q10 = c1[iv]; }
            if (iv + 1 < u.h) { if (c0) q01 = c0[iv + 1]; if (c1) q11 = c1[iv + 1]; }
            if (!(q00 | q01 | q10 | q11)) continue;
            // bilinear on both nibbles at once: core in bits 4-7, glow in 0-3
            const uint32_t coreA = (((q00 >> 4) * wv0 + (q01 >> 4) * wv1) * wu0 +
                                    ((q10 >> 4) * wv0 + (q11 >> 4) * wv1) * wu1) >> 16;   // 0..15
            const uint32_t glowA = (((q00 & 15) * wv0 + (q01 & 15) * wv1) * wu0 +
                                    ((q10 & 15) * wv0 + (q11 & 15) * wv1) * wu1) >> 16;
            uint16_t p = col[y];
            if (glowA && u.glowL) p = addS(p, scaleS(u.glowS, (glowA * u.glowL) >> 4));
            if (coreA) p = blend(p, u.coreS, (coreA * 32 + 7) / 15);
            col[y] = p;
        }
    }
}

// The y-range of column x where a rotated rectangle around the pivot
// (px0, py0) -- |v| <= vmax, umin <= u <= umax -- lies.
static inline bool needle_rows(int x, float px0, float py0, float vmax, float umin, float umax, int &ya, int &yb) {
    const Needle &n = s_nd;
    const float px = x + 0.5f - px0;
    float lo = -1e9f, hi = 1e9f;
    auto clip = [&](float k, float a, float b) {     // a <= px*kx + py*k <= b, kx term folded into a, b
        if (fabsf(k) < 1e-6f) { if (a > 0 || b < 0) { lo = 1; hi = 0; } return; }
        float p = a / k, q = b / k;
        if (p > q) { float t = p; p = q; q = t; }
        if (p > lo) lo = p;
        if (q < hi) hi = q;
    };
    clip(n.dy, umin - px * n.dx, umax - px * n.dx);
    clip(n.ny, -vmax - px * n.nx, vmax - px * n.nx);
    if (lo > hi) return false;
    ya = clampi((int)floorf(py0 + lo - 0.5f), 0, LH);
    yb = clampi((int)ceilf(py0 + hi - 0.5f) + 1, 0, LH);
    return yb > ya;
}

HOT static void draw_needle(int x, uint16_t *col) {
    const Needle &n = s_nd;
    int ya, yb;
    // The shadow, offset down and to the right, soft.
    const float sx0 = CX + n.shx, sy0 = CY + n.shy;
    if (needle_rows(x, sx0, sy0, n.hw0 + n.Ssoft, -n.T - n.Ssoft, n.L + n.Ssoft, ya, yb)) {
        const float px = x + 0.5f - sx0;
        float py = ya + 0.5f - sy0;
        float u = px * n.dx + py * n.dy, v = px * n.nx + py * n.ny;
        const float inv2 = 1.0f / (2 * n.Ssoft), kw = (n.hw1 - n.hw0) / n.L;
        for (int y = ya; y < yb; y++, u += n.dy, v += n.ny) {
            float hw = u >= 0 ? n.hw0 + kw * u : n.hwt;
            float c = clampf((hw + n.Ssoft - fabsf(v)) * inv2, 0, 1) *
                      clampf((u + n.T + n.Ssoft) * inv2, 0, 1) * clampf((n.L + n.Ssoft - u) * inv2, 0, 1);
            if (c > 0) col[y] = unspread(scaleS(spread(col[y]), 32 - (uint32_t)(c * 15)));
        }
    }
    // The glow and the blade.
    const float vmax = n.hw0 + n.G;
    if (!needle_rows(x, (float)CX, (float)CY, vmax, -n.T - 1, n.L + n.G * 0.5f, ya, yb)) return;
    const float px = x + 0.5f - CX;
    float py = ya + 0.5f - CY;
    float u = px * n.dx + py * n.dy, v = px * n.nx + py * n.ny;
    const float invL = 1.0f / n.L, invG = 1.0f / n.G;
    for (int y = ya; y < yb; y++, u += n.dy, v += n.ny) {
        const bool blade = u >= 0;
        const float hw = blade ? n.hw0 + (n.hw1 - n.hw0) * u * invL : n.hwt;
        const float av = fabsf(v);
        uint16_t p = col[y];
        if (blade && av > hw - 0.5f) {                  // the glow outside the blade
            float d = (av - hw) * invG;
            float tip = clampf((n.L + n.G * 0.5f - u) * invG * 2, 0, 1);
            if (d < 1) {
                float g = (1 - d) * (1 - d) * tip;
                int gl = (int)(g * s_glowLvl);
                if (gl > 0) p = addS(p, scaleS(s_ndGlow, gl));
            }
        }
        float cov = clampf(hw - av + 0.5f, 0, 1) * clampf(u + n.T + 0.5f, 0, 1) * clampf(n.L - u + 0.5f, 0, 1);
        if (cov > 0) {
            float s = av / hw;                         // 0 at the ridge, 1 at the edge
            if (s > 1) s = 1;
            const bool litSide = (v > 0) == (n.lit != 0);
            uint32_t c;
            if (blade) {
                uint32_t a = s_ndRidge, b = litSide ? s_ndLit : s_ndDark;
                uint32_t w = (uint32_t)(s * 32);
                c = ((((b - a) * w + 0x02008010u) >> 5) + a) & SMASK;
            } else {
                c = litSide ? s_ndTailL : s_ndTailD;
            }
            p = blend(p, c, (uint32_t)(cov * 32 + 0.5f));
        }
        col[y] = p;
    }
}

HOT static void draw_hub(int x, uint16_t *col) {
    const int hx = x - A->hubX0;
    if ((unsigned)hx >= (unsigned)A->hubN) return;
    const uint16_t *rgb = A->hubRgb + hx * A->hubN;
    const uint8_t *al = A->hubA + hx * A->hubN;
    for (int i = 0; i < A->hubN; i++) {
        const uint32_t a = al[i];
        if (!a) continue;
        const int y = A->hubY0 + i;
        col[y] = a >= 32 ? rgb[i] : blend(col[y], spread(rgb[i]), a);
    }
}

static int s_digits[3], s_lead;              // the read-out: digits, and how many leading ones are ghosts
static int s_kmh[3], s_kmhN;
static uint32_t s_bigCore, s_bigGlow, s_bigGhost, s_smallS;

HOT static void draw_readout(int x, uint16_t *col) {
    // Big digits.
    for (int i = 0; i < 3; i++) {
        const int gx = A->bigX0 + (int)lroundf(i * A->bigAdv);
        const int c = x - gx;
        if ((unsigned)c >= (unsigned)A->bigW) continue;
        const uint8_t *d = A->big + (s_digits[i] * A->bigW + c) * A->bigH;
        uint16_t *o = col + A->bigY0;
        const bool ghost = i < s_lead;
        for (int k = 0; k < A->bigH; k++) {
            const uint32_t q = d[k];
            if (!q) continue;
            uint16_t p = o[k];
            if (ghost) {
                if (q >> 4) p = blend(p, s_bigGhost, ((q >> 4) * 32) / 15 / 3);
            } else {
                if (q & 15) p = addS(p, scaleS(s_bigGlow, (q & 15) * 2));
                if (q >> 4) p = blend(p, s_bigCore, ((q >> 4) * 32 + 7) / 15);
            }
            o[k] = p;
        }
    }
    // km/h, right-aligned.
    for (int i = 3 - s_kmhN; i < 3; i++) {
        const int gx = A->smallX0 + (int)lroundf(i * A->smallAdv);
        const int c = x - gx;
        if ((unsigned)c >= (unsigned)A->smallW) continue;
        const uint8_t *d = A->small + (s_kmh[i] * A->smallW + c) * A->smallH;
        uint16_t *o = col + A->smallY0;
        for (int k = 0; k < A->smallH; k++)
            if (d[k]) o[k] = blend(o[k], s_smallS, (d[k] * 32 + 7) / 15);
    }
}

HOT static void draw_glass(int x, uint16_t *col) {
    // The fixed reflection.
    const int y0 = s_shY0[x], y1 = s_shY1[x];
    if (y1 > y0) {
        const int n = y1 - y0;
        const float a = s_shI0[x] / (256.0f * n * n);
        for (int y = y0; y < y1; y++) {
            const float p = (float)(y1 - y);                    // 1..n, bigger toward the top
            const uint32_t lvl = (uint32_t)(a * p * p);
            if (lvl) col[y] = addS(col[y], scaleS(WHITE_S, lvl));
        }
    }
    // The glint: a diagonal band across the glass.
    if (s_glint > -5 && s_faceY1[x] > s_faceY0[x]) {
        const float dx = (x + 0.5f - CX) / R;
        const float w = 0.12f, k = 0.55f, kr = k / R, iw = 1 / w;
        float ya = CY + (s_glint - w - dx) / k * R, yb = CY + (s_glint + w - dx) / k * R;
        int a = clampi((int)ya, s_faceY0[x], s_faceY1[x]), b = clampi((int)yb + 1, s_faceY0[x], s_faceY1[x]);
        for (int y = a; y < b; y++) {
            float g = 1 - fabsf(dx + kr * (y + 0.5f - CY) - s_glint) * iw;
            if (g > 0) col[y] = addS(col[y], scaleS(WHITE_S, (uint32_t)(g * g * 11)));
        }
    }
}

// The warp flash: every pixel moved toward white by f, as two byte tables
// (each channel's change is linear, so the high and low bytes' parts just
// add -- green's two parts stay inside its field).
static uint16_t s_fHi[256], s_fLo[256];
static bool s_flashOn = false;
static void flash_tables(float f) {
    s_flashOn = f > 0.004f;
    if (!s_flashOn) return;
    const float k = 1 - f;
    for (int h = 0; h < 256; h++) {
        int r = h >> 3, gh = (h & 7) << 3;
        int r2 = (int)(r * k + 31 * f + 0.5f), g2 = (int)(gh * k);
        s_fHi[h] = (uint16_t)(r2 << 11 | g2 << 5);
    }
    for (int l = 0; l < 256; l++) {
        int gl = l >> 5, b = l & 31;
        int g2 = (int)(gl * k + 63 * f + 0.5f), b2 = (int)(b * k + 31 * f + 0.5f);
        if (g2 > 63) g2 = 63;
        s_fLo[l] = (uint16_t)(g2 << 5 | b2);
    }
}

static void readout_step() {
    int v = (int)(s_mph + 0.5f);
    if (v > 999) v = 999;
    s_digits[0] = v / 100; s_digits[1] = v / 10 % 10; s_digits[2] = v % 10;
    s_lead = v >= 100 ? 0 : v >= 10 ? 1 : 2;
    int k = (int)(s_mph * 1.609344f + 0.5f);
    if (k > 999) k = 999;
    s_kmh[0] = k / 100; s_kmh[1] = k / 10 % 10; s_kmh[2] = k % 10;
    s_kmhN = k >= 100 ? 3 : k >= 10 ? 2 : 1;
    int r, g, b;
    speed_colour(fminf(s_mph / 200.0f, 1), r, g, b);
    s_bigGlow = spreadRGB(r, g, b);
    s_bigCore = spreadRGB(235, 248, 255);
    s_bigGhost = spreadRGB(60, 80, 110);
    s_smallS = spreadRGB(150, 190, 220);
}

/* ================================================================ phases */

static void enter_phase(int p) {
    s_phase = p;
    s_pt = 0;
    if (p == 1) {
        s_mph = 0; s_needle = -135; s_needleV = 0; s_glint = -10; s_flash = 0; s_gear = 0;
        for (int i = 0; i < s_nStars; i++) star_respawn(s_star[i], true);
    } else {
        memset(&s_old, 0, sizeof s_old);
    }
}

bool scene_supports(int panelW, int panelH) {
    return (panelW == 240 && panelH >= 320) || (panelW == 320 && panelH >= 480);
}

void scene_init(int panelW, bool flip) {
    A = panelW == 320 ? &ASSET_L : &ASSET;
    LW = A->lw; LH = A->lh; CX = A->cx; CY = A->cy; R = A->R;
    K = LH / 240.0f;
    s_flip = flip;
    s_dir = flip ? 1 : -1;
    s_oldR = 0.95f * R;
    s_oldTs = LH > 240 ? 2 : 1;
    s_oldBig = LH > 240 ? 5 : 4;
    s_oldMph = LH > 240 ? 3 : 2;
    float u = 0;
    for (int g = 0; g < NGEARS; g++) u += GEAR_S[g] + (g < NGEARS - 1 ? SHIFT_S : 0);
    NEW_END_CLIMB = LAUNCH + u;
    FLASH_AT = NEW_END_CLIMB + 0.9f;
    NEW_SECONDS = FLASH_AT + 1.6f;
    new_init_tables();
    old_init();
    enter_phase(0);
}

int scene_width(void) { return LW; }
int scene_height(void) { return LH; }
int scene_chunks(void) { return LW / SCENE_CHUNK; }
void scene_next_phase(void) { s_skip = true; }
int scene_phase(void) { return s_phase; }
const char *scene_phase_name(void) { return s_hold ? "hold" : NAMES[s_phase]; }
void scene_hold_speed(float mph) { s_holdReq = mph < 0 ? 0 : mph; }
void scene_hold_lifted(void) { s_liftReq = true; }
void scene_resume(void) { s_resumeReq = true; }
bool scene_holding(void) { return s_hold; }
float scene_hold_max(void) { return HOLD_MAX; }
float scene_speed_mph(void) { return s_phase == 1 ? s_mph : (float)s_old.shownV; }
int scene_min_periods(void) { return 2; }

static float phase_len(void) { return s_phase == 0 ? OLD_SECONDS : NEW_SECONDS; }

float scene_brightness(void) {
    const float len = phase_len();
    if (s_phase == 0) return clampf(fminf(s_pt / 0.2f, (len - s_pt) / 1.0f), 0, 1);
    return clampf(fminf(s_pt / NEW_FADE_IN, (len - s_pt) / 0.9f), 0, 1);
}

void scene_step_begin(float dt) {
    uint32_t t0 = CYC();
    if (dt > 0.1f) dt = 0.1f;
    s_dt = dt;
    s_clock += dt;
    if (s_clock > 3600) {
        s_clock -= 3600; s_lastGlint -= 3600;
        if (s_glintAt >= 0) s_glintAt = s_glintAt > 3600 ? s_glintAt - 3600 : 0;
    }
    const float req = s_holdReq;
    if (req >= 0) {
        s_holdReq = -1;
        s_glintAt = -1;                      // touched again: not yet
        if (!s_hold) {                       // the first touch: take over from wherever it is
            if (s_phase != 1) enter_phase(1);
            s_hold = true;
            s_holdNow = s_mph;
        }
        s_holdMph = clampf(req, 0, HOLD_MAX);
    }
    if (s_liftReq) {                         // after the speed: lift-off sends both at once
        s_liftReq = false;
        if (s_hold) s_glintAt = s_clock + GLINT_AFTER_LIFT;
    }
    if (s_resumeReq || (s_skip && s_hold)) {
        s_resumeReq = false;
        s_hold = false;                      // s_pt is already where the run would be
    }
    if (s_hold) {
        s_holdNow += (s_holdMph - s_holdNow) * (1 - expf(-dt / HOLD_SMOOTH));
        s_pt = time_at_speed(s_holdNow);
        s_skip = false;
    } else {
        if (s_skip) {
            s_skip = false;
            const float out = phase_len() - 1.0f;
            if (s_pt < out) s_pt = out;
        }
        s_pt += dt;
        if (s_pt >= phase_len()) enter_phase(s_phase ^ 1);
    }
    s_prof[0][SCENE_PROF_STEP] += CYC() - t0;
}

void scene_step_main(void) {
    uint32_t t0 = CYC();
    if (s_phase == 0) old_step();
    else { new_step_main(s_dt); readout_step(); flash_tables(s_flash * s_flash * 0.75f); }
    s_prof[0][SCENE_PROF_STEP] += CYC() - t0;
}

void scene_step_helper(void) {
    uint32_t t0 = CYC();
    if (s_phase == 1) stars_step(s_dt);
    s_prof[1][SCENE_PROF_STEP] += CYC() - t0;
}

void scene_step(float dt) {
    scene_step_begin(dt);
    scene_step_main();
    scene_step_helper();
}

HOT void scene_render_chunk(int c, uint16_t *dst, int worker) {
    s_dst = dst;
    s_x0 = s_flip ? LW - SCENE_CHUNK - c * SCENE_CHUNK : c * SCENE_CHUNK;
    volatile uint32_t *prof = s_prof[worker];
    uint16_t *col = s_col[worker];
    const int d = s_dir;
    if (s_phase == 0) {
        uint32_t t0 = CYC();
        for (int xi = 0; xi < SCENE_CHUNK; xi++) {
            const int x = s_x0 + xi;
            old_column(x, col);
            uint16_t *o = colp(x);
            for (int y = 0; y < LH; y++, o += d) *o = SW(col[y]);
        }
        prof[SCENE_PROF_TEXT] += CYC() - t0;
        return;
    }
    // The stars that reach this chunk.
    int nIdx = 0;
    uint16_t *idx = s_segIdx[worker];
    const float cx0 = (float)s_x0, cx1 = (float)(s_x0 + SCENE_CHUNK);
    for (int i = 0; i < s_nSeg; i++)
        if (s_seg[i].xmax >= cx0 && s_seg[i].xmin <= cx1) idx[nIdx++] = (uint16_t)i;
    uint32_t cy[SCENE_PROF_N] = {};
    for (int xi = 0; xi < SCENE_CHUNK; xi++) {
        const int x = s_x0 + xi;
        uint32_t t0 = CYC();
        draw_background(x, col);
        uint32_t t1 = CYC();
        draw_stars(x, col, idx, nIdx);
        uint32_t t2 = CYC();
        draw_dial(x, col);
        uint32_t t3 = CYC();
        draw_light(x, col);
        uint32_t t4 = CYC();
        draw_numbers(x, col);
        draw_needle(x, col);
        draw_hub(x, col);
        uint32_t t5 = CYC();
        draw_readout(x, col);
        if (s_flash < 0.5f) draw_glass(x, col);      // (lost in the white-out anyway)
        uint16_t *o = colp(x);
        if (s_flashOn) {           // toward white: two look-ups
            for (int y = 0; y < LH; y++, o += d) { const uint16_t c = col[y]; *o = SW((uint16_t)(s_fHi[c >> 8] + s_fLo[c & 255])); }
        } else {
            for (int y = 0; y < LH; y++, o += d) *o = SW(col[y]);
        }
        uint32_t t6 = CYC();
        cy[SCENE_PROF_BG] += t1 - t0; cy[SCENE_PROF_STARS] += t2 - t1; cy[SCENE_PROF_DIAL] += t3 - t2;
        cy[SCENE_PROF_LIGHT] += t4 - t3; cy[SCENE_PROF_NEEDLE] += t5 - t4; cy[SCENE_PROF_TEXT] += t6 - t5;
    }
    for (int k = 0; k < SCENE_PROF_STEP; k++) prof[k] += cy[k];
}

void scene_take_profile(uint32_t out[SCENE_PROF_N]) {
    for (int k = 0; k < SCENE_PROF_N; k++) {
        out[k] = 0;
        for (int w = 0; w < SCENE_WORKERS; w++) { out[k] += s_prof[w][k]; s_prof[w][k] = 0; }
    }
}

// The no-speed-test notice, in TFT_eSPI's font, white on black.
void scene_notice_row(int panelW, bool flip, int r, uint16_t *row) {
    static const char *L[3] = { "Performance data not saved to board NVS.", "Run stress test sketch first.",
                                "Demo in degraded mode." };
    const int lw = panelW == 320 ? 480 : 320, lh = panelW;
    const int s = panelW == 320 ? 2 : 1;
    const int x = flip ? lw - 1 - r : r;
    for (int c = 0; c < lh; c++) row[c] = 0;
    uint16_t tmp[SCENE_MAX_H];
    for (int y = 0; y < lh; y++) tmp[y] = 0;
    for (int k = 0; k < 3; k++) {
        const int tw = (int)strlen(L[k]) * 6 * s - s;
        glcd_text_col(tmp, x, (lw - tw) / 2, lh / 2 - 20 * s + k * 14 * s, s, L[k], 0xFFFF, 0, lh);
    }
    for (int c = 0; c < lh; c++) row[c] = tmp[flip ? c : lh - 1 - c];
}
