/*******************************************************************************
 * LvglRotation - change the screen rotation while the sketch runs
 *
 * For the resistive-touch "Cheap Yellow Display" boards: ESP32-2432S028,
 * ESP32-32E 2.8", 3.2", 3.5", 4.0", ESP32-024 2.4" and ESP32-035 3.5"
 * (XPT2046 touch; board and panel detected at start-up).
 *
 * Tap "Rotate" or press BOOT to step through the four rotations.  Each step
 * turns the picture a quarter-turn clockwise, so turn the board a quarter-turn
 * counter-clockwise to keep reading it.  The arrow at the top always points
 * to the top of the picture, a marker sits in each corner, and a dot follows
 * your finger, so you can see that touch turned with the screen.
 *
 * The one call that does it is lv_setup.setRotation(r): display, touch and
 * LVGL change together, and LVGL redraws the screen at the new size.  This
 * UI is built from a flex column and lv_obj_align(), which reflow by
 * themselves.  Objects placed at fixed x/y coordinates would not move.
 *
 * Call setRotation() from loop(), as here, rather than from inside an LVGL
 * event callback: the button's callback only sets a flag.
 *
 * Requires the following libraries:
 *   - chipguy_ESP32-CYD-Resistive_display  (display + touch driver, this library)
 *   - lvgl 9
 *
 * Arduino IDE Board Settings:
 *   - Board: "ESP32 Dev Module" (in Tools > Board > esp32).
 *   - The defaults work; the sketch fits the default partition scheme.
 *
 ******************************************************************************/

// The bundled lv_conf.h is based on LVGL 9.3 (LVGL 9 compatible).
#include "lv_conf.h"
#include "lvgl.h"
#include "lv_setup.hpp"

#define BOOT_PIN 0                         // BOOT button: next rotation

static lv_obj_t *lblRotation, *lblSize, *dot;
static volatile bool rotateRequested = false;

static void showRotation() {
    static const char *names[4] = {"portrait", "landscape",
                                   "portrait, upside down", "landscape, upside down"};
    uint8_t r = lv_setup.rotation();
    lv_label_set_text_fmt(lblRotation, "Rotation %u (%u\xC2\xB0)", r, r * 90);
    lv_label_set_text_fmt(lblSize, "%s, %dx%d", names[r], display.width(), display.height());
    Serial.printf("rotation %u: %dx%d\n", r, display.width(), display.height());
}

static void rotateClicked(lv_event_t *) { rotateRequested = true; }

// The dot follows the finger anywhere on the background.
static void screenPressing(lv_event_t *) {
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_obj_set_pos(dot, p.x - 6, p.y - 6);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *cornerMarker(lv_obj_t *scr, const char *text, lv_align_t align) {
    lv_obj_t *l = lv_label_create(scr);
    lv_obj_set_style_text_color(l, lv_color_hex(0x40C0FF), 0);
    lv_label_set_text(l, text);
    lv_obj_align(l, align, 0, 0);
    return l;
}

static void buildScreen() {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(scr, 4, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, screenPressing, LV_EVENT_PRESSING, nullptr);

    // Arrow to the top of the picture.
    lv_obj_t *up = lv_label_create(scr);
    lv_obj_set_style_text_font(up, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(up, lv_color_hex(0xFFE000), 0);
    lv_label_set_text(up, LV_SYMBOL_UP);
    lv_obj_align(up, LV_ALIGN_TOP_MID, 0, 0);

    cornerMarker(scr, "TL", LV_ALIGN_TOP_LEFT);
    cornerMarker(scr, "TR", LV_ALIGN_TOP_RIGHT);
    cornerMarker(scr, "BL", LV_ALIGN_BOTTOM_LEFT);
    cornerMarker(scr, "BR", LV_ALIGN_BOTTOM_RIGHT);

    // The middle: a flex column, centred whatever the screen's shape.
    lv_obj_t *col = lv_obj_create(scr);
    lv_obj_set_size(col, LV_PCT(90), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 8, 0);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);    // touches fall through to the screen
    lv_obj_center(col);

    lblRotation = lv_label_create(col);
    lv_obj_set_style_text_font(lblRotation, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lblRotation, lv_color_hex(0xFFFFFF), 0);

    lblSize = lv_label_create(col);
    lv_obj_set_style_text_color(lblSize, lv_color_hex(0xC0C0C0), 0);

    lv_obj_t *model = lv_label_create(col);
    lv_obj_set_style_text_color(model, lv_color_hex(0x909090), 0);
    lv_label_set_text(model, display.modelName());

    lv_obj_t *btn = lv_button_create(col);
    lv_obj_add_event_cb(btn, rotateClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, LV_SYMBOL_REFRESH "  Rotate");

    lv_obj_t *hint = lv_label_create(col);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x909090), 0);
    lv_label_set_text(hint, "or press BOOT.  Draw anywhere.");

    dot = lv_obj_create(scr);
    lv_obj_set_size(dot, 12, 12);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0xFFE000), 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(dot, LV_OBJ_FLAG_IGNORE_LAYOUT);

    showRotation();
}

void setup() {
    Serial.begin(115200);
    lv_setup.begin(0);                     // start in portrait
    pinMode(BOOT_PIN, INPUT_PULLUP);
    buildScreen();
}

void loop() {
    if (digitalRead(BOOT_PIN) == LOW) {
        rotateRequested = true;
        while (digitalRead(BOOT_PIN) == LOW) delay(10);
    }
    if (rotateRequested) {
        rotateRequested = false;
        lv_setup.setRotation((lv_setup.rotation() + 1) % 4);
        lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
        showRotation();
    }
    lv_timer_handler();
    delay(5);
}
