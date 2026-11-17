/*
 * scene.cpp - Fire and smoke, a thunderstorm at sunset, and snowfall, over a
 * perspective grid that creeps toward the viewer.  See scene.h.
 *
 * The frame, landscape LW x LH (320 x 240, or 480 x 320):
 *   rows 0 .. HZ-1   the backdrop -- sky, sun or moon, mountains -- copied
 *                    from flash (assets.h, made by tools/gen_assets.py)
 *   rows HZ .. LH-1  the ground: a ground plane seen from a camera KX units
 *                    up with a focal length of KZ / KX pixels.  Lines on it
 *                    are anti-aliased by the exact share of each pixel's
 *                    footprint they cover, so they fade to an even tint where
 *                    they get too fine.
 * and on top, per scene:
 *   fire   burning logs; flames from a heat field (a cooling-map fire, run
 *          at a fixed 30 Hz, scaled up bilinearly as it is drawn); smoke
 *          puffs that rise, grow, drift and fade; sparks; the fire's
 *          flickering light on the ground
 *   storm  rain streaks at depths, each falling to the ground row of its
 *          depth and splashing there (a ring and a few droplets); wet ground
 *          reflecting the sky, rippled; lightning with branching bolts
 *   snow   flakes at depths, swaying in a gusting wind; where they land they
 *          stay on the ground and ride it toward the viewer, fading
 *
 * Everything is laid out for 320 x 240 and scaled by K (1, or 4/3 on the
 * larger panels).
 *
 * Speed.  A chunk has to be drawn in about the time the previous one takes
 * to send (the raster task chases the panel's scan), so the per-pixel work
 * is kept to table look-ups:
 *   - Chunks are drawn straight into the bounce buffer, in panel order and
 *     byte order: no tile, no copy.  PP(x, y) finds a pixel.
 *   - The ground's converging lines never move, so their coverage of each
 *     pixel is worked out once (g_vq, mirrored about the centre).  Each
 *     frame a colour table per ground row (g_lut, 33 coverages) folds in the
 *     moving cross lines, the fog and the lightning; a ground pixel is then a
 *     look-up -- plus, for the fire's light and the wet reflection, one
 *     saturating add of RGB565 "spread" words.
 *   - The flames are interpolated across once per column, then down.
 *   - No trigonometry or square roots per pixel or per chunk.
 *
 * Copyright (c) 2026 chipguyhere
 * MIT License
 */

#pragma GCC optimize ("O2")

#include "scene.h"
#include "assets.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

// The drawing runs from IRAM on the ESP32: the backdrops stream through the
// flash cache every frame, and code fetched through it would miss constantly.
#if defined(ARDUINO)
#include <esp_attr.h>
#include <esp_cpu.h>
#define HOT IRAM_ATTR
#define CYC() esp_cpu_get_cycle_count()
#include <esp_heap_caps.h>
// Tables read every frame: internal RAM, not PSRAM (on the S3, malloc() may
// put a large block in PSRAM, which is slower).
static void *scene_alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
#else
static void *scene_alloc(size_t n) { return malloc(n); }
#define HOT
#define CYC() 0u
#endif

// Profile: CPU cycles per stage (see scene.h), per worker,
// since scene_take_profile().
static volatile uint32_t s_prof[SCENE_WORKERS][SCENE_PROF_N];

// (The RISC-V toolchain's headers define HZ, the system tick rate; here it
// is the horizon row.)
#ifdef HZ
#undef HZ
#endif

#define MAX_GR (SCENE_MAX_H - ASSET_HZ_L)
#define NQ 33                     // vertical-line coverages in the ground table: 0..32

static int   LW = 320, LH = 240, HZ = ASSET_HZ, GR = 240 - ASSET_HZ, CX = 160;
static float K = 1;              // scale against the 320 x 240 layout

// The ground plane.  A pixel row dy below the horizon is at depth KZ / dy;
// a pixel column dx from CX at lateral position dx * KX / dy.
static float KZ = 400.0f;
#define KX 2.0f
#define LINE_Z 0.07f            // width of the lines across (world units; spacing 1)
#define LINE_X 0.05f            // width of the lines toward the horizon
#define GROUND_SPEED 1.1f       // world units a second toward the viewer

/* ================================================================ helpers */

static uint32_t s_rng = 0x9E3779B9u;
static inline uint32_t rnd() {
    uint32_t x = s_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return s_rng = x;
}
static inline float frand(float a, float b) { return a + (rnd() >> 8) * (1.0f / 16777216.0f) * (b - a); }
static inline float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static inline int clampi(int v, int a, int b) { return v < a ? a : v > b ? b : v; }

// In RAM, not flash: every dithered pixel reads it, and flash-cache misses
// are expensive while the other core streams the backdrops.
static uint8_t BAYER[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

static inline uint16_t pack(int r, int g, int b) {
    r = r > 255 ? 255 : r < 0 ? 0 : r;
    g = g > 255 ? 255 : g < 0 ? 0 : g;
    b = b > 255 ? 255 : b < 0 ? 0 : b;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static inline uint16_t packd(int r, int g, int b, int d) {      // ordered-dithered, d = 0..15
    return pack(r + (d >> 1), g + (d >> 2), b + (d >> 1));
}
static inline int R8(uint16_t c) { return (c >> 8) & 0xF8; }
static inline int G8(uint16_t c) { return (c >> 3) & 0xFC; }
static inline int B8(uint16_t c) { return (c << 3) & 0xF8; }

// RGB565 "spread" into a 32-bit word with room above each field:
// B bits 0-4, R bits 11-15, G bits 21-26.
#define SMASK 0x07E0F81Fu
static inline uint32_t spread(uint16_t c) { return (c | ((uint32_t)c << 16)) & SMASK; }
static inline uint16_t unspread(uint32_t s) { return (uint16_t)(s | (s >> 16)); }
// fgS over bg at alpha a/32, rounded (+16 in each field): truncated, every
// channel would lean half a step toward the background -- and as green's
// steps are half the size of red's and blue's, faint overlays would tint.
static inline uint16_t blend(uint16_t bg, uint32_t fgS, uint32_t a) {
    uint32_t b = spread(bg);
    uint32_t r = ((((fgS - b) * a + 0x02008010u) >> 5) + b) & SMASK;
    return unspread(r);
}
// Between spread colours a and b at w/32, rounded.
static inline uint32_t lerpS(uint32_t a, uint32_t b, uint32_t w) {
    return ((((b - a) * w + 0x02008010u) >> 5) + a) & SMASK;
}
// The sum of two spread words, each field clamped at its maximum.
static inline uint32_t sat(uint32_t s) {
    uint32_t oBR = s & 0x00010020u, oG = s & 0x08000000u;   // carries out of B, R and G
    return (s | (oBR - (oBR >> 5)) | (oG - (oG >> 6))) & SMASK;
}
// spread * a/32 (a 0..32).
static inline uint32_t scaleS(uint32_t s, uint32_t a) { return ((s * a) >> 5) & SMASK; }
static inline uint16_t add(uint16_t c, int r, int g, int b) {
    return pack(R8(c) + r, G8(c) + g, B8(c) + b);
}
static inline uint16_t SW(uint16_t v) { return __builtin_bswap16(v); }

static float hash2(int x, int y, uint32_t seed) {
    uint32_t v = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed * 2246822519u;
    v = (v ^ (v >> 13)) * 1274126177u;
    return ((v ^ (v >> 16)) & 0xFFFF) / 65535.0f;
}
static float vnoise1(float x, uint32_t seed) {     // smooth 1D value noise, 0..1
    int i = (int)floorf(x);
    float f = x - i;
    f = f * f * (3 - 2 * f);
    float a = hash2(i, 0, seed), b = hash2(i + 1, 0, seed);
    return a + (b - a) * f;
}
static float vnoise2(float x, float y, uint32_t seed) {
    int xi = (int)floorf(x), yi = (int)floorf(y);
    float fx = x - xi, fy = y - yi;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    float a = hash2(xi, yi, seed), b = hash2(xi + 1, yi, seed);
    float c = hash2(xi, yi + 1, seed), d = hash2(xi + 1, yi + 1, seed);
    float top = a + (b - a) * fx;
    return top + ((c + (d - c) * fx) - top) * fy;
}

/* ================================================================ state */

static bool      s_flip = false;
static bool      s_large = false;     // the 480 x 320 picture
static int       s_dir = -1;          // a column's step from y to y+1 in the panel row
static uint16_t *s_skyRam = nullptr;
static const SceneAsset *s_asset = &ASSET_FIRE;  // the phase's backdrop: sky and mountains
static const uint16_t *s_sky = nullptr;       // its sky (the storm's), in flash or copied to RAM; null: drawn live

// Two render workers draw chunks at once, each into its own bounce buffer;
// which buffer, and which chunk, is per task (thread-local).
static __thread uint16_t *s_dst;
static __thread int       s_x0;
static __thread int       s_worker;
// A column of the backdrop as composed (native byte order), per worker: the
// wet ground's reflection reads it back.
static uint16_t s_colTmp[SCENE_WORKERS][SCENE_MAX_H];

/* ---------------------------------------------------------------- the mountains' zoom */

// The mountains come slowly toward the viewer: scaled by s_zoom about the
// vanishing point on the horizon, sampled bilinearly from the layer.  Per
// frame, the layer row (and 32nds between it and the next) for each sky row,
// and the layer column for each picture column.
// Scale reached at the end of a scene, above 1: fire, storm, snow.
// Only the snow's: at a lower frame rate (scene_min_periods()), which snow hides well.
static const float ZOOM_GROWTH[3] = { 0.0f, 0.0f, 0.70f };
static float   s_zoom = 1;
static int16_t g_mRow[SCENE_MAX_H];
static uint8_t g_mFy[SCENE_MAX_H];
static int16_t g_mCol[SCENE_MAX_W];
static uint8_t g_mFx[SCENE_MAX_W];

static void zoom_step() {
    const SceneAsset &A = *s_asset;
    const float inv = A.scale / s_zoom;                              // layer pixels per picture pixel
    for (int y = 0; y < HZ; y++) {
        float r = A.lhz - (HZ - (y + 0.5f)) * inv - 0.5f - A.mtop;  // layer row
        if (r < 0) r = 0;
        int ri = (int)r;
        if (ri > A.mh - 1) ri = A.mh - 1;
        g_mRow[y] = (int16_t)ri;
        g_mFy[y] = (uint8_t)((r - ri) * 32 + 0.5f);
    }
    for (int x = 0; x < LW; x++) {
        float c = A.lw * 0.5f + (x + 0.5f - CX) * inv - 0.5f;
        if (c < 0) c = 0;
        int ci = (int)c;
        if (ci > A.lw - 2) ci = A.lw - 2;
        g_mCol[x] = (int16_t)ci;
        g_mFx[x] = (uint8_t)((c - ci) * 32 + 0.5f);
    }
}

// The sky row at which layer row r lands at the current zoom.
static inline float zoom_row(float r) {
    return HZ - (s_asset->lhz - (s_asset->mtop + r)) * s_zoom / s_asset->scale;
}

// The mountains' top at column x (for the lightning to strike).
static int mountain_top(int x) {
    int c = g_mCol[clampi(x, 0, LW - 1)];
    return clampi((int)zoom_row(s_asset->mfirst[c] + 1), 0, HZ - 1);
}

static int   s_phase = 0;
static float s_pt = 0;             // seconds into the phase
// Seconds into the current scene (restarted at each scene change, in the
// dark): the sway, wind, ripples and flicker take sinf() of it, and an
// ever-growing argument made sinf() slower by the minute (and would cost
// precision in the end).
static float s_time = 0;
static float s_groundZ = 0;        // the grid's travel, 0..1
static volatile bool s_skip = false;

// Column x of the chunk being drawn: pixel y is at colp(x)[y * s_dir].
// Not flipped, panel row r is column x0 + r and runs from the picture's
// bottom to its top; flipped, row r is column x0 + 15 - r, top to bottom.
static inline uint16_t *colp(int x) {
    int xi = x - s_x0;
    return s_flip ? s_dst + (SCENE_CHUNK - 1 - xi) * LH : s_dst + xi * LH + (LH - 1);
}
static inline uint16_t *PP(int x, int y) { return colp(x) + y * s_dir; }

/* ================================================================ per-scene memory */

// Only one scene runs at a time, so their simulations share this memory
// (each scene's enter function sets its part up).  Static RAM is tight on
// the classic ESP32.
#define FIRE_WIDE 1.25f          // the fire's width against the original 56-cell design
#define FW 70                    // fire: heat field (56 * FIRE_WIDE)
#define FH 72
#define NPUFF 24
#define NEMBER 48
#define NDROP 210
#define NSPLASH 180
#define NFLAKE 340
#define NLANDED 320
static int s_nflake = NFLAKE;             // flakes in use (fewer on the larger panel)
static int s_ndrop = NDROP;               // raindrops in use (likewise)

// The smoke is composited once a frame into a coarse grid of SMOKE_G-pixel
// cells, and each pixel blends one bilinear sample of it: far cheaper than
// blending every puff at every pixel it covers.
#define SMOKE_G  4
#define SMOKE_GW (SCENE_MAX_W / SMOKE_G + 2)
#define SMOKE_GH (SCENE_MAX_H / SMOKE_G + 2)

#define NSTAR 640
// The firelight on the ground: GLOW_LEVELS colour tables -- the ground plus
// the light at 1/4, 2/4, 3/4 and full strength -- for the rows it reaches (at
// most GLOW_ROWS), by a quarter of the vertical lines' coverage steps (NQ4).
// Each column's strength falls between two levels and is ordered-dithered
// between them (2 x 2), so the falloff to the sides is smooth.
#define GLOW_LEVELS 4
#define GLOW_ROWS  112
#define NQ4        9

struct Puff { float x, y, r, vx, vy, age, life, a0, wob; uint8_t tex; bool live; };
struct Ember { float x, y, vx, vy, age, life; bool live; };
struct Drop { float x, y, sp, len, yG, a; };
struct Splash { float x, y, s, age, life, dvx[3], dvy[3]; uint8_t nd; bool live; };
struct Flake { float x, y, sp, r, amp, ph, freq, a, yG, sc, sx; };
struct Landed { float X, z, age, life, a; bool live; };

static union {
    struct {
        uint8_t heat[FH][FW];
        Puff    puff[NPUFF];
        Ember   ember[NEMBER];
        uint8_t smokeA[SMOKE_GW][SMOKE_GH];   // smoke: opacity per cell, column-major
        uint8_t smokeW[SMOKE_GW][SMOKE_GH];   // ... and how warm (lit by the fire)
        int16_t starX16[NSTAR], starY16[NSTAR];
        uint8_t starA[NSTAR];
        int16_t starHead[SCENE_MAX_W];        // per column: a list of the stars touching it
        int16_t starNext[2 * NSTAR], starOf[2 * NSTAR];
        uint16_t glowLut[GLOW_LEVELS][GLOW_ROWS * NQ4];  // the ground's colours at each firelight level
    } f;
    struct {
        Drop   drop[NDROP];
        Splash splash[NSPLASH];
    } r;
    struct {
        Flake  flake[NFLAKE];
        Landed landed[NLANDED];
        uint8_t cloud8[CLOUD_W_L * CLOUD_H_L];        // the clouds in RAM: lum << 4 | alpha, 4 bits each
    } s;
} U;
#define s_heat   U.f.heat
#define s_puff   U.f.puff
#define s_ember  U.f.ember
#define s_smokeA U.f.smokeA
#define s_smokeW U.f.smokeW
#define s_glowLut U.f.glowLut
#define s_cloud8 U.s.cloud8
#define s_starX16  U.f.starX16
#define s_starY16  U.f.starY16
#define s_starA    U.f.starA
#define s_starHead U.f.starHead
#define s_starNext U.f.starNext
#define s_starOf   U.f.starOf
#define s_drop   U.r.drop
#define s_splash U.r.splash
#define s_flake  U.s.flake
#define s_landed U.s.landed

/* ================================================================ the ground */

static int32_t  g_s[MAX_GR];       // world X per pixel at this row, 16.16
static uint32_t g_invS[MAX_GR];    // 256 / s, for the coverage
static uint8_t  g_hcov[MAX_GR];    // the cross lines' coverage of this row, 0..255
static float    g_fog[MAX_GR];
static int16_t  g_base[MAX_GR][3], g_line[MAX_GR][3];
static uint8_t  g_reflK[MAX_GR];   // storm: reflection strength, /32
static uint8_t  g_reflY[MAX_GR];   // storm: the sky row mirrored into this one, rippled
static uint32_t g_glowS[MAX_GR];   // fire: the firelight at this row, spread, at full strength
static uint16_t g_lut[MAX_GR * NQ];// this frame's ground colour by row and line coverage
static uint8_t *g_vq = nullptr;    // [|dx| * GR + j]: the converging lines' coverage, 0..32

#define WLX_F ((int32_t)(LINE_X * 65536))

static inline int32_t lineG(int32_t v) {      // integral of the lines' coverage up to v
    int32_t f = v & 0xFFFF;
    return (v >> 16) * WLX_F + (f < WLX_F ? f : WLX_F);
}
static inline float lineF(float z) {
    float fl = floorf(z), fr = z - fl;
    return fl * LINE_Z + (fr < LINE_Z ? fr : LINE_Z);
}

static void ground_tables_init() {
    for (int j = 0; j < GR; j++) {
        float dy = j + 0.5f;
        float s = KX / dy;
        g_s[j] = (int32_t)(s * 65536.0f);
        g_invS[j] = (uint32_t)(256.0f / s);
        g_fog[j] = expf(-dy / (16.0f * K));
        g_reflK[j] = (uint8_t)((70 + 110 * g_fog[j]) / 8);
    }
    // The converging lines' coverage of each pixel; symmetric about CX
    // (pixel dx and pixel -dx-1 mirror each other).  Sized once for the
    // most ground any scene has.
    if (!g_vq) g_vq = (uint8_t *)scene_alloc((size_t)(LW / 2) * (LH - (s_large ? ASSET_HZ_L : ASSET_HZ)));
    for (int dx = 0; dx < LW / 2; dx++)
        for (int j = 0; j < GR; j++) {
            int32_t s = g_s[j];
            int32_t xa = dx * s + WLX_F / 2;
            uint32_t dG = (uint32_t)(lineG(xa + s) - lineG(xa));
            int v = (int)((dG * g_invS[j]) >> 16);             // 0..256
            g_vq[dx * GR + j] = (uint8_t)((v + 4) >> 3);
        }
}

// The ground's colours for a phase: base and lines near, blending into
// `haze` toward the horizon.
static void ground_colors(const float base[3], const float line[3], const float haze[3]) {
    for (int j = 0; j < GR; j++) {
        float f = g_fog[j];
        for (int k = 0; k < 3; k++) {
            g_base[j][k] = (int16_t)(base[k] + (haze[k] - base[k]) * f);
            g_line[j][k] = (int16_t)(line[k] + (haze[k] - line[k]) * f);
        }
    }
}

static float s_flash = 0;          // storm: lightning, 0..1
// Storm: the light as the sun goes -- unchanged, then reddening, then toward
// night (per channel, /32) -- applied to the backdrop and the ground.
static int s_tintR = 32, s_tintG = 32, s_tintB = 32;

// Per frame: the cross lines' coverage of each row, then the colour table.
static void ground_step() {
    const int fl = s_phase == 1 ? (int)(s_flash * 255) : 0;
    const int lineShift = s_phase == 1 ? 9 : 8;     // wet ground: lines at half strength
    for (int j = 0; j < GR; j++) {
        float za = KZ / (j + 1.0f), zb = KZ / (j > 0 ? (float)j : 0.35f);
        za += s_groundZ; zb += s_groundZ;
        int hc = (int)clampf((lineF(zb) - lineF(za)) / (zb - za) * 255.0f, 0, 255);
        g_hcov[j] = (uint8_t)hc;
        const int16_t *b = g_base[j], *l = g_line[j];
        uint16_t *lut = &g_lut[j * NQ];
        const int d = BAYER[j & 3][(j >> 2) & 3];
        for (int q = 0; q < NQ; q++) {
            int vc = q * 8;
            int c = hc + vc - ((hc * vc) >> 8);
            int r = b[0] + (((l[0] - b[0]) * c) >> lineShift);
            int g = b[1] + (((l[1] - b[1]) * c) >> lineShift);
            int bb = b[2] + (((l[2] - b[2]) * c) >> lineShift);
            if (s_phase == 1) { r = (r * s_tintR) >> 5; g = (g * s_tintG) >> 5; bb = (bb * s_tintB) >> 5; }
            if (fl) { r += (fl * 50) >> 8; g += (fl * 48) >> 8; bb += (fl * 62) >> 8; }
            lut[q] = packd(r, g, bb, d);
        }
    }
}

/* ================================================================ fire */

#define COOL_H 128                // cooling map rows (it repeats every 4.3 s)
#define FIRE_HZ 30.0f
#define PT 32                                // puff texture size

static int     s_fireCX, s_fireBaseY;        // where the fire stands
static int     s_fireX0, s_fireY0, s_fireW, s_fireH;   // the flames' rectangle
static int32_t s_fireStep;                   // heat cells per pixel, 16.16
static uint8_t s_cool[COOL_H][FW];
static uint8_t s_fshape[FW];
static uint32_t s_palS[256];                 // flame colour by heat, spread
static int     s_coolOff = 0;
static float   s_fireAcc = 0;
static float   s_flicker = 1;               // the fire's light, about 1
static uint8_t s_glowX[SCENE_MAX_W], s_glowY[MAX_GR];
static int     s_glowJ0 = 0, s_glowJ1 = 0;      // the ground rows the firelight reaches
static uint8_t s_glowLv[SCENE_MAX_W][4];     // each column's level (0: none, 1..4) by row (y & 3)
static float   s_puffAcc = 0, s_emberAcc = 0;
static uint8_t s_puffTex[4][PT][PT];
static int s_smX0 = 0, s_smX1 = -1, s_smY0 = 0, s_smY1 = -1;   // cells in use (inclusive)
static uint32_t s_smokeS[4];                                     // colours, cool to warm
static void smoke_build();

struct Log { float ax, ay, bx, by, r; };
// Laid out for 320 x 240 with the fire at (152, 214); placed by fire_layout().
static const Log LOGS0[] = {
    { 118, 217, 180, 206, 5.0f },
    { 128, 205, 186, 219, 4.5f },
    { 146, 221, 160, 202, 4.0f },
};
#define NLOGS (int)(sizeof LOGS0 / sizeof LOGS0[0])

// The logs don't move: drawn once into a sprite -- coverage, bark colour,
// and how much each pixel glows with the fire.
struct LogPx { uint16_t bark; uint8_t cov, ember; };
static LogPx *s_logPx = nullptr;
static int    s_logX0, s_logY0, s_logW, s_logH;

static void fire_layout() {
    s_fireCX = (int)lroundf(LW * (152.0f / 320));
    s_fireBaseY = (int)lroundf(LH * (214.0f / 240));
    const float scale = 2 * K;                               // pixels per heat cell
    s_fireW = (int)lroundf(FW * scale);
    s_fireH = (int)lroundf(FH * scale);
    s_fireX0 = s_fireCX - s_fireW / 2;
    s_fireY0 = s_fireBaseY - s_fireH + (int)lroundf(6 * K);
    s_fireStep = (int32_t)(65536 / scale);
    Log logs[NLOGS];
    float xa = 1e9f, xb = -1e9f, ya = 1e9f, yb = -1e9f;
    for (int i = 0; i < NLOGS; i++) {
        const Log &L = LOGS0[i];
        logs[i] = { s_fireCX + (L.ax - 152) * K * FIRE_WIDE, s_fireBaseY + (L.ay - 214) * K,
                    s_fireCX + (L.bx - 152) * K * FIRE_WIDE, s_fireBaseY + (L.by - 214) * K, L.r * K };
        xa = fminf(xa, fminf(logs[i].ax, logs[i].bx) - logs[i].r - 1);
        xb = fmaxf(xb, fmaxf(logs[i].ax, logs[i].bx) + logs[i].r + 1);
        ya = fminf(ya, fminf(logs[i].ay, logs[i].by) - logs[i].r - 1);
        yb = fmaxf(yb, fmaxf(logs[i].ay, logs[i].by) + logs[i].r + 1);
    }
    s_logX0 = (int)floorf(xa); s_logY0 = (int)floorf(ya);
    s_logW = (int)ceilf(xb) - s_logX0 + 1; s_logH = (int)ceilf(yb) - s_logY0 + 1;
    free(s_logPx);
    s_logPx = (LogPx *)scene_alloc((size_t)s_logW * s_logH * sizeof(LogPx));
    if (s_logPx) memset(s_logPx, 0, (size_t)s_logW * s_logH * sizeof(LogPx));
    for (int k = 0; k < NLOGS && s_logPx; k++) {
        const Log &L = logs[k];
        const float ex = L.bx - L.ax, ey = L.by - L.ay, ll = 1.0f / (ex * ex + ey * ey);
        for (int x = 0; x < s_logW; x++)
            for (int y = 0; y < s_logH; y++) {
                int X = s_logX0 + x, Y = s_logY0 + y;
                float px = X + 0.5f - L.ax, py = Y + 0.5f - L.ay;
                float t = clampf((px * ex + py * ey) * ll, 0, 1);
                float qx = px - t * ex, qy = py - t * ey;
                float cov = clampf(L.r + 0.5f - sqrtf(qx * qx + qy * qy), 0, 1);
                if (cov <= 0) continue;
                float side = qy / L.r;                          // -1 top .. 1 bottom
                float lit = 0.55f - 0.45f * side;
                float e = hash2((int)(X / (2 * K)), (int)(Y / K), 77 + k) * 0.6f +
                          vnoise2(X / (3.0f * K), Y / (2.0f * K), 78 + k) * 0.6f;
                e = clampf((e - 0.62f) * 3.0f, 0, 1) * (0.6f + 0.4f * (1 - fabsf(side)));
                LogPx &p = s_logPx[x * s_logH + y];                // later logs on top
                p.bark = pack((int)(46 * lit), (int)(30 * lit), (int)(20 * lit));
                p.ember = (uint8_t)(e * 255);
                p.cov = (uint8_t)fmaxf(p.cov, cov * 32);
            }
    }
    // The fire's light on the ground: wider than deep, reaching farther
    // toward the viewer than behind the fire.
    for (int x = 0; x < LW; x++) s_glowX[x] = (uint8_t)(255 * expf(-powf((x - s_fireCX) / (95.0f * K * FIRE_WIDE), 2)));
    // Each column's strength in levels (0..4), ordered-dithered (4 x 4
    // Bayer, in 16ths of a level) between the two levels around it.
    {
        const float sg = 95.0f * K * FIRE_WIDE;
        for (int x = 0; x < LW; x++) {
            float g = 4.0f * expf(-powf((x + 0.5f - s_fireCX) / sg, 2));
            int base = (int)g, fr = (int)((g - base) * 16);
            for (int p = 0; p < 4; p++) {
                int lv = base + (fr > BAYER[p][x & 3]);
                s_glowLv[x][p] = (uint8_t)(lv > GLOW_LEVELS ? GLOW_LEVELS : lv);
            }
        }
    }
    for (int j = 0; j < GR; j++) {
        float d = HZ + j - s_fireBaseY;
        float sg = (d < 0 ? 26.0f : 46.0f) * K;
        s_glowY[j] = (uint8_t)(255 * expf(-(d / sg) * (d / sg)));
        if (s_glowY[j] >= 4) { if (!s_glowJ1) s_glowJ0 = j; s_glowJ1 = j + 1; }
        if (s_glowJ1 - s_glowJ0 > GLOW_ROWS) s_glowJ1 = s_glowJ0 + GLOW_ROWS;
        float g = s_glowY[j] / 255.0f;
        g_glowS[j] = spread(pack((int)(230 * g), (int)(100 * g), (int)(28 * g)));
    }
}

static void fire_init_tables() {
    // Palette: black body-ish, emitted light added onto the scene.
    for (int h = 0; h < 256; h++) {
        float t = h / 255.0f, r, g, b;
        if (t < 0.16f)      { r = g = b = 0; }
        else if (t < 0.40f) { float u = (t - 0.16f) / 0.24f; r = 170 * u; g = 22 * u; b = 4 * u; }
        else if (t < 0.65f) { float u = (t - 0.40f) / 0.25f; r = 170 + 85 * u; g = 22 + 118 * u; b = 4 + 6 * u; }
        else if (t < 0.85f) { float u = (t - 0.65f) / 0.20f; r = 255; g = 140 + 90 * u; b = 10 + 50 * u; }
        else                { float u = (t - 0.85f) / 0.15f; r = 255; g = 230 + 25 * u; b = 60 + 150 * u; }
        s_palS[h] = spread(pack((int)r, (int)g, (int)b));
    }
    // Cooling map: smooth noise, scrolled up with the flames so they break
    // into tongues; more at the sides so the fire tapers.
    for (int y = 0; y < COOL_H; y++)
        for (int x = 0; x < FW; x++) {
            float n = vnoise2(x / 4.0f, y / 6.0f, 3) * 0.6f + vnoise2(x / 2.0f, y / 2.5f, 4) * 0.4f;
            float e = (x - FW / 2.0f) / (FW / 2.0f);
            float c = 1.0f + 9.0f * n * n + 7.0f * e * e;
            s_cool[y][x] = (uint8_t)clampf(c, 0, 255);
        }
    // Where the fuel is: a mound with three hot spots (the logs' crossing).
    for (int x = 0; x < FW; x++) {
        float e = (x - FW / 2.0f) / (FW * 0.36f);
        float m = clampf(1 - e * e, 0, 1);
        const float u = x / FIRE_WIDE;                       // in the original 56-cell layout
        float spots = 0.75f + 0.25f * (expf(-powf((u - 22) / 4.0f, 2)) + expf(-powf((u - 31) / 3.5f, 2)) +
                                       expf(-powf((u - 27) / 5.0f, 2)));
        s_fshape[x] = (uint8_t)clampf(255 * sqrtf(m) * spots, 0, 255);
    }
    // Smoke, grey to lit orange.
    for (int k = 0; k < 4; k++) {
        float w = (k + 0.5f) / 4;
        s_smokeS[k] = spread(pack((int)(96 + 60 * w), (int)(92 + 6 * w), (int)(98 - 34 * w)));
    }
    // Smoke puff textures: soft discs with a lumpy inside.
    for (int k = 0; k < 4; k++)
        for (int v = 0; v < PT; v++)
            for (int u = 0; u < PT; u++) {
                float dx = (u + 0.5f) / (PT / 2.0f) - 1, dy = (v + 0.5f) / (PT / 2.0f) - 1;
                float fall = clampf(1 - (dx * dx + dy * dy), 0, 1);
                fall = fall * fall;
                float n = vnoise2(u / 6.0f + k * 7, v / 6.0f, 11) * 0.6f + vnoise2(u / 3.0f + k * 5, v / 3.0f, 12) * 0.4f;
                s_puffTex[k][v][u] = (uint8_t)clampf(255 * fall * (0.35f + 0.9f * n), 0, 255);
            }
}

static void fire_sim_once() {
    // Fuel rows, flickering.
    for (int x = 0; x < FW; x++)
        for (int k = 1; k <= 2; k++)
            s_heat[FH - k][x] = (uint8_t)((s_fshape[x] * (150 + (rnd() % 106))) >> 8);
    s_coolOff = (s_coolOff + 1) & (COOL_H - 1);
    // Upward: each cell from those below it, less the cooling there.  Rows
    // are updated top-down so the rows read are still last step's.
    const int sway = (int)lroundf(sinf(s_time * 0.9f) * 0.6f + sinf(s_time * 2.3f) * 0.3f);
    for (int y = 0; y < FH - 2; y++) {
        int sh = sway;
        uint32_t r = rnd();
        if ((r & 7) == 0) sh += (r & 8) ? 1 : -1;
        const uint8_t *cool = s_cool[(y + s_coolOff) & (COOL_H - 1)];
        for (int x = 1; x < FW - 1; x++) {
            int xs = clampi(x + sh, 1, FW - 2);
            int v = (s_heat[y + 1][xs - 1] + s_heat[y + 1][xs] + s_heat[y + 1][xs + 1] + s_heat[y + 2][xs]) >> 2;
            v -= cool[x];
            s_heat[y][x] = (uint8_t)(v < 0 ? 0 : v);
        }
        s_heat[y][0] = s_heat[y][FW - 1] = 0;
    }
}

static void puffs_step(float dt);

static void fire_enter() {
    memset(s_heat, 0, sizeof s_heat);
    memset(s_smokeA, 0, sizeof s_smokeA);
    memset(s_smokeW, 0, sizeof s_smokeW);
    s_smX0 = 0; s_smX1 = -1; s_smY0 = 0; s_smY1 = -1;
    for (int i = 0; i < NPUFF; i++) s_puff[i].live = false;
    for (int i = 0; i < NEMBER; i++) s_ember[i].live = false;
    for (int i = 0; i < 90; i++) fire_sim_once();       // already burning
    s_fireAcc = 0;
    for (int i = 0; i < 40; i++) puffs_step(0.15f);    // and smoke already rising
}

static void puffs_step(float dt) {
    s_puffAcc += dt;
    while (s_puffAcc > 0.19f) {
        s_puffAcc -= 0.19f;
        for (int i = 0; i < (s_large ? 18 : NPUFF); i++) {   // fewer on the larger panel: its frame budget is tight
            Puff &p = s_puff[i];
            if (p.live) continue;
            p.live = true;
            p.x = s_fireCX + frand(-14, 14) * K;
            p.y = s_fireBaseY - frand(62, 84) * K;
            p.r = frand(6, 10) * K;
            p.vx = frand(3, 9) * K;
            p.vy = -frand(16, 24) * K;
            p.age = 0;
            p.life = frand(5.0f, 7.0f);
            p.a0 = frand(0.60f, 0.85f);
            p.wob = frand(0, 6.28f);
            p.tex = rnd() & 3;
            break;
        }
    }
    for (int i = 0; i < NPUFF; i++) {
        Puff &p = s_puff[i];
        if (!p.live) continue;
        p.age += dt;
        p.x += (p.vx + (5.0f * sinf(p.age * 0.8f + p.wob) + p.age * 2.0f) * K) * dt;
        p.y += p.vy * dt;
        p.vy *= 1.0f - 0.10f * dt;
        float grow = 5.0f - p.age * 0.45f;
        p.r += (grow > 1.2f ? grow : 1.2f) * K * dt;
        if (p.age >= p.life || p.y + p.r < 0) p.live = false;
    }
}

static void embers_step(float dt) {
    s_emberAcc += dt * 22;
    while (s_emberAcc >= 1) {
        s_emberAcc -= 1;
        for (int i = 0; i < NEMBER; i++) {
            Ember &e = s_ember[i];
            if (e.live) continue;
            e.live = true;
            e.x = s_fireCX + frand(-25, 25) * K;
            e.y = s_fireBaseY - frand(4, 30) * K;
            e.vx = frand(-10, 10) * K;
            e.vy = -frand(30, 75) * K;
            e.age = 0;
            e.life = frand(0.7f, 2.2f);
            break;
        }
    }
    for (int i = 0; i < NEMBER; i++) {
        Ember &e = s_ember[i];
        if (!e.live) continue;
        e.age += dt;
        e.vx += frand(-90, 90) * K * dt;
        e.vx *= 1.0f - 1.5f * dt;
        e.vy *= 1.0f - 0.6f * dt;
        e.x += (e.vx + 6 * K) * dt;
        e.y += e.vy * dt;
        if (e.age >= e.life || e.y < 0) e.live = false;
    }
}

static void fire_step(float dt) {               // (the smoke is the helper's: scene_step_helper())
    s_fireAcc += dt;
    int n = 0;
    while (s_fireAcc >= 1.0f / FIRE_HZ && n < 4) { s_fireAcc -= 1.0f / FIRE_HZ; fire_sim_once(); n++; }
    if (n == 4) s_fireAcc = 0;
    embers_step(dt);
    s_flicker = 0.80f + 0.12f * vnoise1(s_time * 7.0f, 21) + 0.10f * vnoise1(s_time * 17.0f, 22);
}

static void glow_tables() {
    // Fire: each band's table -- the ground plus the firelight at the band's
    // strength -- for the rows the light reaches.
    {
        for (int k = 0; k < GLOW_LEVELS; k++) {
            const float lv = (k + 1) / (float)GLOW_LEVELS * s_flicker;
            for (int j = s_glowJ0, r = 0; j < s_glowJ1; j++, r++) {
                const float gy = s_glowY[j] * lv / 255.0f;
                const int gr = (int)(230 * gy), gg = (int)(100 * gy), gb = (int)(28 * gy);
                const uint16_t *src = &g_lut[j * NQ];
                uint16_t *dst = &s_glowLut[k][r * NQ4];
                const int dd = BAYER[j & 3][(k + j) & 3];
                for (int q = 0; q < NQ4; q++) {
                    uint16_t c = src[q * 4];
                    dst[q] = packd(R8(c) + gr, G8(c) + gg, B8(c) + gb, dd);
                }
            }
        }
    }
}

HOT static void draw_logs() {
    if (!s_logPx) return;
    const int xa = s_x0 > s_logX0 ? s_x0 : s_logX0;
    const int xb = s_x0 + SCENE_CHUNK < s_logX0 + s_logW ? s_x0 + SCENE_CHUNK : s_logX0 + s_logW;
    const int fl = (int)(s_flicker * 256);
    const int d = s_dir;
    for (int x = xa; x < xb; x++) {
        const LogPx *lp = &s_logPx[(x - s_logX0) * s_logH];
        uint16_t *col = colp(x);
        for (int i = 0; i < s_logH; i++) {
            const LogPx &p = lp[i];
            if (!p.cov) continue;
            int y = s_logY0 + i;
            if ((unsigned)y >= (unsigned)LH) continue;
            int e = (p.ember * fl) >> 8;
            uint16_t c = add(p.bark, e, (e * 85) >> 8, (e * 15) >> 8);
            uint16_t &px = col[y * d];
            px = SW(blend(SW(px), spread(c), p.cov));
        }
    }
}


// Composite the puffs into the grid (in scene_step, once a frame).
static void smoke_build() {
    for (int cx = s_smX0; cx <= s_smX1; cx++) {
        memset(&s_smokeA[cx][s_smY0], 0, s_smY1 - s_smY0 + 1);
        memset(&s_smokeW[cx][s_smY0], 0, s_smY1 - s_smY0 + 1);
    }
    const int GW = LW / SMOKE_G + 1, GH = LH / SMOKE_G + 1;
    int x0 = GW, x1 = -1, y0 = GH, y1 = -1;
    for (int i = 0; i < NPUFF; i++) {
        const Puff &p = s_puff[i];
        if (!p.live) continue;
        float fade = clampf(p.age / 0.8f, 0, 1);
        float left = clampf(1 - p.age / p.life, 0, 1);
        fade *= left * (0.6f + 0.4f * left);
        int a8 = (int)(p.a0 * fade * 255);
        if (a8 <= 0) continue;
        const int warm = (int)(255 * expf(-p.age / 1.1f));
        // Cell (cx, cy) samples the pixel at its corner (cx * G, cy * G).
        int ca = clampi((int)floorf((p.x - p.r) / SMOKE_G), 0, GW - 1);
        int cb = clampi((int)ceilf((p.x + p.r) / SMOKE_G), 0, GW - 1);
        int ra = clampi((int)floorf((p.y - p.r) / SMOKE_G), 0, GH - 1);
        int rb = clampi((int)ceilf((p.y + p.r) / SMOKE_G), 0, GH - 1);
        const float k = (PT / 2.0f) / p.r;
        const uint8_t *tex = &s_puffTex[p.tex][0][0];
        for (int cx = ca; cx <= cb; cx++) {
            int u = (int)((cx * SMOKE_G - p.x) * k + PT / 2);
            if ((unsigned)u >= PT) continue;
            for (int cy = ra; cy <= rb; cy++) {
                int v = (int)((cy * SMOKE_G - p.y) * k + PT / 2);
                if ((unsigned)v >= PT) continue;
                int a = (tex[v * PT + u] * a8) >> 8;
                if (a <= 0) continue;
                uint8_t &A = s_smokeA[cx][cy], &W = s_smokeW[cx][cy];
                W = (uint8_t)(W + (((warm - W) * a) >> 8));
                A = (uint8_t)(A + ((a * (255 - A)) >> 8));      // "over"
                if (cx < x0) x0 = cx;
                if (cx > x1) x1 = cx;
                if (cy < y0) y0 = cy;
                if (cy > y1) y1 = cy;
            }
        }
    }
    s_smX0 = x0; s_smX1 = x1; s_smY0 = y0; s_smY1 = y1 < GH - 2 ? y1 + 1 : y1;
    if (x1 >= 0 && x1 < GW - 2) s_smX1 = x1 + 1;
}

HOT static void draw_puffs() {
    if (s_smX1 < s_smX0) return;
    const int xa = s_x0 > s_smX0 * SMOKE_G ? s_x0 : s_smX0 * SMOKE_G;
    const int xb = s_x0 + SCENE_CHUNK < s_smX1 * SMOKE_G ? s_x0 + SCENE_CHUNK : s_smX1 * SMOKE_G;
    const int ya = s_smY0 * SMOKE_G, yb = s_smY1 * SMOKE_G < LH ? s_smY1 * SMOKE_G : LH;
    const int dir = s_dir;
    uint8_t ca[SMOKE_GH], cw[SMOKE_GH];
    for (int x = xa; x < xb; x++) {
        const int cx = x / SMOKE_G, fx = x % SMOKE_G;            // 0..3 of 4
        const uint8_t *A0 = s_smokeA[cx], *A1 = s_smokeA[cx + 1];
        const uint8_t *W0 = s_smokeW[cx], *W1 = s_smokeW[cx + 1];
        int any = 0;
        for (int cy = s_smY0; cy <= s_smY1; cy++) {
            int a = A0[cy] + (((A1[cy] - A0[cy]) * fx) >> 2);
            ca[cy] = (uint8_t)a;
            cw[cy] = (uint8_t)(W0[cy] + (((W1[cy] - W0[cy]) * fx) >> 2));
            any |= a;
        }
        if (!any) continue;
        uint16_t *col = colp(x);
        const int bx = x & 3;
        for (int y = ya; y < yb; y++) {
            const int cy = y / SMOKE_G, fy = y % SMOKE_G;
            const int a0 = ca[cy], a1 = ca[cy + 1];
            if (!(a0 | a1)) { y |= SMOKE_G - 1; continue; }     // skip to the next cell
            int a = a0 + (((a1 - a0) * fy) >> 2);
            a = (a + BAYER[y & 3][bx] * 2) >> 3;                 // 32nds, dithered
            if (a <= 0) continue;
            int w = cw[cy] + (((cw[cy + 1] - cw[cy]) * fy) >> 2);
            uint16_t &px = col[y * dir];
            px = SW(blend(SW(px), s_smokeS[w >> 6], (uint32_t)(a > 32 ? 32 : a)));
        }
    }
}

// The flames: the heat field interpolated across once per column, then
// down, and added to the scene.
HOT static void draw_flames() {
    const int x1 = s_x0 + SCENE_CHUNK;
    const int xe = s_fireX0 + s_fireW;
    const int dir = s_dir;
    uint16_t hcol[FH];
    for (int x = s_x0 > s_fireX0 ? s_x0 : s_fireX0; x < x1 && x < xe; x++) {
        int32_t gx = (int32_t)((x - s_fireX0) * s_fireStep + s_fireStep / 2) - 32768;
        if (gx < 0) gx = 0;
        int ix = gx >> 16, fx = (gx >> 8) & 255;
        int ix1 = ix + 1 < FW ? ix + 1 : ix;
        int any = 0;
        for (int i = 0; i < FH; i++) {
            int a = s_heat[i][ix], b = s_heat[i][ix1];
            int v = a * 256 + (b - a) * fx;                  // heat * 256
            hcol[i] = (uint16_t)v;
            any |= v;
        }
        if ((any >> 8) < 42) continue;
        uint16_t *col = colp(x);
        int32_t gy = s_fireStep / 2 - 32768;
        for (int i = 0; i < s_fireH; i++, gy += s_fireStep) {
            int iy = gy < 0 ? 0 : gy >> 16, fy = gy < 0 ? 0 : (gy >> 8) & 255;
            int a = hcol[iy], b = hcol[iy + 1 < FH ? iy + 1 : iy];
            if ((a | b) < 42 * 256) continue;
            int v = (a + (((b - a) * fy) >> 8)) >> 8;
            if (v < 42) continue;
            int y = s_fireY0 + i;
            if ((unsigned)y >= (unsigned)LH) continue;
            uint16_t &px = col[y * dir];
            px = SW(unspread(sat(spread(SW(px)) + s_palS[v])));
        }
    }
}

HOT static void draw_embers() {
    const int x1 = s_x0 + SCENE_CHUNK;
    for (int i = 0; i < NEMBER; i++) {
        const Ember &e = s_ember[i];
        if (!e.live) continue;
        int x = (int)e.x, y = (int)e.y;
        if (x < s_x0 || x >= x1 || (unsigned)y >= (unsigned)LH) continue;
        float f = e.age / e.life, k = 1 - f;
        uint16_t *p = PP(x, y);
        *p = SW(add(SW(*p), (int)(255 * k), (int)((200 - 150 * f) * k), (int)(90 * k * (1 - f))));
    }
}

/* ================================================================ storm */

#define WIND 0.16f               // rain: x per y

struct Seg { int16_t x0, y0, x1, y1; uint8_t main; };
#define NSEG 120
static Seg   s_seg[NSEG];
static int   s_nseg = 0;
static int   s_splashNext = 0;
static float s_strikeT = -1;     // seconds since the strike began, <0: none
static float s_nextStrike = 2;
#define RING 32
static float s_ringC[RING], s_ringS[RING];   // a unit circle, for the splashes

static void drop_new(Drop &d, bool anywhere) {
    float dy = 2 + (GR - 2) * frand(0, 1);
    float sc = dy / GR;
    d.yG = HZ + dy;
    d.sp = (230 + 620 * sc) * K;
    d.len = (3 + 15 * sc) * K;
    d.a = 0.18f + 0.45f * sc;
    d.y = anywhere ? frand(-d.len, d.yG) : -frand(0, 60 * K);
    d.x = frand(-50 * K, LW + 4);
}

static void splash_new(float x, float y, float sc) {
    Splash &s = s_splash[s_splashNext];
    s_splashNext = (s_splashNext + 1) % NSPLASH;
    s.live = true;
    s.x = x; s.y = y; s.s = sc;
    s.age = 0;
    s.life = 0.16f + 0.16f * sc;
    s.nd = sc > 0.35f ? 2 + (rnd() % 2) : (sc > 0.15f ? 1 : 0);
    for (int i = 0; i < s.nd; i++) {
        s.dvx[i] = frand(-1, 1) * (10 + 40 * sc) * K;
        s.dvy[i] = frand(0.6f, 1.0f) * (20 + 70 * sc) * K;
    }
}

static void bolt_make() {
    s_nseg = 0;
    float x = frand(30 * K, LW - 30 * K), y = frand(2, 10) * K;
    const float drift = frand(-2.5f, 2.5f) * K;
    struct B { float x, y, dir; int left; };
    B branches[6];
    int nb = 0;
    while (s_nseg < NSEG - 20) {
        int top = mountain_top((int)x);
        if (y >= top) break;
        float ny = y + frand(3, 9) * K, nx = x + drift + frand(-6, 6) * K;
        int ntop = mountain_top((int)nx);
        if (ny > ntop) ny = (float)ntop;
        s_seg[s_nseg++] = { (int16_t)x, (int16_t)y, (int16_t)nx, (int16_t)ny, 1 };
        if (nb < 6 && frand(0, 1) < 0.22f) branches[nb++] = { nx, ny, frand(0, 1) < 0.5f ? -1.0f : 1.0f, 3 + (int)(rnd() % 6) };
        x = nx; y = ny;
    }
    for (int b = 0; b < nb && s_nseg < NSEG; b++) {
        float bx = branches[b].x, by = branches[b].y;
        for (int k = 0; k < branches[b].left && s_nseg < NSEG; k++) {
            float ny = by + frand(2, 7) * K, nx = bx + branches[b].dir * frand(2, 8) * K;
            if (ny >= mountain_top((int)nx)) break;
            s_seg[s_nseg++] = { (int16_t)bx, (int16_t)by, (int16_t)nx, (int16_t)ny, 0 };
            bx = nx; by = ny;
        }
    }
}

// A strike's brightness: three return strokes and an afterglow.
static float strike_flash(float t) {
    static const float P[][3] = { { 0.00f, 0.06f, 1.0f }, { 0.10f, 0.05f, 0.55f }, { 0.19f, 0.10f, 0.9f } };
    float f = 0;
    for (auto &p : P) {
        float u = t - p[0];
        if (u >= 0 && u < p[1] * 3) f = fmaxf(f, p[2] * expf(-u / p[1]));
    }
    if (t > 0.19f) f = fmaxf(f, 0.18f * expf(-(t - 0.19f) / 0.35f));
    return f;
}

static void storm_enter() {
    s_ndrop = s_large ? 170 : NDROP;
    for (int i = 0; i < s_ndrop; i++) drop_new(s_drop[i], true);
    for (int i = 0; i < NSPLASH; i++) s_splash[i].live = false;
    s_strikeT = -1;
    s_nextStrike = frand(1.5f, 3.0f);
    s_flash = 0;
}

static void storm_step(float dt) {
    for (int i = 0; i < s_ndrop; i++) {
        Drop &d = s_drop[i];
        d.y += d.sp * dt;
        d.x += WIND * d.sp * dt;
        if (d.y >= d.yG) {
            float over = d.y - d.yG;
            splash_new(d.x - WIND * over, d.yG, (d.yG - HZ) / GR);
            drop_new(d, false);
        }
    }
    for (int i = 0; i < NSPLASH; i++) {
        Splash &s = s_splash[i];
        if (!s.live) continue;
        s.age += dt;
        if (s.age >= s.life) s.live = false;
    }
    s_nextStrike -= dt;
    if (s_nextStrike <= 0) {
        bolt_make();
        s_strikeT = 0;
        s_nextStrike = frand(2.0f, 4.5f);
    }
    if (s_strikeT >= 0) {
        s_strikeT += dt;
        s_flash = strike_flash(s_strikeT);
        if (s_strikeT > 0.3f && s_flash < 0.02f) { s_strikeT = -1; s_flash = 0; }
    }
    for (int j = 0; j < GR; j++) {
        float d = (float)j / GR;
        int rip = (int)lroundf((sinf(s_time * 9.0f + j * 0.9f / K) * (0.4f + 1.6f * d) +
                                sinf(s_time * 5.3f - j * 0.47f / K) * (0.3f + 0.9f * d)) * K);
        g_reflY[j] = (uint8_t)clampi(HZ - 1 - j + rip, 0, HZ - 1);
    }
}

HOT static inline void plot_i(int x, int y, uint32_t cS, int a32) {   // a32: 0..32
    if (x < s_x0 || x >= s_x0 + SCENE_CHUNK || (unsigned)y >= (unsigned)LH || a32 <= 0) return;
    uint16_t *p = PP(x, y);
    *p = SW(blend(SW(*p), cS, (uint32_t)(a32 > 32 ? 32 : a32)));
}
HOT static inline void plot_a(int x, int y, uint32_t cS, float a) { plot_i(x, y, cS, (int)(a * 32 + 0.5f)); }

HOT static void draw_bolt() {
    if (s_strikeT < 0 || s_strikeT > 0.33f || s_flash < 0.25f) return;
    const uint32_t core = spread(pack(255, 255, 255)), glow = spread(pack(190, 170, 255));
    const bool wide = K > 1.2f;
    for (int i = 0; i < s_nseg; i++) {
        const Seg &s = s_seg[i];
        int lo = s.x0 < s.x1 ? s.x0 : s.x1, hi = s.x0 < s.x1 ? s.x1 : s.x0;
        if (hi + 3 < s_x0 || lo - 3 >= s_x0 + SCENE_CHUNK) continue;
        int dx = s.x1 - s.x0, dy = s.y1 - s.y0;
        int n = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
        if (n == 0) n = 1;
        int a = (int)((s.main ? s_flash : s_flash * 0.6f) * 32);
        for (int k = 0; k <= n; k++) {
            int x = s.x0 + dx * k / n, y = s.y0 + dy * k / n;
            plot_i(x - 1, y, glow, (a * 15) >> 5);
            plot_i(x + 1, y, glow, (a * 15) >> 5);
            if (s.main) { plot_i(x - 2, y, glow, (a * 5) >> 5); plot_i(x + 2, y, glow, (a * 5) >> 5); }
            plot_i(x, y, core, a);
            if (wide && s.main) plot_i(x + 1, y, core, (a * 19) >> 5);
        }
    }
}

HOT static void draw_rain() {
    const int x1 = s_x0 + SCENE_CHUNK;
    const uint32_t cS = spread(pack(196, 206, 228));
    for (int i = 0; i < s_ndrop; i++) {
        const Drop &d = s_drop[i];
        float xt = d.x - WIND * d.len;
        if (d.x + 1 < s_x0 || xt - 1 >= x1) continue;
        int n = (int)d.len;
        // Fixed point along the streak: x in 1/256 px.
        int32_t px = (int32_t)(d.x * 256), step = (int32_t)(WIND * 256);
        int py = (int)d.y;
        const int a0 = (int)(d.a * 32 * 256);
        for (int k = 0; k <= n; k++, px -= step, py--) {
            if (py < 0) break;
            if (py > d.yG) continue;
            int ix = px >> 8, fx = px & 255;
            int a = (a0 - a0 * 3 * k / (4 * (n + 1))) >> 8;     // fading toward the tail
            plot_i(ix, py, cS, (a * (256 - fx)) >> 8);
            plot_i(ix + 1, py, cS, (a * fx) >> 8);
        }
    }
}

HOT static void draw_splashes() {
    const int x1 = s_x0 + SCENE_CHUNK;
    const uint32_t cS = spread(pack(205, 215, 235));
    for (int i = 0; i < NSPLASH; i++) {
        const Splash &s = s_splash[i];
        if (!s.live) continue;
        float f = s.age / s.life;
        float r = ((1.0f + 6.5f * s.s) * sqrtf(f) + 0.5f) * K;
        if (s.x + r + 14 * K < s_x0 || s.x - r - 14 * K >= x1) continue;
        int a = (int)((1 - f) * (0.22f + 0.38f * s.s) * 32 + 0.5f);
        float ry = r * 0.28f;
        int pts = 6 + (int)(16 * s.s * K);
        if (pts > RING) pts = RING;
        int stepq = (RING << 8) / pts;
        for (int k = 0, q = 0; k < pts; k++, q += stepq) {
            int idx = q >> 8;
            plot_i((int)(s.x + r * s_ringC[idx]), (int)(s.y + ry * s_ringS[idx]), cS, a);
        }
        float t = s.age;
        for (int k = 0; k < s.nd; k++) {
            float py = s.y - (s.dvy[k] * t - 0.5f * (300 + 500 * s.s) * K * t * t);
            if (py > s.y) continue;
            plot_i((int)(s.x + s.dvx[k] * t), (int)py, cS, a + (a >> 2));
        }
    }
}

/* ================================================================ snow */

static int   s_landedNext = 0;
static float s_wind = 0;

static void flake_new(Flake &f, bool anywhere) {
    float dy = 2 + (GR - 2) * powf(frand(0, 1), 1.15f);
    f.sc = dy / GR;
    f.yG = HZ + dy;
    f.r = (0.45f + 2.3f * f.sc) * K;
    f.sp = (9 + 52 * f.sc) * K;
    f.amp = (1.2f + 8 * f.sc) * K;
    f.freq = frand(0.5f, 1.6f);
    f.ph = frand(0, 6.28f);
    f.a = 0.5f + 0.5f * f.sc;
    f.y = anywhere ? frand(-4, f.yG) : -frand(2, 30) * K;
    f.x = frand(-40 * K, LW + 40 * K);
    f.sx = f.x;
}

static void snow_enter() {
    s_nflake = s_large ? 300 : NFLAKE;
    for (int i = 0; i < s_nflake; i++) flake_new(s_flake[i], true);
    for (int i = 0; i < NLANDED; i++) s_landed[i].live = false;
}

static void snow_step(float dt) {
    s_wind = (10 * sinf(s_time * 0.37f) + 6 * sinf(s_time * 1.13f + 1) + 4) * K;
    for (int i = 0; i < s_nflake; i++) {
        Flake &f = s_flake[i];
        f.y += f.sp * dt;
        f.x += s_wind * (0.3f + f.sc) * dt;
        f.sx = f.x + f.amp * sinf(s_time * f.freq + f.ph);     // where it's drawn
        if (f.y >= f.yG) {
            Landed &l = s_landed[s_landedNext];
            s_landedNext = (s_landedNext + 1) % NLANDED;
            float dy = f.yG - HZ;
            l.live = true;
            l.z = KZ / dy;
            l.X = (f.sx - CX) * KX / dy;
            l.age = 0;
            l.life = frand(1.5f, 3.0f);
            l.a = f.a;
            flake_new(f, false);
        }
        if (f.x < -60 * K) f.x += LW + 100 * K;
        if (f.x > LW + 60 * K) f.x -= LW + 100 * K;
    }
    for (int i = 0; i < NLANDED; i++) {
        Landed &l = s_landed[i];
        if (!l.live) continue;
        l.age += dt;
        l.z -= GROUND_SPEED * dt;
        if (l.age >= l.life || l.z <= KZ / (GR + 3)) l.live = false;
    }
}

// A soft dot of radius r: coverage from the squared distance (no root):
// near the edge, r - d ~ (r^2 - d^2) / 2r.
HOT static void soft_dot(float cx, float cy, float r, float a, uint32_t cS) {
    int xa = (int)floorf(cx - r), xb = (int)floorf(cx + r);
    if (xb < s_x0 || xa >= s_x0 + SCENE_CHUNK) return;
    if (r < 0.75f) {                                   // too small for a disc: one pixel
        plot_a((int)floorf(cx), (int)floorf(cy), cS, a * (0.45f + 0.7f * r));
        return;
    }
    int ya = (int)floorf(cy - r), yb = (int)floorf(cy + r);
    const float r2 = (r + 0.3f) * (r + 0.3f), inv = 1.0f / (2 * r);
    for (int x = xa; x <= xb; x++) {
        if (x < s_x0 || x >= s_x0 + SCENE_CHUNK) continue;
        float dx = x + 0.5f - cx;
        for (int y = ya; y <= yb; y++) {
            float dy = y + 0.5f - cy;
            float c = (r2 - dx * dx - dy * dy) * inv;
            if (c <= 0) continue;
            plot_a(x, y, cS, a * (c > 1 ? 1 : c));
        }
    }
}

HOT static void draw_snow() {
    const uint32_t cS = spread(pack(252, 253, 255));
    const int x1 = s_x0 + SCENE_CHUNK;
    for (int i = 0; i < NLANDED; i++) {
        const Landed &l = s_landed[i];
        if (!l.live) continue;
        float dy = KZ / l.z;
        float x = CX + l.X * dy / KX;
        if (x + 4 * K < s_x0 || x - 4 * K >= x1) continue;
        float fade = clampf(1 - l.age / l.life, 0, 1);
        soft_dot(x, HZ + dy, (0.45f + 2.3f * dy / GR) * 0.75f * K, l.a * fade * 0.9f, cS);
    }
    for (int i = 0; i < s_nflake; i++) {
        const Flake &f = s_flake[i];
        if (f.sx + f.r + 1 < s_x0 || f.sx - f.r - 1 >= x1) continue;
        soft_dot(f.sx, f.y, f.r, f.a, cS);
    }
}

/* ---------------------------------------------------------------- the backdrop */

// Storm: the sun, setting.  Its disc and inner glow are added where the
// clouds (CLOUDA_STORM) don't cover them, through a table indexed by the
// squared distance from its centre.
#define SUN_LUT 1024
static uint32_t s_sunMoonLut[1024];    // the sun's, or (fire) the moon's halo: one scene at a time
#define s_sunLut s_sunMoonLut
static int      s_sunSh = 0;            // squared distance (1/256 px^2) >> (8 + this) = index
static int32_t  s_sunX16, s_sunY16;     // centre, 1/16 px
static int      s_sunRB = 0;            // radius of the box drawn in
static const uint8_t *s_cloudA = nullptr;

static void sun_init() {
    const float SYK = HZ / 128.0f;
    const float R = 19 * SYK, tau = 26 * SYK;
    s_sunRB = (int)(1.8f * tau);
    s_sunSh = 0;
    while ((s_sunRB * s_sunRB >> s_sunSh) >= SUN_LUT) s_sunSh++;
    const float edge = expf(-s_sunRB / tau);
    for (int i = 0; i < SUN_LUT; i++) {
        float d = sqrtf((float)((i << s_sunSh) + (1 << s_sunSh) / 2));
        float g = fmaxf(0, (expf(-d / tau) - edge) / (1 - edge)) * 170;
        float disc = clampf(R + 0.5f - d, 0, 1);
        // Added to an orange sky: a disc of (235, 175, 90) comes out a warm yellow.
        s_sunLut[i] = spread(pack((int)(g + 235 * disc), (int)(g * 0.62f + 175 * disc),
                                  (int)(g * 0.28f + 90 * disc)));
    }
    s_cloudA = s_large ? CLOUDA_STORM_L : CLOUDA_STORM;
}


// The sun sets along a bearing of 120 degrees (0 up, 180 straight down):
// down and to the right, as from mid-northern latitudes facing west.  Its
// centre reaches the horizon at SUN_SET_AT of the scene, at 0.62 of the
// width -- where gen_assets.py puts the glow.
#define SUN_SET_AT 0.70f
static void sun_step() {
    const float SYK = HZ / 128.0f, t = s_pt / SCENE_PHASE_SECONDS;
    const float y0 = 40 * SYK;
    const float y = y0 + (HZ - y0) * (t / SUN_SET_AT);
    const float dir = 0.8660254f / 0.5f;                // sin 120 / -cos 120: x per y
    const float x = 0.62f * LW + (y - HZ) * dir;
    s_sunX16 = (int32_t)(x * 16);
    s_sunY16 = (int32_t)(y * 16);
    // 1 until 0.35; to red (1, 0.68, 0.55) at the setting; to night
    // (0.42, 0.30, 0.45) at the end.
    float r = 1, g = 1, b = 1;
    if (t > 0.35f) {
        float u = clampf((t - 0.35f) / (SUN_SET_AT - 0.35f), 0, 1);
        r = 1; g = 1 - 0.32f * u; b = 1 - 0.45f * u;
        if (t > SUN_SET_AT) {
            float v = clampf((t - SUN_SET_AT) / (1 - SUN_SET_AT), 0, 1);
            r += (0.42f - r) * v; g += (0.30f - g) * v; b += (0.45f - b) * v;
        }
    }
    s_tintR = (int)(r * 32 + 0.5f); s_tintG = (int)(g * 32 + 0.5f); s_tintB = (int)(b * 32 + 0.5f);
}

// The tint as two tables on a 565 pixel's bytes: per channel scaling is
// linear, so tint(c) = s_tintHi[c >> 8] + s_tintLo[c & 255] -- green, split
// across the bytes, adds up within its own field.  Rebuilt each frame.
// The lightning folds in too: a flash is a blend toward a pale lightning
// colour (c * (1 - k) + k * flash), also linear per channel, so one pair of
// tables does the tint, the flash, or both.  Each channel's scale and offset
// keep it within its field, so the two halves of green never carry.
static uint16_t s_tintHi[256], s_tintLo[256];
static bool     s_tintOn = false;
static void tint_tables() {
    const float k = clampf(s_flash * 0.6f, 0, 0.9f);
    const float mr = s_tintR / 32.0f * (1 - k), mg = s_tintG / 32.0f * (1 - k), mb = s_tintB / 32.0f * (1 - k);
    const float ar = k * 26, ag = k * 54, ab = k * 30;           // the flash's colour in 5/6/5 steps
    s_tintOn = k > 0 || (s_tintR & s_tintG & s_tintB) != 32;
    for (int i = 0; i < 256; i++) {
        const int r = i >> 3, gh = (i & 7) << 3;                 // high byte: R5, G bits 5..3
        const int gl = i >> 5, b = i & 31;                        // low byte: G bits 2..0, B5
        const int R = (int)(r * mr + ar), GH = (int)(gh * mg + ag);
        const int GL = (int)(gl * mg), B = (int)(b * mb + ab);
        s_tintHi[i] = (uint16_t)((R > 31 ? 31 : R) << 11 | (GH > 63 ? 63 : GH) << 5);
        s_tintLo[i] = (uint16_t)(GL << 5 | (B > 31 ? 31 : B));
    }
}


HOT static void draw_sun(int x, uint16_t *tmp) {
    const int cx = s_sunX16 >> 4, cy = s_sunY16 >> 4;
    if (x < cx - s_sunRB || x > cx + s_sunRB) return;
    const int y0 = cy - s_sunRB < 0 ? 0 : cy - s_sunRB;
    const int y1 = cy + s_sunRB >= HZ ? HZ - 1 : cy + s_sunRB;
    const int32_t ddx = x * 16 + 8 - s_sunX16, dx2 = ddx * ddx;
    const uint8_t *ca = s_cloudA + x * HZ;
    const int sh = 8 + s_sunSh;
    for (int y = y0; y <= y1; y++) {
        int32_t ddy = y * 16 + 8 - s_sunY16;
        uint32_t i = (uint32_t)(dx2 + ddy * ddy) >> sh;
        if (i >= SUN_LUT) continue;
        int clear = 255 - ca[y];
        if (clear <= 8) continue;
        tmp[y] = unspread(sat(spread(tmp[y]) + scaleS(s_sunLut[i], (uint32_t)(clear + 4) >> 3)));
    }
}

// Snow: clouds drifting from right to left, one of three strips each time.
#define CLOUD_SPEED 9.0f         // pixels a second, at 320 x 240
static const uint16_t *s_cloud = nullptr;
static int      s_cloudW = 0, s_cloudH = 0, s_cloudSet = 0;
static int      s_cloudRows = 0;        // picture rows the clouds are drawn in (the top of the sky)
static float    s_cloudOff = 0;
static uint32_t s_cloudLut[256];

static void clouds_init() {
    for (int l = 0; l < 256; l++) s_cloudLut[l] = spread(pack(l * 245 / 255, l * 248 / 255, l));
    s_cloudW = s_large ? CLOUD_W_L : CLOUD_W;
    s_cloudH = s_large ? CLOUD_H_L : CLOUD_H;
    // The strip covers the top 0.62 of the sky (at its normal horizon); drawn
    // in the top 0.25 of the picture only, for the frame rate.
    s_cloudRows = (int)(LH * 0.25f) & ~1;
    if (s_cloudRows > s_cloudH * 2) s_cloudRows = s_cloudH * 2;
}

static uint8_t s_cloudLum4[16];

// The scene's strip, copied into RAM at 4 bits of luminance and 4 of
// coverage: read from flash every frame, it cost several times as much.
static void clouds_enter() {
    s_cloud = (s_large ? CLOUDS_L : CLOUDS)[s_cloudSet];
    s_cloudSet = (s_cloudSet + 1) % 3;
    s_cloudOff = frand(0, (float)(2 * s_cloudW));
    for (int i = 0; i < 16; i++) s_cloudLum4[i] = (uint8_t)(150 + i * 100 / 15);
    const int n = s_cloudW * s_cloudH;
    for (int i = 0; i < n; i++) {
        int v = s_cloud[i], l = clampi(((v >> 8) - 150) * 15 / 100, 0, 15);
        int a = ((v & 255) + 8) / 17;
        s_cloud8[i] = (uint8_t)(l << 4 | (a > 15 ? 15 : a));
    }
}

// The strip is stored at half resolution: each strip row covers two picture
// rows (the clouds are soft), interpolated across once for both; per pixel
// there's only the dithered blend.
HOT static void draw_clouds(int x, uint16_t *tmp, int yS) {
    float u = (x + s_cloudOff) * 0.5f;
    int c0 = (int)u;
    const int fu = (int)((u - c0) * 256);
    c0 %= s_cloudW;
    const int c1 = c0 + 1 < s_cloudW ? c0 + 1 : 0;
    const uint8_t *p0 = s_cloud8 + c0 * s_cloudH, *p1 = s_cloud8 + c1 * s_cloudH;
    const int bx = x & 3;
    const int rows = (s_cloudRows < yS ? s_cloudRows : yS) >> 1;   // none behind solid mountain
    for (int r = 0; r < rows; r++) {
        const int v0 = p0[r], v1 = p1[r];
        if (!((v0 | v1) & 15)) continue;
        const int a0 = (v0 & 15) * 17, a1 = (v1 & 15) * 17;
        const int a = a0 + (((a1 - a0) * fu) >> 8);
        const int l0 = s_cloudLum4[v0 >> 4], l1 = s_cloudLum4[v1 >> 4];
        const uint32_t cS = s_cloudLut[l0 + (((l1 - l0) * fu) >> 8)];
        const int y = r * 2;
        int a5 = (a + BAYER[y & 3][bx] * 2) >> 3;
        if (a5 > 0) tmp[y] = blend(tmp[y], cS, (uint32_t)(a5 > 32 ? 32 : a5));
        a5 = (a + BAYER[(y + 1) & 3][bx] * 2) >> 3;
        if (a5 > 0) tmp[y + 1] = blend(tmp[y + 1], cS, (uint32_t)(a5 > 32 ? 32 : a5));
    }
}

// The procedural skies: a gradient by rows, four dithered variants a row
// (picked by x & 3).
static uint16_t s_skyRowD[SCENE_MAX_H * 4];

// One row's colour.  (Kept out of line: clang's vectorizer, on the host,
// got the loop below wrong when this was inside it.)
__attribute__((noinline)) static void sky_row(const float stops[][4], int n, int y) {
    float t = (float)y / (HZ - 1), c[3];
    int k = 0;
    while (k < n - 2 && t > stops[k + 1][0]) k++;
    float u = clampf((t - stops[k][0]) / (stops[k + 1][0] - stops[k][0]), 0, 1);
    for (int i = 0; i < 3; i++) c[i] = stops[k][i + 1] + (stops[k + 1][i + 1] - stops[k][i + 1]) * u;
    for (int x = 0; x < 4; x++)
        s_skyRowD[y * 4 + x] = packd((int)c[0], (int)c[1], (int)c[2], BAYER[y & 3][x]);
}

static void sky_rows(const float stops[][4], int n) {   // stops: { y as a share of HZ, r, g, b }
    for (int y = 0; y < HZ; y++) sky_row(stops, n, y);
}

// Fire: stars turning about the celestial pole, off the top right -- the
// view faces west from mid-northern latitudes, so they wheel down to the
// right and set behind the mountains; sped up.  Each is spread over the
// 2 x 2 pixels around it (sub-pixel), twinkling a little.
#define STAR_TURN 0.0175f        // radians a second (10 degrees a scene)
static int      s_nstar = 0;
static float    s_poleX, s_poleY;
// (The stars' arrays are the fire's part of the per-scene union.)

static const int16_t *s_stars = nullptr;      // the catalog in flash: x, y, brightness, phase

static void stars_init() {
    s_poleX = LW * 1.25f;
    s_poleY = -HZ * 1.3f;
    s_stars = s_large ? STARS_L : STARS;
    s_nstar = s_large ? NSTARS_L * 13 / 16 : NSTARS;    // a few fewer on the larger panel (frame budget)
    if (s_nstar > NSTAR) s_nstar = NSTAR;
}

static void stars_step() {
    const float a = -STAR_TURN * s_pt, ca = cosf(a), sa = sinf(a);
    for (int x = 0; x < LW; x++) s_starHead[x] = -1;
    int ne = 0;
    for (int i = 0; i < s_nstar; i++) {
        const int16_t *st = s_stars + i * 4;
        float dx = st[0] - s_poleX, dy = st[1] - s_poleY;
        float x = s_poleX + dx * ca - dy * sa, y = s_poleY + dx * sa + dy * ca;
        if (x < 0 || x >= LW - 1 || y < 0 || y >= HZ - 1) continue;
        s_starX16[i] = (int16_t)(x * 16);
        s_starY16[i] = (int16_t)(y * 16);
        // Twinkle: a triangle wave, 0.8 .. 1.
        int ph = ((int)(s_time * 64 * (1 + (st[3] & 3))) + st[3]) & 255;
        int tri = ph < 128 ? ph : 255 - ph;                 // 0..127
        s_starA[i] = (uint8_t)((st[2] * (205 + (tri * 50 >> 7))) >> 8);
        for (int k = 0; k < 2; k++) {
            int c = (int)x + k;
            s_starOf[ne] = (int16_t)i;
            s_starNext[ne] = s_starHead[c];
            s_starHead[c] = (int16_t)ne++;
        }
    }
}

HOT static void draw_stars(int x, uint16_t *tmp, int yS) {
    for (int e = s_starHead[x]; e >= 0; e = s_starNext[e]) {
        const int i = s_starOf[e];
        const int fx = s_starX16[i] & 15, ix = s_starX16[i] >> 4;
        const int wx = x == ix ? 16 - fx : fx;                // /16
        const int fy = s_starY16[i] & 15, iy = s_starY16[i] >> 4;
        const int a = s_starA[i] * wx;                        // /16
        for (int k = 0; k < 2; k++) {
            int y = iy + k;
            if (y >= yS) break;
            int v = (a * (k ? fy : 16 - fy)) >> 8;            // 0..255
            if (v > 2) tmp[y] = add(tmp[y], v, v, (v * 15) >> 4);
        }
    }
}

// Fire: the moon, a crescent with a halo, setting along the sun's bearing.
#define MOON_LUT 1024
#define s_moonHalo s_sunMoonLut
static uint8_t  s_moonCov[MOON_LUT];
static int      s_moonSh = 0, s_moonRB = 0;
static int32_t  s_moonX16, s_moonY16, s_shX16, s_shY16;
static uint32_t s_moonS;

static void moon_init() {
    const float SYK = HZ / 128.0f, R = 10 * SYK, halo = 14 * SYK;
    s_moonRB = (int)(R + 3 * halo);
    s_moonSh = 0;
    while ((s_moonRB * s_moonRB >> s_moonSh) >= MOON_LUT) s_moonSh++;
    for (int i = 0; i < MOON_LUT; i++) {
        float d = sqrtf((float)((i << s_moonSh) + (1 << s_moonSh) / 2));
        float h = expf(-fmaxf(d - R, 0) / halo) * 38 * clampf((s_moonRB - d) / (2 * SYK), 0, 1);
        s_moonHalo[i] = spread(pack((int)(h * 0.8f), (int)(h * 0.85f), (int)h));
        s_moonCov[i] = (uint8_t)(clampf(R + 0.5f - d, 0, 1) * 32);
    }
    s_moonS = spread(pack(235, 232, 210));
}

static void moon_step() {
    const float SYK = HZ / 128.0f, t = s_pt / SCENE_PHASE_SECONDS;
    const float dist = 46 * SYK * t;                      // along a bearing of 120 degrees
    const float x = 262.0f * LW / 320 + dist * 0.8660254f, y = 30 * SYK + dist * 0.5f;
    s_moonX16 = (int32_t)(x * 16);
    s_moonY16 = (int32_t)(y * 16);
    s_shX16 = (int32_t)((x + 5.5f * SYK) * 16);
    s_shY16 = (int32_t)((y - 2.5f * SYK) * 16);
}

HOT static void draw_moon(int x, uint16_t *tmp) {
    const int cx = s_moonX16 >> 4, cy = s_moonY16 >> 4;
    if (x < cx - s_moonRB || x > cx + s_moonRB) return;
    const int y0 = cy - s_moonRB < 0 ? 0 : cy - s_moonRB;
    const int y1 = cy + s_moonRB >= HZ ? HZ - 1 : cy + s_moonRB;
    const int32_t dx = x * 16 + 8 - s_moonX16, sdx = x * 16 + 8 - s_shX16;
    const int sh = 8 + s_moonSh;
    for (int y = y0; y <= y1; y++) {
        int32_t dy = y * 16 + 8 - s_moonY16;
        uint32_t i = (uint32_t)(dx * dx + dy * dy) >> sh;
        if (i >= MOON_LUT) continue;
        uint32_t c = sat(spread(tmp[y]) + s_moonHalo[i]);
        int cov = s_moonCov[i];
        if (cov) {
            int32_t sdy = y * 16 + 8 - s_shY16;
            uint32_t j = (uint32_t)(sdx * sdx + sdy * sdy) >> sh;
            int shade = j < MOON_LUT ? s_moonCov[j] : 0;
            int lit = (cov * (32 - shade)) >> 5;
            if (lit) c = lerpS(c, s_moonS, (uint32_t)lit);
        }
        tmp[y] = unspread(c);
    }
}

// The mountains, zoomed: bilinear in colour and coverage.  Rows above the
// layer's first coverage keep the sky; rows below its solid part don't need
// the sky at all.
HOT static void draw_mountains(int x, uint16_t *tmp, int yA, int yS) {
    const SceneAsset &A = *s_asset;
    const int c0 = g_mCol[x], fx = g_mFx[x];
    const uint16_t *m0 = A.mcol + c0 * A.mh, *m1 = m0 + A.mh;
    const uint8_t *a0 = A.malpha + c0 * A.mh, *a1 = a0 + A.mh;
    const int last = A.mh - 1;
    if (s_zoom == 1.0f && A.scale == 1.0f) {   // unscaled: the layer as it is
        const int base = A.mtop + (HZ - A.lhz);         // the layer's horizon on the picture's
        const uint16_t *mc = A.mcol + x * A.mh;              // (g_mCol clamps the last column)
        const uint8_t *ma = A.malpha + x * A.mh;
        for (int y = yA; y < HZ; y++) {
            const int r = y - base;
            if (r < 0) continue;
            if (y >= yS) { tmp[y] = mc[r]; continue; }
            int a = ma[r];
            if (a > 3) tmp[y] = blend(tmp[y], spread(mc[r]), (uint32_t)((a + 4) >> 3));
        }
        return;
    }
    // Each layer row is interpolated across once, and kept: zoomed in, the
    // next picture row usually needs the same pair of layer rows.
    int rp = -2;
    uint32_t top = 0, bot = 0;
    int at = 0, ab = 0;
    for (int y = yA; y < HZ; y++) {
        const int r = g_mRow[y], fy = g_mFy[y];
        if (r != rp) {
            const int r1 = r < last ? r + 1 : r;
            if (r == rp + 1) { top = bot; at = ab; }
            else {
                top = lerpS(spread(m0[r]), spread(m1[r]), fx);
                at = a0[r] * 32 + (a1[r] - a0[r]) * fx;        // x32
            }
            bot = lerpS(spread(m0[r1]), spread(m1[r1]), fx);
            ab = a0[r1] * 32 + (a1[r1] - a0[r1]) * fx;
            rp = r;
        }
        uint32_t c = lerpS(top, bot, fy);
        if (y >= yS) { tmp[y] = unspread(c); continue; }
        int a = (at * 32 + (ab - at) * fy) >> 10;              // 0..255
        if (a <= 3) continue;
        tmp[y] = blend(tmp[y], c, (uint32_t)((a + 4) >> 3));
    }
}

// Column x of the backdrop, composed into tmp (native byte order).
HOT static void draw_backdrop(int x, uint16_t *tmp) {
    const SceneAsset &A = *s_asset;
    const int c0 = g_mCol[x];
    const int first = A.mfirst[c0] < A.mfirst[c0 + 1] ? A.mfirst[c0] : A.mfirst[c0 + 1];
    const int solid = A.msolid[c0] > A.msolid[c0 + 1] ? A.msolid[c0] : A.msolid[c0 + 1];
    int yA = (int)zoom_row((float)first - 1);
    int yS = (int)zoom_row((float)solid + 1) + 1;
    yA = clampi(yA, 0, HZ);
    yS = clampi(yS, yA, HZ);
    // The sky, down to where the mountains are solid: from flash (the
    // storm's), or a gradient by rows, ordered-dithered.
    volatile uint32_t *prof = s_prof[s_worker];
    uint32_t t0 = CYC();
    if (s_sky) memcpy(tmp, s_sky + x * HZ, (size_t)yS * 2);
    else {
        const uint16_t *rd = s_skyRowD + (x & 3);
        for (int y = 0; y < yS; y++) tmp[y] = rd[y * 4];
    }
    uint32_t t1 = CYC();
    if (s_phase == 2) draw_clouds(x, tmp, yS);
    else if (s_phase == 1) draw_sun(x, tmp);
    else { draw_stars(x, tmp, yS); draw_moon(x, tmp); }
    uint32_t t2 = CYC();
    draw_mountains(x, tmp, yA, yS);
    prof[SCENE_PROF_SKYCOPY] += t1 - t0;
    prof[SCENE_PROF_CLOUDSUN] += t2 - t1;
    prof[SCENE_PROF_MOUNTAINS] += CYC() - t2;
}

/* ================================================================ phases */

static const char *const NAMES[] = { "fire", "storm", "snow" };

static void load_sky() {
    if (!s_asset->sky) s_sky = nullptr;
    else if (s_skyRam) { memcpy(s_skyRam, s_asset->sky, (size_t)LW * HZ * 2); s_sky = s_skyRam; }
    else s_sky = s_asset->sky;
}

static void enter_phase(int p) {
    s_phase = p;
    s_time = 0;
    if (s_large) {
        s_asset = p == 0 ? &ASSET_FIRE_L : p == 1 ? &ASSET_STORM_L : &ASSET_SNOW_L;
    } else {
        s_asset = p == 0 ? &ASSET_FIRE : p == 1 ? &ASSET_STORM : &ASSET_SNOW;
    }
    // The horizon: snow's is lower, for more of the mountains.
    const int hz = s_large ? ASSET_HZ_L : ASSET_HZ;
    const int newHZ = p == 2 ? (int)lroundf(LH * 0.70f) : hz;
    if (newHZ != HZ || !g_vq) { HZ = newHZ; GR = LH - HZ; ground_tables_init(); }
    load_sky();
    s_zoom = 1;
    zoom_step();
    if (p == 0) {
        moon_init();
        static const float sky[][4] = { { 0, 3, 5, 16 }, { 0.55f, 10, 14, 38 }, { 1, 34, 30, 56 } };
        sky_rows(sky, 3);
        moon_step();
        stars_step();
        const float base[3] = { 20, 15, 12 }, line[3] = { 64, 44, 34 }, haze[3] = { 14, 13, 24 };
        ground_colors(base, line, haze);
        fire_enter();
    } else if (p == 1) {
        sun_init();
        sun_step();
        const float base[3] = { 16, 12, 22 }, line[3] = { 92, 64, 84 }, haze[3] = { 70, 40, 52 };
        ground_colors(base, line, haze);
        storm_enter();
    } else {
        const float base[3] = { 214, 220, 232 }, line[3] = { 150, 164, 192 }, haze[3] = { 196, 202, 216 };
        ground_colors(base, line, haze);
        static const float sky[][4] = { { 0, 120, 132, 152 }, { 0.55f, 168, 178, 196 }, { 1, 206, 212, 224 } };
        sky_rows(sky, 3);
        clouds_enter();
        snow_enter();
    }
}

bool scene_supports(int panelW, int panelH) {
    return (panelW == 240 && panelH >= 320) || (panelW == 320 && panelH >= 480);
}

unsigned scene_sky_bytes(int panelW) {
    return panelW == 320 ? ASSET_W_L * ASSET_HZ_L * 2 : ASSET_W * ASSET_HZ * 2;
}

void scene_init(int panelW, bool flip) {
    s_large = panelW == 320;
    LW = s_large ? ASSET_W_L : ASSET_W;
    LH = panelW;
    HZ = s_large ? ASSET_HZ_L : ASSET_HZ;
    GR = LH - HZ;
    CX = LW / 2;
    K = LH / 240.0f;
    KZ = 400.0f * K;
    s_flip = flip;
    s_dir = flip ? 1 : -1;
    for (int i = 0; i < RING; i++) {
        s_ringC[i] = cosf(i * 6.2831853f / RING);
        s_ringS[i] = sinf(i * 6.2831853f / RING);
    }
    ground_tables_init();
    fire_init_tables();
    clouds_init();
    stars_init();
    fire_layout();
    enter_phase(0);
    ground_step();
}

void scene_use_sky_ram(uint16_t *buf) {
    s_skyRam = buf;
    load_sky();
}

int scene_width(void) { return LW; }
int scene_height(void) { return LH; }
int scene_chunks(void) { return LW / SCENE_CHUNK; }
void scene_next_phase(void) { s_skip = true; }
int scene_phase(void) { return s_phase; }
const char *scene_phase_name(void) { return NAMES[s_phase]; }
#ifdef SCENE_HOST
float scene_host_flash(void) { return s_flash; }
#endif

float scene_brightness(void) {
    float b = fminf(s_pt / SCENE_FADE_SECONDS, (SCENE_PHASE_SECONDS - s_pt) / SCENE_FADE_SECONDS);
    return clampf(b, 0, 1);
}

static float s_dt = 0;

// The frame's step is in three parts so two workers can share it:
// scene_step_begin() (time, scene changes), then scene_step_main() and
// scene_step_helper() side by side.
void scene_step_begin(float dt) {
    uint32_t t0 = CYC();
    if (dt > 0.2f) dt = 0.2f;
    s_dt = dt;
    if (s_skip) {
        s_skip = false;
        const float out = SCENE_PHASE_SECONDS - SCENE_FADE_SECONDS;
        if (s_pt < out) s_pt = s_pt < SCENE_FADE_SECONDS ? SCENE_PHASE_SECONDS - s_pt : out;
    }
    s_time += dt;
    s_pt += dt;
    if (s_pt >= SCENE_PHASE_SECONDS) {
        s_pt -= SCENE_PHASE_SECONDS;
        enter_phase((s_phase + 1) % 3);
    }
    s_groundZ += GROUND_SPEED * dt;
    s_groundZ -= floorf(s_groundZ);
    s_zoom = 1 + ZOOM_GROWTH[s_phase] * (s_pt / SCENE_PHASE_SECONDS);
    s_prof[0][SCENE_PROF_STEP] += CYC() - t0;
}

void scene_step_main(void) {
    uint32_t t0 = CYC();
    const float dt = s_dt;
    zoom_step();
    switch (s_phase) {
    case 0: fire_step(dt); break;
    case 1: storm_step(dt); sun_step(); tint_tables(); break;
    default: snow_step(dt); s_cloudOff += CLOUD_SPEED * K * dt; if (s_cloudOff >= 2 * s_cloudW) s_cloudOff -= 2 * s_cloudW; break;
    }
    ground_step();
    if (s_phase == 0) glow_tables();
    s_prof[0][SCENE_PROF_STEP] += CYC() - t0;
}

void scene_step_helper(void) {
    uint32_t t0 = CYC();
    if (s_phase == 0) {
        puffs_step(s_dt);
        smoke_build();
        moon_step();
        stars_step();
    }
    s_prof[1][SCENE_PROF_STEP] += CYC() - t0;
}

void scene_step(float dt) {
    scene_step_begin(dt);
    scene_step_main();
    scene_step_helper();
}

int scene_min_periods(void) { return 2; }

/* ================================================================ drawing */

// The backdrop and the ground for this chunk's columns.
HOT static void draw_base(uint32_t &skyCyc, uint32_t &allCyc) {
    const int phase = s_phase;
    const int d = s_dir;
    uint32_t t0 = CYC();
    skyCyc = 0;
    for (int xi = 0; xi < SCENE_CHUNK; xi++) {
        const int x = s_x0 + xi;
        uint16_t *col = colp(x);
        uint16_t *tmp = s_colTmp[s_worker];
        uint32_t c0 = CYC();
        draw_backdrop(x, tmp);
        // Out to the panel row, with -- in the storm -- the sunset's light and
        // any lightning folded into the same pass.  tmp keeps the result for
        // the wet ground's reflection.
        if (phase == 1 && s_tintOn) {        // the sunset's light and the lightning: two look-ups a pixel
            uint16_t *o = col;
            for (int y = 0; y < HZ; y++, o += d) {
                const uint16_t c0 = tmp[y];
                const uint16_t c = (uint16_t)(s_tintHi[c0 >> 8] + s_tintLo[c0 & 255]);
                tmp[y] = c;
                *o = SW(c);
            }
        } else if (d < 0) {
            uint16_t *o = col;
            for (int y = 0; y < HZ; y++) *o-- = SW(tmp[y]);
        } else {
            for (int y = 0; y < HZ; y++) col[y] = SW(tmp[y]);
        }
        skyCyc += CYC() - c0;
        // The ground.
        const int dx = x - CX;
        const uint8_t *vq = g_vq + (dx >= 0 ? dx : -dx - 1) * GR;
        const uint16_t *lut = g_lut;
        uint16_t *g = col + HZ * d;
        if (phase == 0 && (s_glowLv[x][0] | s_glowLv[x][1] | s_glowLv[x][2] | s_glowLv[x][3])) {
            // The firelight: by the row's place in the dither pattern, the
            // table of this column's level or the next; 0 is the plain ground.
            const uint16_t *tp[4];
            for (int p = 0; p < 4; p++) tp[p] = s_glowLv[x][p] ? s_glowLut[s_glowLv[x][p] - 1] : nullptr;
            int j = 0;
            for (; j < s_glowJ0; j++, lut += NQ, g += d) *g = SW(lut[vq[j]]);
            for (int r = 0; j < s_glowJ1; j++, r++, lut += NQ, g += d) {
                const uint16_t *t = tp[(HZ + j) & 3];
                *g = SW(t ? t[r * NQ4 + (vq[j] >> 2)] : lut[vq[j]]);
            }
            for (; j < GR; j++, lut += NQ, g += d) *g = SW(lut[vq[j]]);
        } else if (phase == 1) {
            // (The nearest quarter of the ground, where the reflection is
            // weakest, goes without it: plain wet ground, for the frame rate.)
            const int jr = GR - GR / 4;
            for (int j = 0; j < jr; j++, lut += NQ, g += d) {
                // Wet ground: the sky mirrored, rippled, stronger at a
                // grazing angle -- from this column as just composed, with
                // the mountains and the sun in it.  (The lightning is in
                // the ground table.)
                uint16_t m = tmp[g_reflY[j]];
                uint32_t s = spread(lut[vq[j]]) + scaleS(spread(m), g_reflK[j]);
                *g = SW(unspread(sat(s)));
            }
            for (int j = jr; j < GR; j++, lut += NQ, g += d) *g = SW(lut[vq[j]]);
        } else {
            for (int j = 0; j < GR; j++, lut += NQ, g += d) *g = SW(lut[vq[j]]);
        }
    }
    allCyc = CYC() - t0;
}

HOT void scene_render_chunk(int c, uint16_t *dst, int worker) {
    s_dst = dst;
    s_worker = worker;
    s_x0 = s_flip ? LW - SCENE_CHUNK - c * SCENE_CHUNK : c * SCENE_CHUNK;
    volatile uint32_t *prof = s_prof[worker];
    uint32_t skyCyc, baseCyc;
    draw_base(skyCyc, baseCyc);
    uint32_t t1 = CYC();
    switch (s_phase) {
    case 0: {
        uint32_t a = CYC(); draw_logs();
        uint32_t b = CYC(); draw_puffs();
        uint32_t c2 = CYC(); draw_flames();
        uint32_t e = CYC(); draw_embers();
        prof[SCENE_PROF_LOGS] += b - a; prof[SCENE_PROF_PUFFS] += c2 - b; prof[SCENE_PROF_FLAMES] += e - c2;
        break;
    }
    case 1: draw_bolt(); draw_splashes(); draw_rain(); break;
    default: draw_snow(); break;
    }
    prof[SCENE_PROF_SKY] += skyCyc;
    prof[SCENE_PROF_GROUND] += baseCyc - skyCyc;
    prof[SCENE_PROF_OVER] += CYC() - t1;
}

void scene_take_profile(uint32_t out[SCENE_PROF_N]) {
    for (int k = 0; k < SCENE_PROF_N; k++) {
        out[k] = 0;
        for (int w = 0; w < SCENE_WORKERS; w++) { out[k] += s_prof[w][k]; s_prof[w][k] = 0; }
    }
}

// The no-speed-test notice: white text centred on black, in the picture's
// landscape orientation, as one panel row (native little-endian RGB565, for
// flushRegion()).  Usable before scene_init().
void scene_notice_row(int panelW, bool flip, int r, uint16_t *row) {
    const int lw = panelW == 320 ? ASSET_W_L : ASSET_W, lh = panelW;
    const int x = flip ? lw - 1 - r : r;                 // the picture column this panel row shows
    const int ox = (lw - NOTICE_W) / 2, oy = (lh - NOTICE_H) / 2;
    const int stride = (NOTICE_W + 7) / 8;
    for (int c = 0; c < lh; c++) {
        const int y = flip ? c : lh - 1 - c;
        const int u = x - ox, v = y - oy;
        bool on = (unsigned)u < NOTICE_W && (unsigned)v < NOTICE_H &&
                  (NOTICE[v * stride + (u >> 3)] & (0x80 >> (u & 7)));
        row[c] = on ? 0xFFFF : 0x0000;
    }
}
