/*
 * ui.cpp - LVGL UI for the CYD MP3 player.
 *
 * Laid out for whatever screen it finds: the design target is 240x320
 * portrait, and every size, padding and font is scaled from that by one
 * factor (1.0 on the 240x320 boards, 1.33 on the 320x480 ones).  The
 * main screen is flex rows, so it also works in landscape; the click-wheel
 * screen puts the LCD beside the wheel when the screen is wider than tall.
 *
 *   +----------------------------+
 *   | Title (scrolls if long)    |   now-playing panel
 *   | Artist (scrolls if long)   |
 *   |             >              |   state glyph, centered
 *   | [=====progress======     ] |
 *   +----------------------------+
 *   | > track1.mp3               |   track list (fills the middle,
 *   |   track2.mp3               |   scrolls; tap to play)
 *   |   ...                      |
 *   +----------------------------+
 *   | |<   >||   >|   [ ]        |   transport
 *   | (o) ------------ volume    |
 *   +----------------------------+
 *
 * Tapping the now-playing panel switches to a second screen styled after a
 * classic click-wheel music player: a small "LCD" with the same title / info /
 * progress, and a click wheel with no legends.  Center = play/pause,
 * left/right = prev/next, bottom = stop, top = back (Menu), dragging around the
 * ring = volume.  Tapping the little LCD also returns to this screen.
 *
 * A small "8" in the click-wheel screen's lower-right corner, long-pressed,
 * opens a third screen with the dither A/B checkbox for the 8-bit quantizer and
 * a one-line explanation of what it trades.  Back returns to the click-wheel
 * screen.
 *
 * Everything here runs on the LVGL (Arduino loop) task.  The player lives on
 * another core, so the UI only sends commands and polls player_status() from
 * an LVGL timer — it never blocks on the decoder.
 */

#include "ui.h"
#include "player.h"
#include <math.h>

static lv_obj_t *mainScreen;
static lv_obj_t *titleLabel;
static lv_obj_t *infoLabel;       // artist line
static lv_obj_t *stateLabel;      // play / pause / stop glyph
static lv_obj_t *progressBar;
static lv_obj_t *trackList;
static lv_obj_t *playBtnLabel;
static lv_obj_t *volumeSlider;

// Click-wheel screen: its own copies of the now-playing widgets.
static lv_obj_t *clickwheelScreen;
static lv_obj_t *clickwheelTitle;
static lv_obj_t *clickwheelInfo;
static lv_obj_t *clickwheelState;
static lv_obj_t *clickwheelProgress;
static uint32_t volumeFlashUntil = 0;     // show "Volume NN%" on the info line until then

// Dither screen, reached by long-pressing the "8" on the click-wheel screen.
static lv_obj_t *ditherScreen;

static std::vector<lv_obj_t *> trackButtons;

// ---------------------------------------------------------------------------
// Scaling.  gScale = 1.0 at the 240x320 design size; px() scales a design
// pixel count, font() picks the built-in Montserrat nearest a scaled size.

static float gScale = 1.0f;

static void compute_scale() {
    lv_display_t *disp = lv_display_get_default();
    int w = lv_display_get_horizontal_resolution(disp);
    int h = lv_display_get_vertical_resolution(disp);
    int shortSide = w < h ? w : h, longSide = w < h ? h : w;
    float a = shortSide / 240.0f, b = longSide / 320.0f;
    gScale = a < b ? a : b;
}

static int32_t px(int designPx) { return (int32_t)lroundf(designPx * gScale); }

// Only a handful of sizes are referenced on purpose: every Montserrat size this
// function names is linked into the image (~40 KB each), referenced or not
// at run time.  14/16 cover the 240x320 boards, 18/20/22 the 320x480 ones.
static const lv_font_t *font(int designPx) {
    static const struct { int sz; const lv_font_t *f; } fonts[] = {
        {14, &lv_font_montserrat_14}, {16, &lv_font_montserrat_16}, {18, &lv_font_montserrat_18},
        {20, &lv_font_montserrat_20}, {22, &lv_font_montserrat_22},
    };
    float want = designPx * gScale;
    const lv_font_t *best = fonts[0].f;
    float bestErr = 1e9f;
    for (const auto &e : fonts) {
        float err = fabsf(e.sz - want);
        if (err < bestErr) { bestErr = err; best = e.f; }
    }
    return best;
}
static int highlightedTrack = -1;
static PlayerState lastState = PLAYER_STOPPED;
static char lastTitle[64] = "";

// ---------------------------------------------------------------------------
// Event handlers

static void track_clicked_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    player_play(idx);
}

static void prev_cb(lv_event_t *)  { player_prev(); }
static void play_cb(lv_event_t *)  { player_toggle_pause(); }
static void next_cb(lv_event_t *)  { player_next(); }
static void stop_cb(lv_event_t *)  { player_stop(); }

static void volume_cb(lv_event_t *e) {
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
    player_set_volume((uint8_t)lv_slider_get_value(slider));
}

static void dither_cb(lv_event_t *e) {
    lv_obj_t *cb = (lv_obj_t *)lv_event_get_target(e);
    player_set_dither(lv_obj_has_state(cb, LV_STATE_CHECKED));
}


// ---------------------------------------------------------------------------
// Status refresh

static void set_highlight(int track) {
    if (track == highlightedTrack) return;
    if (highlightedTrack >= 0 && highlightedTrack < (int)trackButtons.size())
        lv_obj_remove_state(trackButtons[highlightedTrack], LV_STATE_CHECKED);
    if (track >= 0 && track < (int)trackButtons.size()) {
        lv_obj_add_state(trackButtons[track], LV_STATE_CHECKED);
        lv_obj_scroll_to_view(trackButtons[track], LV_ANIM_ON);
    }
    highlightedTrack = track;
}

static void refresh_cb(lv_timer_t *) {
    PlayerStatus s = player_status();
    const auto &tracks = player_tracks();

    // Play/pause glyph
    if (s.state != lastState) {
        lv_label_set_text(playBtnLabel, s.state == PLAYER_PLAYING ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        lastState = s.state;
    }

    set_highlight(s.track);

    // Title line: ID3 title if we have one, else the filename.
    const char *title = s.title[0] ? s.title
                      : (s.track >= 0 && s.track < (int)tracks.size()) ? tracks[s.track].file.c_str()
                      : "No track";
    if (strcmp(title, lastTitle) != 0) {
        lv_label_set_text(titleLabel, title);
        lv_label_set_text(clickwheelTitle, title);
        strlcpy(lastTitle, title, sizeof(lastTitle));
    }

    // Artist line (or the filename when the title came from ID3), and the
    // state glyph on its own row.
    const char *stateStr = s.state == PLAYER_PLAYING ? LV_SYMBOL_PLAY
                         : s.state == PLAYER_PAUSED  ? LV_SYMBOL_PAUSE : LV_SYMBOL_STOP;
    const char *second;
    if (s.track >= 0 && s.track < (int)tracks.size())
        second = s.artist[0] ? s.artist
               : s.title[0]  ? tracks[s.track].file.c_str() : "";
    else
        second = "";
    if (strcmp(second, lv_label_get_text(infoLabel)) != 0)   // don't restart the scroll
        lv_label_set_text(infoLabel, second);
    if (strcmp(stateStr, lv_label_get_text(stateLabel)) != 0) {
        lv_label_set_text(stateLabel, stateStr);
        lv_label_set_text(clickwheelState, stateStr);
    }
    if ((int32_t)(volumeFlashUntil - lv_tick_get()) > 0)
        lv_label_set_text_fmt(clickwheelInfo, "Volume %u%%", player_volume());
    else if (strcmp(second, lv_label_get_text(clickwheelInfo)) != 0)
        lv_label_set_text(clickwheelInfo, second);

    int32_t pct = s.size ? (int32_t)((uint64_t)s.pos * 100 / s.size) : 0;
    lv_bar_set_value(progressBar, pct, LV_ANIM_OFF);
    lv_bar_set_value(clickwheelProgress, pct, LV_ANIM_OFF);
}

// ---------------------------------------------------------------------------
// Click-wheel screen

static void enter_clickwheel_cb(lv_event_t *) {
    lv_screen_load_anim(clickwheelScreen, LV_SCR_LOAD_ANIM_FADE_IN, 250, 0, false);
}

static void leave_clickwheel_cb(lv_event_t *) {
    lv_screen_load_anim(mainScreen, LV_SCR_LOAD_ANIM_FADE_OUT, 250, 0, false);
}

// Click wheel geometry, filled in when it's built.
static lv_point_t wheelCenter;
static int wheelRadius;

// Angle of a touch point around the wheel center, degrees, 0 = right,
// 90 = down (screen coordinates).
static float wheel_angle(const lv_point_t &p) {
    return atan2f((float)(p.y - wheelCenter.y), (float)(p.x - wheelCenter.x)) * 180.0f / (float)M_PI;
}

// Drag state for the ring: a tap is a click on one of the four zones; a drag
// around the ring scrolls the volume, click-wheel style.
static float dragLastAngle;
static float dragTotal;           // |degrees| travelled, to tell a tap from a scroll
static float dragAccum;           // degrees since the last volume step
static constexpr float kDegPerStep = 12.0f;   // one volume step per this much arc
static constexpr int   kVolStep    = 2;

static void wheel_cb(lv_event_t *e) {
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        dragLastAngle = wheel_angle(p);
        dragTotal = dragAccum = 0;
        break;

    case LV_EVENT_PRESSING: {
        float a = wheel_angle(p);
        float d = a - dragLastAngle;
        if (d > 180) d -= 360; else if (d < -180) d += 360;
        dragLastAngle = a;
        dragTotal += fabsf(d);
        dragAccum += d;
        while (dragAccum >= kDegPerStep) {
            dragAccum -= kDegPerStep;
            player_set_volume(min(100, player_volume() + kVolStep));
            volumeFlashUntil = lv_tick_get() + 1500;
        }
        while (dragAccum <= -kDegPerStep) {
            dragAccum += kDegPerStep;
            player_set_volume(max(0, player_volume() - kVolStep));
            volumeFlashUntil = lv_tick_get() + 1500;
        }
        break;
    }

    case LV_EVENT_CLICKED: {
        if (dragTotal > 15) break;               // that was a scroll, not a tap
        float a = wheel_angle(p);
        if (a > -45 && a <= 45)        player_next();          // right
        else if (a > 45 && a <= 135)   player_stop();          // bottom
        else if (a > -135 && a <= -45) leave_clickwheel_cb(e); // top: "Menu"
        else                           player_prev();          // left
        break;
    }
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Dither screen

static void enter_dither_cb(lv_event_t *) {
    lv_screen_load_anim(ditherScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

static void leave_dither_cb(lv_event_t *) {
    lv_screen_load_anim(clickwheelScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, false);
}

static void build_dither_screen() {
    ditherScreen = lv_obj_create(nullptr);
    lv_obj_set_style_text_font(ditherScreen, font(14), 0);
    lv_obj_set_flex_flow(ditherScreen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ditherScreen, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(ditherScreen, px(12), 0);
    lv_obj_set_style_pad_row(ditherScreen, px(14), 0);
    lv_obj_remove_flag(ditherScreen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *heading = lv_label_create(ditherScreen);
    lv_obj_set_style_text_font(heading, font(16), 0);
    lv_label_set_text(heading, "8-bit sound");

    lv_obj_t *explain = lv_label_create(ditherScreen);
    lv_obj_set_width(explain, lv_pct(100));
    lv_label_set_long_mode(explain, LV_LABEL_LONG_WRAP);
    lv_label_set_text(explain,
        "This CYD has an 8 bit sound generator that may sound tinny or robotic, "
        "the dither option trades that for faint static instead.\n\n"
        "(TPDF dither: random noise added before rounding to 8 bits, shaped "
        "toward frequencies the speaker barely reproduces.)");

    lv_obj_t *ditherBox = lv_checkbox_create(ditherScreen);
    lv_checkbox_set_text(ditherBox, "Dither");
    if (player_dither()) lv_obj_add_state(ditherBox, LV_STATE_CHECKED);
    lv_obj_add_event_cb(ditherBox, dither_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t *back = lv_button_create(ditherScreen);
    lv_obj_set_size(back, px(90), px(40));
    lv_obj_add_event_cb(back, leave_dither_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *lbl = lv_label_create(back);
    lv_label_set_text(lbl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(lbl);
}

// ---------------------------------------------------------------------------
// Click-wheel screen construction

static void build_clickwheel_screen() {
    clickwheelScreen = lv_obj_create(nullptr);
    lv_obj_set_style_text_font(clickwheelScreen, font(14), 0);
    lv_obj_set_style_bg_color(clickwheelScreen, lv_color_hex(0xF4F4F4), 0);
    lv_obj_set_style_bg_opa(clickwheelScreen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(clickwheelScreen, LV_OBJ_FLAG_SCROLLABLE);

    // Geometry.  Portrait: the LCD across the top, the wheel centred below it
    // (the 240x320 design: a 204x108 LCD 14 px down, an 86 px-radius wheel at
    // y=128).  Landscape: the LCD on the left half, the wheel on the right.
    lv_display_t *disp = lv_display_get_default();
    const int W = lv_display_get_horizontal_resolution(disp);
    const int H = lv_display_get_vertical_resolution(disp);
    const bool landscape = W > H;
    const int m = px(14);                         // outer margin
    int lcdW, lcdH, lcdX, lcdY, wheelX, wheelY;
    if (!landscape) {
        lcdW = px(204);  lcdH = px(108);
        lcdX = (W - lcdW) / 2;  lcdY = m;
        int below = lcdY + lcdH + px(6);          // top of the wheel's area
        wheelRadius = px(86);
        int fit = (H - below - px(20)) / 2;       // leave room for the "8" corner
        if (wheelRadius > fit) wheelRadius = fit;
        wheelX = (W - 2 * wheelRadius) / 2;  wheelY = below;
    } else {
        lcdW = W / 2 - m - m / 2;  lcdH = px(108);
        lcdX = m;  lcdY = (H - lcdH) / 2;
        wheelRadius = (H - 2 * m) / 2;
        int fit = (W / 2 - m) / 2;
        if (wheelRadius > fit) wheelRadius = fit;
        wheelX = W / 2 + (W / 2 - 2 * wheelRadius) / 2;  wheelY = (H - 2 * wheelRadius) / 2;
    }

    // The little LCD.
    lv_obj_t *lcd = lv_obj_create(clickwheelScreen);
    lv_obj_set_size(lcd, lcdW, lcdH);
    lv_obj_set_pos(lcd, lcdX, lcdY);
    lv_obj_set_style_radius(lcd, px(6), 0);
    lv_obj_set_style_bg_color(lcd, lv_color_hex(0xDDE7F0), 0);
    lv_obj_set_style_border_color(lcd, lv_color_hex(0x30363D), 0);
    lv_obj_set_style_border_width(lcd, px(2), 0);
    lv_obj_set_style_pad_all(lcd, px(6), 0);
    lv_obj_set_style_pad_row(lcd, px(2), 0);
    lv_obj_set_flex_flow(lcd, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(lcd, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(lcd, LV_OBJ_FLAG_SCROLLABLE);
    // Tap the LCD to go back to the track list.
    lv_obj_add_flag(lcd, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(lcd, leave_clickwheel_cb, LV_EVENT_CLICKED, nullptr);

    clickwheelTitle = lv_label_create(lcd);
    lv_obj_set_width(clickwheelTitle, lv_pct(100));
    lv_label_set_long_mode(clickwheelTitle, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_font(clickwheelTitle, font(16), 0);
    lv_obj_set_style_text_color(clickwheelTitle, lv_color_hex(0x1A1F24), 0);
    lv_label_set_text(clickwheelTitle, lv_label_get_text(titleLabel));

    clickwheelInfo = lv_label_create(lcd);
    lv_obj_set_width(clickwheelInfo, lv_pct(100));
    lv_label_set_long_mode(clickwheelInfo, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_color(clickwheelInfo, lv_color_hex(0x4A5560), 0);
    lv_label_set_text(clickwheelInfo, "");

    clickwheelState = lv_label_create(lcd);
    lv_obj_set_width(clickwheelState, lv_pct(100));
    lv_obj_set_style_text_align(clickwheelState, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(clickwheelState, lv_color_hex(0x1A1F24), 0);
    lv_label_set_text(clickwheelState, LV_SYMBOL_STOP);

    clickwheelProgress = lv_bar_create(lcd);
    lv_obj_set_size(clickwheelProgress, lv_pct(100), px(8));
    lv_bar_set_range(clickwheelProgress, 0, 100);
    lv_obj_set_style_bg_color(clickwheelProgress, lv_color_hex(0xB9C6D2), 0);
    lv_obj_set_style_bg_color(clickwheelProgress, lv_color_hex(0x30363D), LV_PART_INDICATOR);

    // The click wheel: one clickable disc, no legends.
    lv_obj_t *wheel = lv_obj_create(clickwheelScreen);
    lv_obj_set_size(wheel, wheelRadius * 2, wheelRadius * 2);
    lv_obj_set_pos(wheel, wheelX, wheelY);
    lv_obj_set_style_radius(wheel, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(wheel, lv_color_hex(0xD6D6D6), 0);
    lv_obj_set_style_bg_color(wheel, lv_color_hex(0xC8C8C8), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(wheel, lv_color_hex(0xB8B8B8), 0);
    lv_obj_set_style_border_width(wheel, 1, 0);
    lv_obj_set_style_pad_all(wheel, 0, 0);
    lv_obj_remove_flag(wheel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(wheel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(wheel, wheel_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(wheel, wheel_cb, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(wheel, wheel_cb, LV_EVENT_CLICKED, nullptr);
    // Screen coordinates of the centre, for wheel_angle() (touch points are
    // absolute, and lv_obj_set_pos() is relative to the parent's padding).
    lv_obj_update_layout(wheel);
    lv_area_t wa;
    lv_obj_get_coords(wheel, &wa);
    wheelCenter.x = (wa.x1 + wa.x2) / 2;
    wheelCenter.y = (wa.y1 + wa.y2) / 2;

    // Center button: play/pause.  A child of the wheel, so it takes the
    // press before the ring sees it.
    lv_obj_t *center = lv_button_create(wheel);
    lv_obj_set_size(center, wheelRadius * 64 / 86, wheelRadius * 64 / 86);
    lv_obj_center(center);
    lv_obj_set_style_radius(center, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(center, lv_color_hex(0xF8F8F8), 0);
    lv_obj_set_style_bg_color(center, lv_color_hex(0xE4E4E4), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(center, lv_color_hex(0xB0B0B0), 0);
    lv_obj_set_style_border_width(center, 1, 0);
    lv_obj_set_style_shadow_width(center, 0, 0);
    lv_obj_add_event_cb(center, play_cb, LV_EVENT_CLICKED, nullptr);

    // The "8" in the lower-right corner: a quiet hint that this is an 8-bit
    // player.  Long-press it for the dither screen.  The label is a box about
    // six characters wide with the glyph right-aligned inside it, so the touch
    // target is a comfortable size while the "8" itself sits in the corner.
    lv_obj_t *eight = lv_label_create(clickwheelScreen);
    lv_label_set_text(eight, "8");
    lv_obj_set_style_text_font(eight, font(16), 0);
    lv_obj_set_style_text_color(eight, lv_color_hex(0xA0A0A0), 0);
    lv_obj_set_style_text_align(eight, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(eight, px(60));
    lv_obj_set_style_pad_all(eight, px(6), 0);
    lv_obj_align(eight, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_flag(eight, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(eight, enter_dither_cb, LV_EVENT_LONG_PRESSED, nullptr);
}

// ---------------------------------------------------------------------------
// Construction

static lv_obj_t *make_button(lv_obj_t *parent, const char *symbol, lv_event_cb_t cb, lv_obj_t **labelOut = nullptr) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, px(50), px(40));
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, symbol);
    lv_obj_center(lbl);
    if (labelOut) *labelOut = lbl;
    return btn;
}

void ui_init(bool sdOk) {
    compute_scale();
    lv_obj_t *screen = lv_screen_active();
    mainScreen = screen;
    lv_obj_set_style_text_font(screen, font(14), 0);      // inherited by everything below
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(screen, px(3), 0);
    lv_obj_set_style_pad_row(screen, px(4), 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    // --- Now-playing panel ------------------------------------------------
    lv_obj_t *panel = lv_obj_create(screen);
    lv_obj_set_width(panel, lv_pct(100));
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(panel, px(8), 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(panel, px(4), 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    // Tap the panel to switch to the click-wheel screen.
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(panel, enter_clickwheel_cb, LV_EVENT_CLICKED, nullptr);

    titleLabel = lv_label_create(panel);
    lv_obj_set_width(titleLabel, lv_pct(100));
    lv_label_set_long_mode(titleLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_font(titleLabel, font(16), 0);
    lv_label_set_text(titleLabel, sdOk ? "No track" : "SD card not found");

    infoLabel = lv_label_create(panel);
    lv_obj_set_width(infoLabel, lv_pct(100));
    lv_label_set_long_mode(infoLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_color(infoLabel, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_label_set_text(infoLabel, "");

    stateLabel = lv_label_create(panel);
    lv_obj_set_width(stateLabel, lv_pct(100));
    lv_obj_set_style_text_align(stateLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(stateLabel, LV_SYMBOL_STOP);

    progressBar = lv_bar_create(panel);
    lv_obj_set_size(progressBar, lv_pct(100), px(6));
    lv_bar_set_range(progressBar, 0, 100);
    lv_bar_set_value(progressBar, 0, LV_ANIM_OFF);

    // --- Track list ---------------------------------------------------------
    trackList = lv_list_create(screen);
    lv_obj_set_width(trackList, lv_pct(100));
    lv_obj_set_flex_grow(trackList, 1);
    // Dense: one line per track, minimal padding, icon hard against the edge.
    lv_obj_set_style_pad_all(trackList, px(2), 0);
    lv_obj_set_style_pad_row(trackList, 0, 0);

    const auto &tracks = player_tracks();
    if (tracks.empty()) {
        lv_list_add_text(trackList, sdOk ? "No *.mp3 files in the card root" : "Insert a card and reset");
    }
    for (size_t i = 0; i < tracks.size(); i++) {
        // "Title - Artist" from the tag, or the filename.  The label's text
        // is copied by LVGL, so the temporary String is fine.
        lv_obj_t *btn = lv_list_add_button(trackList, LV_SYMBOL_AUDIO, tracks[i].label().c_str());
        lv_obj_set_style_pad_ver(btn, px(5), 0);
        lv_obj_set_style_pad_hor(btn, px(6), 0);
        lv_obj_set_style_pad_column(btn, px(6), 0);
        // Exactly one line: fix the label's height to the font and cut the
        // rest off with "..." rather than wrap or scroll 25 rows at once.
        lv_obj_t *lbl = lv_obj_get_child(btn, 1);
        if (lbl) {
            lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
            lv_obj_set_height(lbl, lv_font_get_line_height(lv_obj_get_style_text_font(lbl, LV_PART_MAIN)));
        }
        lv_obj_add_event_cb(btn, track_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        // Checked state marks the current track.
        lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_BLUE), LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_CHECKED);
        lv_obj_set_style_text_color(btn, lv_color_white(), LV_STATE_CHECKED);
        trackButtons.push_back(btn);
    }

    // --- Transport --------------------------------------------------------
    lv_obj_t *transport = lv_obj_create(screen);
    lv_obj_set_width(transport, lv_pct(100));
    lv_obj_set_height(transport, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(transport, px(4), 0);
    lv_obj_set_flex_flow(transport, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(transport, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(transport, LV_OBJ_FLAG_SCROLLABLE);

    make_button(transport, LV_SYMBOL_PREV, prev_cb);
    make_button(transport, LV_SYMBOL_PLAY, play_cb, &playBtnLabel);
    make_button(transport, LV_SYMBOL_NEXT, next_cb);
    make_button(transport, LV_SYMBOL_STOP, stop_cb);

    // --- Volume -------------------------------------------------------------
    lv_obj_t *volRow = lv_obj_create(screen);
    lv_obj_set_width(volRow, lv_pct(100));
    lv_obj_set_height(volRow, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(volRow, px(8), 0);
    lv_obj_set_style_pad_column(volRow, px(10), 0);
    lv_obj_set_flex_flow(volRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(volRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(volRow, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *volIcon = lv_label_create(volRow);
    lv_label_set_text(volIcon, LV_SYMBOL_VOLUME_MAX);

    volumeSlider = lv_slider_create(volRow);
    lv_obj_set_flex_grow(volumeSlider, 1);
    lv_obj_set_style_margin_right(volumeSlider, px(6), 0);   // room for the knob
    lv_slider_set_range(volumeSlider, 0, 100);
    lv_slider_set_value(volumeSlider, player_volume(), LV_ANIM_OFF);
    lv_obj_add_event_cb(volumeSlider, volume_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    // The dither A/B switch lives on its own screen (long-press the "8" on
    // the click-wheel screen), so the track list gets the row it used to occupy.
    build_clickwheel_screen();
    build_dither_screen();

    lv_timer_create(refresh_cb, 250, nullptr);
}
