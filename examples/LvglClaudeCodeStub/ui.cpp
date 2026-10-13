/*
 * ui.cpp - Application UI for the LvglClaudeCodeStub example.
 *
 * A clean starting point for building an LVGL application on a
 * resistive-touch CYD (240x320 or 320x480 portrait).  Replace the body
 * of ui_init() with your own widgets.
 *
 * This stub doubles as a bring-up test:
 *   - "Hello, world!" follows your finger, centered on the touch point, so you
 *     can confirm touch coordinates line up with the display.
 *   - A rectangle cycles RED -> GREEN -> BLUE once per second so you can verify
 *     the panel's color order (BGR vs RGB) and inversion are correct.
 *
 * The display, touch input, and LVGL itself are already initialized by the
 * sketch (see lv_setup.hpp) before ui_init() is called.
 */

#include "ui.h"

static lv_obj_t *hello_label;
static lv_obj_t *color_rect;
static lv_obj_t *color_name;

// Move the "Hello, world!" label so it's centered on wherever the screen is
// being touched.  Fires continuously while pressed, so it tracks a drag too.
static void follow_touch_cb(lv_event_t *e) {
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;

    lv_point_t p;
    lv_indev_get_point(indev, &p);

    lv_coord_t w = lv_obj_get_width(hello_label);
    lv_coord_t h = lv_obj_get_height(hello_label);
    lv_obj_set_pos(hello_label, p.x - w / 2, p.y - h / 2);
}

// Cycle the test rectangle through pure red, green, and blue.
static void color_cycle_cb(lv_timer_t *t) {
    static const uint32_t colors[3] = { 0xFF0000, 0x00FF00, 0x0000FF };
    static const char    *names[3]  = { "RED", "GREEN", "BLUE" };
    static int i = 0;

    lv_obj_set_style_bg_color(color_rect, lv_color_hex(colors[i]), 0);
    lv_label_set_text(color_name, names[i]);
    i = (i + 1) % 3;
}

void ui_init(void) {
    // The screen LVGL created for us.  Everything we draw is a child of this.
    lv_obj_t *screen = lv_screen_active();

    // Color-test rectangle near the top.
    color_rect = lv_obj_create(screen);
    lv_obj_set_size(color_rect, 200, 90);
    lv_obj_align(color_rect, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_style_bg_opa(color_rect, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(color_rect, 0, 0);
    lv_obj_remove_flag(color_rect, LV_OBJ_FLAG_SCROLLABLE);
    // Don't let the rectangle eat touches meant for the "follow" behavior.
    lv_obj_remove_flag(color_rect, LV_OBJ_FLAG_CLICKABLE);

    // Label naming the current color (white text, drawn over the rectangle).
    color_name = lv_label_create(color_rect);
    lv_obj_set_style_text_color(color_name, lv_color_white(), 0);
    lv_obj_center(color_name);
    lv_label_set_text(color_name, "RED");

    // The roaming "Hello, world!" label.  Uses absolute positioning (default
    // top-left alignment) so follow_touch_cb can place it by screen coords.
    hello_label = lv_label_create(screen);
    lv_label_set_text(hello_label, "Hello, world!");
    lv_obj_update_layout(hello_label);     // so get_width/height are valid

    lv_display_t *disp = lv_display_get_default();
    lv_coord_t scr_w = lv_display_get_horizontal_resolution(disp);
    lv_coord_t scr_h = lv_display_get_vertical_resolution(disp);
    lv_obj_set_pos(hello_label,
                   (scr_w - lv_obj_get_width(hello_label))  / 2,
                   (scr_h - lv_obj_get_height(hello_label)) / 2);

    // Track touches on the whole screen.  The screen must be clickable to
    // receive press events from the touch indev; disable scrolling so dragging
    // a finger doesn't rubber-band/scroll the screen contents.
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(screen, follow_touch_cb, LV_EVENT_PRESSING, NULL);

    // Run the color cycle once per second.
    lv_timer_create(color_cycle_cb, 1000, NULL);

    // From here, add your own screens, widgets, and event handlers.
    // See https://docs.lvgl.io/ for the LVGL widget reference.
}
