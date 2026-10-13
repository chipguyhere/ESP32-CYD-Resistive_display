/*
 * ui.cpp - A resolution-independent "bouncing balls" demo in plain LVGL 9.
 *
 * Nothing here is pinned to a resolution: ui_init() reads the live display size
 * from LVGL, so the same file works on any board whose sketch brings up LVGL
 * through an lv_setup.hpp of the same shape (portrait or landscape, any size).
 *
 * The balls are just circular LVGL objects under gravity.  A bottom control strip
 * holds two sliders: one selects 1..20 balls, the other scales the speed.  Each
 * ball is a random color and re-launches to a random height every floor bounce.
 */

#include "ui.h"
#include <math.h>
#include <esp_random.h>

/* ----------------------------------------------------------------- tunables */

#define MAX_BALLS 20

/* ----------------------------------------------------------------- state    */

typedef struct {
    lv_obj_t *obj;
    float x, y;     /* top-left position, pixels                              */
    float vx, vy;   /* velocity, pixels per base tick                         */
    int   d;        /* diameter, pixels                                       */
} ball_t;

static ball_t   g_balls[MAX_BALLS];
static int      g_ball_count = 5;     /* how many are active/visible          */
static float    g_speed      = 1.0f;  /* time multiplier from the speed slider */

/* Play field (above the control strip) and physics, all derived from the
 * live resolution in ui_init(). */
static int   g_field_w = 0;   /* play area width  (full display width)        */
static int   g_floor_y = 0;   /* play area height (display height minus strip) */
static float g_gravity = 0.0f;
static int   g_min_d = 0, g_max_d = 0;

static lv_obj_t *g_count_label, *g_speed_label;

/* ----------------------------------------------------------------- helpers  */

/* Uniform random float in [a, b) using the ESP32 hardware RNG (no seeding). */
static inline float frand(float a, float b) {
    return a + (esp_random() / 4294967296.0f) * (b - a);
}
/* Uniform random int in [a, b] inclusive. */
static inline int irand(int a, int b) {
    return a + (int)(esp_random() % (uint32_t)(b - a + 1));
}

/* Upward speed needed to reach a random height between 30% and 100% of the
 * field, so every floor bounce tops out somewhere different. */
static float launch_speed(void) {
    float h = frand(0.30f, 1.0f) * (float)g_floor_y;
    return sqrtf(2.0f * g_gravity * h);
}

/* (Re)initialize one ball with a random size, position, color and motion. */
static void init_ball(int i) {
    ball_t *b = &g_balls[i];
    b->d  = irand(g_min_d, g_max_d);
    b->x  = frand(0, (float)(g_field_w - b->d));
    b->y  = frand(0, (float)(g_floor_y - b->d));
    b->vx = frand(g_field_w * 0.003f, g_field_w * 0.010f) * ((esp_random() & 1) ? 1.0f : -1.0f);
    b->vy = -launch_speed();

    lv_color_t color = lv_color_make(64 + esp_random() % 192,
                                     64 + esp_random() % 192,
                                     64 + esp_random() % 192);
    lv_obj_set_size(b->obj, b->d, b->d);
    lv_obj_set_style_bg_color(b->obj, color, 0);
    lv_obj_set_pos(b->obj, (int)b->x, (int)b->y);
}

/* ----------------------------------------------------------------- physics  */

static void tick_cb(lv_timer_t *t) {
    LV_UNUSED(t);
    const float s = g_speed;
    for (int i = 0; i < g_ball_count; i++) {
        ball_t *b = &g_balls[i];

        b->vy += g_gravity * s;     /* gravity */
        b->x  += b->vx * s;
        b->y  += b->vy * s;

        /* Side walls */
        if (b->x < 0)                    { b->x = 0;                       b->vx = -b->vx; }
        if (b->x + b->d > g_field_w)     { b->x = g_field_w - b->d;        b->vx = -b->vx; }
        /* Ceiling */
        if (b->y < 0)                    { b->y = 0; if (b->vy < 0)        b->vy = -b->vy; }
        /* Floor -> re-launch to a fresh random height */
        if (b->y + b->d > g_floor_y)     { b->y = g_floor_y - b->d;        b->vy = -launch_speed(); }

        lv_obj_set_pos(b->obj, (int)b->x, (int)b->y);
    }
}

/* ----------------------------------------------------------------- controls */

static void count_event_cb(lv_event_t *e) {
    int v = lv_slider_get_value((lv_obj_t *)lv_event_get_target(e));
    g_ball_count = v;
    lv_label_set_text_fmt(g_count_label, "Balls: %d", v);
    for (int i = 0; i < MAX_BALLS; i++) {
        if (i < v) lv_obj_remove_flag(g_balls[i].obj, LV_OBJ_FLAG_HIDDEN);
        else       lv_obj_add_flag(g_balls[i].obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void speed_event_cb(lv_event_t *e) {
    int v = lv_slider_get_value((lv_obj_t *)lv_event_get_target(e));  /* 2..30 */
    g_speed = v / 10.0f;
    /* Avoid %f (LVGL float printf is off by default): format tenths by hand. */
    lv_label_set_text_fmt(g_speed_label, "Speed: %d.%dx", v / 10, v % 10);
}

/* A circular, non-interactive ball object on the given parent. */
static lv_obj_t *make_ball(lv_obj_t *parent) {
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);   /* don't steal touches from sliders */
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    return b;
}

/* ----------------------------------------------------------------- build    */

void ui_init(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101018), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* --- auto-detect the live resolution --- */
    lv_display_t *disp = lv_display_get_default();
    int W = lv_display_get_horizontal_resolution(disp);
    int H = lv_display_get_vertical_resolution(disp);

    /* Reserve a bottom strip for the two sliders; the balls live above it. */
    int strip_h = H / 4;
    if (strip_h < 112) strip_h = 112;
    if (strip_h > 220) strip_h = 220;

    g_field_w = W;
    g_floor_y = H - strip_h;
    g_gravity = g_floor_y * 0.0010f;                 /* scales with field height */
    int base  = (g_field_w < g_floor_y) ? g_field_w : g_floor_y;
    g_min_d   = base / 16; if (g_min_d < 12) g_min_d = 12;
    g_max_d   = base / 9;  if (g_max_d < g_min_d + 2) g_max_d = g_min_d + 2;

    /* Create all balls up front; show only the active ones. */
    for (int i = 0; i < MAX_BALLS; i++) {
        g_balls[i].obj = make_ball(scr);
        init_ball(i);
        if (i >= g_ball_count) lv_obj_add_flag(g_balls[i].obj, LV_OBJ_FLAG_HIDDEN);
    }

    /* --- control strip: two labeled sliders, full width --- */
    int m       = 12;                                /* margin                 */
    int slider_h = 16;
    int row_gap  = 14;
    int slider_w = W - 2 * m;
    int y        = g_floor_y + m;

    g_count_label = lv_label_create(scr);
    lv_obj_set_style_text_color(g_count_label, lv_color_white(), 0);
    lv_obj_set_pos(g_count_label, m, y);
    lv_label_set_text_fmt(g_count_label, "Balls: %d", g_ball_count);
    y += 20;

    lv_obj_t *count_slider = lv_slider_create(scr);
    lv_obj_set_size(count_slider, slider_w, slider_h);
    lv_obj_set_pos(count_slider, m, y);
    lv_slider_set_range(count_slider, 1, MAX_BALLS);
    lv_slider_set_value(count_slider, g_ball_count, LV_ANIM_OFF);
    lv_obj_add_event_cb(count_slider, count_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    y += slider_h + row_gap;

    g_speed_label = lv_label_create(scr);
    lv_obj_set_style_text_color(g_speed_label, lv_color_white(), 0);
    lv_obj_set_pos(g_speed_label, m, y);
    lv_label_set_text_fmt(g_speed_label, "Speed: %d.%dx", 10 / 10, 10 % 10);
    y += 20;

    lv_obj_t *speed_slider = lv_slider_create(scr);
    lv_obj_set_size(speed_slider, slider_w, slider_h);
    lv_obj_set_pos(speed_slider, m, y);
    lv_slider_set_range(speed_slider, 2, 30);        /* 0.2x .. 3.0x           */
    lv_slider_set_value(speed_slider, 10, LV_ANIM_OFF);
    lv_obj_add_event_cb(speed_slider, speed_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Run the simulation ~60x/second. */
    lv_timer_create(tick_cb, 16, NULL);
}
