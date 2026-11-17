/*******************************************************************************
 * Resistive-touch CYD -- Raster bouncing balls, in step with the panel's scan
 *
 * A teaching demo.  The balls bypass LVGL and have no frame buffer: each
 * 16-row chunk of the ball area is drawn into one of three small bounce
 * buffers just before it is sent, and the sending is timed against the
 * panel's own refresh, read back with command 0x45, so that the picture never
 * tears.  Only the control strip at the bottom is LVGL.
 *
 * Before the animation it walks through what makes that possible, one screen
 * at a time (Next, or the BOOT button):
 *   1. The SPI clock.  Whether this board has passed the 80 MHz speed test
 *      (the same test as TouchCalibrate's); if it hasn't been tested, it runs
 *      the test, and records the result.
 *   2. The scan line.  Whether the panel answers 0x45, and how its reply is
 *      laid out (found by trying every layout).
 *   3. The refresh rate, timed against the ESP32's clock.
 *   4. How long the ball area takes to send.
 *      If that's too slow for the scan, how slow a refresh would make it
 *      fit -- and, if the user wants, a try at that rate with test patterns
 *      to judge the flicker, and a saved choice (see refresh_tuning_pages()).
 *   5. The verdict: tear-free at the refresh rate, at half of it, or not at
 *      all -- and the compromise in the LVGL strip.
 * See raster.cpp for the timing, raster.h for the tasks.
 *
 * Bounce-buffer RAM: config.h, with presets from generous to conservative.
 *
 * Portrait (rotation 0) only: the ball area must be the first rows the panel
 * scans.
 *
 * Requires the following libraries:
 *   - chipguy_ESP32-CYD-Resistive_display  (this library)
 *   - lvgl 9
 *
 * The raster task also watches the render task: if it overruns at its high
 * priority and starves LVGL, the watchdog makes that obvious (raster.h).
 * raster_watchdog(false) turns it off.
 *
 * Arduino IDE Board Settings: as the other examples ("ESP32 Dev Module" for
 * the classic CYDs).  Serial prints the measurements and a status line every
 * second.  Keys typed into it: on the teaching screens 'n' presses the
 * right-hand button, 's' the left-hand one and 'f' Flip; while the balls run,
 * 'm' / 'l' set both sliders to the top / back to their start, 'h' makes the render
 * task overrun (to see the watchdog), 'j' slows each chunk by 1.2 ms (too
 * slow for full rate on a 60 Hz board, fine at half rate) and 'k' stops that, 'w' / 'W' turn the
 * watchdog off / on, '2' / '1' switch to half rate (a frame every two
 * refreshes) / back to full rate where the board can do it (see
 * raster_set_half_rate() in raster.h), 'a' turns the automatic drop to half
 * rate on / off, 'q' turns the status line off / on, 'p' freezes /
 * unfreezes the frame-rate readout.
 *
 * Copyright (c) 2026 chipguyhere
 * MIT License
 ******************************************************************************/

#include "lv_conf.h"
#include "lvgl.h"
#include "lv_setup.hpp"
#include "scan.h"
#include "raster.h"
#include "balls.h"

chipguy_CYD_resistive_display *g_display = &display;   // for scan.cpp and raster.cpp

// The LVGL strip's height at the bottom; the balls get the rows above it.
#define STRIP_ROWS 80
#define START_BALLS 30
#define START_SPEED 20          // 1..100; shown as x 0.05 .. 5.00, 20 = 1.00x
#define START_SEE_THROUGH true

// The raster task is the highest priority on the SPI interrupt's core; the
// render task one below it.  LVGL (loop()) runs at 1.
#define RASTER_PRIORITY (configMAX_PRIORITIES - 4)

#if CYDR_CHIP_C5
#define BOOT_PIN 28
#else
#define BOOT_PIN 0
#endif

static int W = 240, H = 320, BALL_ROWS = 240;
static RasterPlan g_plan;

// A slower refresh the user can pick: the step, its rate, and refreshes per
// frame (1, or 2 for one frame every two refreshes).  Defined up here: a type
// in a function's signature must come before the .ino's first function.
struct RateOption { int step; float hz; int periods; };

// The longest single lv_timer_handler() pass, for the status line.
static volatile uint32_t g_lvMaxUs = 0;
static void lv_handler_timed() {
    uint32_t t = micros();
    lv_timer_handler();
    t = micros() - t;
    if (t > g_lvMaxUs) g_lvMaxUs = t;
}


/* ================================================================ pages */

static volatile int g_choice = -1;
static lv_obj_t *g_page = nullptr;

static void page_btn_cb(lv_event_t *e) { g_choice = (int)(intptr_t)lv_event_get_user_data(e); }

static lv_obj_t *add_text(lv_obj_t *body, const char *text, uint32_t color = 0xD8DCE4) {
    lv_obj_t *l = lv_label_create(body);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

static lv_obj_t *add_button(lv_obj_t *scr, const char *text, int x, int w, int id, bool primary) {
    lv_obj_t *b = lv_button_create(scr);
    lv_obj_set_size(b, w, 34);
    lv_obj_set_pos(b, x, H - 40);
    if (!primary) lv_obj_set_style_bg_color(b, lv_color_hex(0x404850), 0);
    lv_obj_add_event_cb(b, page_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)id);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

// A page: a title, a scrolling body (add_text() into it), a primary button
// on the right (also the BOOT button) and an optional secondary on the left.
static lv_obj_t *page(const char *title, const char *primary, const char *secondary = nullptr) {
    lv_obj_t *old = g_page;
    lv_obj_t *scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *t = lv_label_create(scr);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0x80C8FF), 0);
    lv_label_set_text(t, title);
    lv_obj_set_pos(t, 8, 6);
    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_set_pos(body, 0, 30);
    lv_obj_set_size(body, W, H - 30 - 46);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 8, 0);
    lv_obj_set_style_pad_row(body, 8, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    if (secondary) add_button(scr, secondary, 6, W / 2 - 9, 1, false);
    if (primary)   add_button(scr, primary, W / 2 + 3, W / 2 - 9, 0, true);
    lv_screen_load(scr);
    if (old) lv_obj_delete(old);
    g_page = scr;
    g_choice = -1;
    // On Serial too: which page, and its buttons (n / s answer them).
    Serial.printf("[page] %s | %s | %s   (lvgl max %u us)\n", title, primary ? primary : "-",
                  secondary ? secondary : "-", (unsigned)g_lvMaxUs);
    g_lvMaxUs = 0;
    return body;
}

// Wait for a button (0 primary / BOOT, 1 secondary).  tick() every 150 ms.
static int page_wait(void (*tick)() = nullptr) {
    pinMode(BOOT_PIN, INPUT_PULLUP);
    while (digitalRead(BOOT_PIN) == LOW) { lv_timer_handler(); delay(5); }
    uint32_t last = 0, downAt = 0;
    while (g_choice < 0) {
        lv_handler_timed();
        if (digitalRead(BOOT_PIN) == LOW) { if (!downAt) downAt = millis(); }
        else if (downAt) { if (millis() - downAt > 30) g_choice = 0; downAt = 0; }
        if (Serial.available()) {          // from the Serial Monitor: n = primary, s = secondary
            int c = Serial.read();
            if (c == 'n') g_choice = 0;
            if (c == 's') g_choice = 1;
            if (c == 'f') g_choice = 2;
        }
        if (tick && millis() - last >= 150) { last = millis(); tick(); }
        delay(5);
    }
    return g_choice;
}

// Show a page now (for one that is followed by a long measurement).
static void page_show_now() {
    lv_refr_now(nullptr);
    display.waitFlush();
}

static char g_buf[600];
#define FMT(...) (snprintf(g_buf, sizeof g_buf, __VA_ARGS__), g_buf)

/* ================================================================ 1. SPI clock */

static lv_obj_t *g_progress;
static void test_progress(uint32_t elapsed, uint32_t total) {
    static uint32_t shown = UINT32_MAX;
    uint32_t left = (total - min(elapsed, total) + 999) / 1000;
    if (left == shown) return;
    shown = left;
    lv_label_set_text_fmt(g_progress, "Testing... %u s", (unsigned)left);
    lv_refr_now(nullptr);                  // drawn over the static between windows
}

static void run_speed_test(cydr_speed_test_t &rec) {
    lv_obj_t *b = page("Speed test", "Start");
    add_text(b, "Sends random dots at double speed, then reads them back to check "
                "each one arrived.");
    add_text(b, "For 20 seconds the screen shows flickering static and the LED flashes. "
                "That's the test, not a fault.", 0xFFD040);
    page_wait();
    b = page("Speed test", nullptr);
    g_progress = add_text(b, "Testing...");
    page_show_now();
    int n = 0;
    int bad = display.stressTest(80000000, 20000, test_progress, &n);
    Serial.printf("Speed test at 80 MHz: %d of %d windows wrong\n", bad, n);
    if (bad < 0) {
        b = page("Speed test", "Next");
        add_text(b, "The test couldn't run (out of memory). Staying at normal speed.", 0xFFD040);
        page_wait();
        return;
    }
    rec = {(uint8_t)(bad ? CYDR_SPEED_FAILED : CYDR_SPEED_PASSED), 80000000, n, bad};
    cydr_saveSpeedTest(rec);
    if (bad == 0) {
        b = page("Speed test", "Next", "Save");
        add_text(b, FMT("Passed: all %d patches arrived right. This demo will use double "
                        "speed (80 MHz).", n), 0x80FF80);
        add_text(b, "Save makes every sketch on this board use it too.");
        if (page_wait() == 1) {
            cydr_overrides_t o;
            cydr_loadOverrides(o);
            o.spiClock = 3;
            cydr_saveOverrides(o);
            b = page("Speed test", "Next");
            add_text(b, "Saved. TouchCalibrate (Advanced > SPI clock) can undo it.");
            page_wait();
        }
    } else {
        b = page("Speed test", "Next");
        add_text(b, FMT("%d of %d patches arrived wrong. This board stays at normal speed "
                        "(40 MHz).", bad, n), 0xFFD040);
        page_wait();
    }
}

static void spi_clock_pages() {
    lv_obj_t *b;
    if (display.readback() == CYDR_READBACK_NONE) {
        b = page("SPI clock", "Next");
        add_text(b, "This board can't read back from its screen, so it can't test double "
                    "speed. It stays at normal speed.");
        page_wait();
        return;
    }
    cydr_speed_test_t rec;
    bool have = cydr_loadSpeedTest(rec);
    for (;;) {
        if (!have) {
            b = page("SPI clock", "Test");
            add_text(b, "Everything you see goes to the screen over a wire link (SPI). Normal speed is "
                        "40 MHz. Many boards also work at double speed, 80 MHz.");
            add_text(b, "This board hasn't been tested at double speed yet. The test takes "
                        "20 seconds.", 0xFFD040);
            page_wait();
            run_speed_test(rec);
            have = rec.result != CYDR_SPEED_UNTESTED;
            if (!have) break;
            continue;
        }
        bool passed = rec.result == CYDR_SPEED_PASSED;
        b = page("SPI clock", "Next", "Re-test");
        add_text(b, FMT("This board was tested at double speed (80 MHz) before. It %s.",
                        passed ? "passed" : "failed"), passed ? 0x80FF80 : 0xFFD040);
        add_text(b, passed ? "So this demo uses double speed."
                           : "So this demo stays at normal speed (40 MHz).");
        if (page_wait() == 0) break;
        run_speed_test(rec);
    }
    if (have && rec.result == CYDR_SPEED_PASSED) display.changeSpiClock(80000000);
    else if (display.spiClock() > 40000000) display.changeSpiClock(40000000);
    Serial.printf("SPI clock: %u Hz\n", (unsigned)display.spiClock());
}

/* ================================================================ 2. the scan line */

static ScanModel g_model;

// A growing row of dots while the screen is timed.
static lv_obj_t *g_dots;
static void dots_tick() {
    lv_label_ins_text(g_dots, LV_LABEL_POS_LAST, ".");
    lv_refr_now(nullptr);
}
static bool measure_with_dots(const char *title, const char *what, ScanModel &m) {
    lv_obj_t *b = page(title, nullptr);
    add_text(b, what, 0xFFD040);
    g_dots = add_text(b, "", 0xFFFFFF);
    lv_obj_set_style_text_font(g_dots, &lv_font_montserrat_18, 0);
    page_show_now();
    return scan_measure(m, 2000, dots_tick);
}

static bool scan_pages() {
    lv_obj_t *b = page("Where is the scan?", nullptr);
    add_text(b, "The screen refreshes line by line, top to bottom, many times a "
                "second.");
    add_text(b, "Asking it which line it is drawing (command 0x45)...", 0xFFD040);
    page_show_now();
    char why[160] = "";
    bool ok = !display.rowsReversed() &&
              scan_detect(g_model, display.nativeHeight(), why, sizeof why);
    if (display.rowsReversed()) snprintf(why, sizeof why, "the rows are mirrored (an override)");
    if (!ok) {
        Serial.printf("Scan line unavailable: %s\n", why);
        b = page("Where is the scan?", "Next");
        if (display.readback() == CYDR_READBACK_NONE) {
            // No return traffic at all.  Stated, not explained how to fix: a
            // board change is for those who know they want one.
            add_text(b, "No answer: this board gets no data back from its screen.", 0xFFD040);
            add_text(b, "Some CYDs don't report sync due to an unpopulated 0-ohm R24.");
        } else {
            add_text(b, "No answer this demo can use.", 0xFFD040);
        }
        add_text(b, "So the balls can't be timed to the screen. They will still run, but "
                    "with a \"tear\": a line where the old frame meets the new.");
        page_wait();
        return false;
    }
    b = page("Where is the scan?", "Next");
    add_text(b, "Your screen answers: it reports a usable sync signal, so the balls can "
                "be timed to match its refreshes.", 0x80FF80);
    page_wait();
    return true;
}

/* ================================================================ 3. refresh rate */

static bool refresh_pages() {
    lv_obj_t *b;
    if (!measure_with_dots("Refresh rate", "Timing the screen", g_model)) {
        b = page("Refresh rate", "Next");
        add_text(b, "The answers weren't steady enough to time the balls to.", 0xFFD040);
        page_wait();
        return false;
    }
    const ScanModel &m = g_model;
    b = page("Refresh rate", "Next");
    add_text(b, FMT("Your screen refreshes %.0f times per second.", m.hz()), 0x80FF80);
    add_text(b, "This is the screen's native refresh rate using the screen's standard "
                "settings, to give the best possible picture based on the characteristics "
                "of its LCD hardware.");
    page_wait();
    return true;
}

/* ================================================================ 4. transfer */

static void transfer_pages() {
    lv_obj_t *b = page("Sending a frame", nullptr);
    add_text(b, "Timing how long a frame takes to send...", 0xFFD040);
    page_show_now();
    double rowUs, winUs;
    raster_measure(W, BALL_ROWS, rowUs, winUs);
    g_plan.rowUs = rowUs;
    g_plan.windowUs = winUs;
    Serial.printf("Transfer: %.2f us a row, window %.0f us, area %.2f ms at %u Hz\n",
                  rowUs, winUs, (winUs + rowUs * BALL_ROWS) / 1000, (unsigned)display.spiClock());
    b = page("Sending a frame", "Next");
    const double sendUs = winUs + rowUs * BALL_ROWS;
    add_text(b, FMT("Sending one frame of the balls takes 1/%.0f of a second.", 1e6 / sendUs),
             0x80FF80);
    if (g_plan.scanKnown)
        add_text(b, FMT("In that time the screen refreshes %s.",
                        sendUs < g_model.periodUs() ? "less than once" :
                        sendUs < 2 * g_model.periodUs() ? "between one and two times"
                                                        : "two times or more"));
    // Tear-free at the stock refresh?  (A slower one is the next step's business.)
    RasterPlan stock = g_plan;
    stock.model = g_model;
    raster_plan(stock);
    if (stock.tearFree)
        add_text(b, FMT("If the screen refreshes while a frame is half sent, you see half old, "
                        "half new: a tear. This board can send frames fast enough to avoid "
                        "tearing%s.", stock.stripInStep ? "" :
                        " during the raster animation, but may show tearing in the area "
                        "below it"));
    else
        add_text(b, "If the screen refreshes while a frame is half sent, you see half old, "
                    "half new: a tear. The trick is to start sending at just the right "
                    "moment, so the refresh never catches up.");
    page_wait();
}

/* ================================================================ 4b. a slower refresh */

// The panel's controller can slow its refresh (display.setRefreshStep()).
// A slower scan is easier to stay ahead of, so a board that can't be
// tear-free at its stock rate often can be at a lower one -- at a cost in
// picture quality that only the user can judge.  The library never slows the
// panel by itself; this sketch does it only when the user picks it here, and
// a choice the user keeps is saved (cydr_saveRefreshChoice()) for sketches
// that ask for it with display.applyRefreshChoice().

static ScanModel g_stockModel;   // measured at the stock rate
static bool g_slowed = false;

// The plan if the panel ran at `ratio` of its stock rate -- for the ball
// area, or for `rows` rows (a sketch animating the whole screen).
static RasterPlan plan_at(float ratio, int rows = 0) {
    RasterPlan p = g_plan;
    if (rows) p.ballRows = rows;
    p.model = g_stockModel;
    p.model.lineUs = g_stockModel.lineUs / ratio;
    raster_plan(p);
    return p;
}
static float plan_fps(const RasterPlan &p) { return p.tearFree ? p.model.hz() / p.periods : 0; }

// A frame rate as the user sees it: the refresh rate as a whole number, or
// half of it -- with ".5" when that whole number is odd (59 refreshes per
// second, a frame every other refresh: 29.5).  Two buffers, so it can appear
// twice in one message.
static const char *rate_text(float hz, int periods) {
    static char buf[2][12];
    static int k = 0;
    char *b = buf[k ^= 1];
    int r = (int)lroundf(hz);
    if (periods == 2) snprintf(b, sizeof buf[0], (r & 1) ? "%d.5" : "%d", r / 2);
    else snprintf(b, sizeof buf[0], "%d", r);
    return b;
}

// Re-time the scan after a rate change.
static bool remeasure() {
    ScanModel m = g_model;
    if (!measure_with_dots("Refresh rate", "Timing the screen again", m)) return false;
    g_model = m;
    g_plan.model = m;
    return true;
}

static void back_to_stock() {
    display.setRefreshStep(0, g_stockModel.hz());
    g_model = g_stockModel;
    g_plan.model = g_stockModel;
    g_slowed = false;
}

// Patterns that show a slow refresh first: a 1-pixel checkerboard,
// 1-pixel vertical stripes, 1-pixel lines, and 50 % grey.  Drawn straight
// to the panel over part of the page, from LVGL's own render buffer.
static void draw_patterns(int y0, int rows) {
    display.waitFlush();
    uint16_t *buf = lv_setup.drawBuffer(0);
    const int cap = (int)(lv_setup.drawBufferBytes() / (W * 2));
    const int band = rows / 4;
    for (int y = y0; y < y0 + rows;) {
        int h = min(cap, y0 + rows - y);
        for (int r = 0; r < h; r++)
            for (int x = 0; x < W; x++) {
                int yy = y + r - y0, k = yy / band;
                uint16_t c = k == 0 ? (((x ^ yy) & 1) ? 0xFFFF : 0) :
                             k == 1 ? ((x & 1) ? 0xFFFF : 0) :
                             k == 2 ? ((yy & 1) ? 0xFFFF : 0) : 0x8410;
                buf[r * W + x] = c;
            }
        display.flushRegion(0, y, W - 1, y + h - 1, buf);
        display.waitFlush();
        y += h;
    }
}

static lv_obj_t *g_rateLabel;

// Try the options on the panel: test patterns, with Flip cycling through
// them and the normal rate.  The left button keeps whatever is showing, the
// right one (and BOOT) the normal rate.  Returns the chosen option, or -1 for
// the normal rate.  fullOpt: the option that's tear-free for the whole
// screen too (marked as such), or -1.
static int try_options(const RateOption *opt, int n, int fullOpt = -1) {
    const float stockHz = (float)g_stockModel.hz();
    lv_obj_t *b = page("Look closely", nullptr);
    lv_obj_set_height(b, H - 30 - 46 - 116);
    add_text(b, n > 1 ? "Look for flicker in the patterns below. Flip cycles through the "
                        "choices and normal."
                      : "Look for flicker in the patterns below. Flip switches between "
                        "slowed and normal.");
    g_rateLabel = add_text(b, "", 0xFFFFFF);
    char useStock[12];
    snprintf(useStock, sizeof useStock, "Use %d", (int)lroundf(stockHz));
    add_button(g_page, useStock, W - 78, 72, 0, true);
    add_button(g_page, "Flip", 84, 72, 2, false);
    lv_obj_t *useShown = lv_obj_get_child(add_button(g_page, "", 6, 72, 1, false), 0);
    int shown = 0;                                  // 0..n-1 an option, n the normal rate
    auto show = [&]() {
        bool normal = shown == n;
        float hz = normal ? stockHz : opt[shown].hz;
        display.setRefreshStep(normal ? 0 : opt[shown].step, stockHz);
        lv_label_set_text_fmt(g_rateLabel, "Now: %s, %d refreshes per second%s%s",
                              normal ? "normal" : "slowed", (int)lroundf(hz),
                              normal ? "" : opt[shown].periods == 2 ? "\n(a frame every two)"
                                                                     : "\n(a frame every refresh)",
                              !normal && shown == fullOpt ? "\nWhole screen tear-free too" : "");
        lv_label_set_text_fmt(useShown, "Use %d", (int)lroundf(hz));
        Serial.printf("[page] Look closely | %s | Use %d | Flip (f)\n", useStock, (int)lroundf(hz));
    };
    show();
    page_show_now();
    draw_patterns(H - 46 - 112, 112);
    for (;;) {
        int c = page_wait();
        if (c == 0) { display.setRefreshStep(0, stockHz); return -1; }
        if (c == 1) return shown == n ? -1 : shown;
        shown = (shown + 1) % (n + 1);
        show();
        g_choice = -1;
    }
}

static void refresh_tuning_pages() {
    if (!g_plan.scanKnown || display.refreshStepCount() < 2) return;
    g_stockModel = g_model;
    const float stockHz = (float)g_stockModel.hz();
    const RasterPlan stockPlan = plan_at(1);
    const float stockFps = plan_fps(stockPlan);

    // Best available step: the most frames per second, tear-free, above the floor.
    int best = -1, bestPeriods = 0;
    float bestFps = stockFps, bestHz = 0;
    for (int s = 1; s < display.refreshStepCount(); s++) {
        float r = display.refreshStepRatio(s);
        if (stockHz * r < CYDR_MIN_REFRESH_HZ) break;
        // Judged 2 % faster than predicted: the step's real rate comes out a
        // little off, and the panel's oscillator drifts with temperature.  A
        // step that only just fits would fall to one frame per two refreshes.
        RasterPlan pr = plan_at(r * 1.02f);
        float f = pr.tearFree ? (float)(stockHz * r / pr.periods) : 0;
        if (f > bestFps + 0.5f) { best = s; bestFps = f; bestHz = stockHz * r; bestPeriods = pr.periods; }
    }
    // And the fastest rate that would be tear-free at all, as a continuous
    // estimate (the controller's steps fall either side of it).
    float lo = CYDR_MIN_REFRESH_HZ / stockHz, hi = 1;
    bool any = plan_at(lo).tearFree;
    if (any && !plan_at(1).tearFree) {
        for (int i = 0; i < 30; i++) {
            float mid = (lo + hi) / 2;
            if (plan_at(mid).tearFree) lo = mid; else hi = mid;
        }
    }
    const float needHz = stockHz * lo;
    // When the best is one frame per refresh at a slow rate, one frame every
    // two refreshes at about twice the rate gives nearly the same frames per
    // second with much less flicker: offer the fastest such step instead.
    RateOption opts[2];
    int nOpts = 0;
    if (best >= 0) {
        opts[nOpts++] = {best, bestHz, bestPeriods};
        if (bestPeriods == 1) {
            for (int s = 1; s < display.refreshStepCount(); s++) {
                float r = display.refreshStepRatio(s);
                if (stockHz * r < 1.5f * bestHz) break;
                RasterPlan pr = plan_at(r * 1.02f);
                if (pr.tearFree && pr.periods == 2 &&
                    (stockFps == 0 || stockHz * r / 2 > stockFps + 0.5f)) {
                    opts[0] = {s, stockHz * r, 2};
                    break;
                }
            }
        }
    }
    // A sketch that animates the whole screen, not just a ball area, needs
    // more: the write drifts against the scan over every row.  Where neither
    // the normal rate nor the balls' choice is tear-free for all the rows,
    // offer too the fastest step that is, so the saved choice can serve such
    // sketches (they apply it with display.applyRefreshChoice()).
    const int fullRows = H / CHUNK_ROWS * CHUNK_ROWS;
    int fullOpt = -1, full = -1;
    float fullHz = 0;
    if (!plan_at(1, fullRows).tearFree) {
        for (int s = 1; s < display.refreshStepCount(); s++) {
            float r = display.refreshStepRatio(s);
            if (stockHz * r < CYDR_MIN_REFRESH_HZ) break;
            if (plan_at(r * 1.02f, fullRows).tearFree) { full = s; fullHz = stockHz * r; break; }
        }
        bool covered = nOpts > 0 &&
                       plan_at(display.refreshStepRatio(opts[0].step) * 1.02f, fullRows).tearFree;
        if (full >= 0 && !covered) {
            RasterPlan pb = plan_at(display.refreshStepRatio(full) * 1.02f);   // the balls at that rate
            opts[nOpts] = {full, fullHz, pb.tearFree ? pb.periods : 2};
            fullOpt = nOpts++;
        } else if (full >= 0) {
            fullOpt = 0;                       // the balls' choice does for the whole screen
        }
    }
    Serial.printf("Refresh tuning: stock %.2f Hz (%.1f fps tear-free), tear-free from %.1f Hz, "
                  "best step %d at %.1f Hz -> %.1f fps; whole screen (%d rows) tear-free from "
                  "step %d at %.1f Hz\n",
                  stockHz, stockFps, any ? needHz : 0, best, bestHz, bestFps, fullRows, full, fullHz);

    cydr_refresh_choice_t saved;
    bool haveSaved = cydr_loadRefreshChoice(saved) && saved.panel == display.panel();
    if (haveSaved) {
        lv_obj_t *b = page("A slower refresh", "Use it", "Change");
        add_text(b, FMT("Last time you chose to slow this screen to %.0f refreshes per second "
                        "(normally %.0f).", saved.hz, saved.stockHz), 0x80FF80);
        add_text(b, "Use it again?");
        if (page_wait() == 0 && display.applyRefreshChoice() && remeasure()) {
            g_slowed = true;
            return;
        }
        back_to_stock();
    }

    lv_obj_t *b;
    if (nOpts == 0) {
        b = page("A slower refresh", "Next");
        if (stockFps > 0)
            add_text(b, "No need to slow your screen down: it's fast enough already.",
                     0x80FF80);
        else
            add_text(b, FMT("Even slowed to %.0f refreshes per second, the safe limit, your screen "
                            "would still tear.", CYDR_MIN_REFRESH_HZ), 0xFFD040);
        page_wait();
        return;
    }
    b = page("A slower refresh", "Next", "Try it");
    const RateOption &o = opts[0];
    Serial.printf("Refresh offer: step %d, %.2f Hz, %d refresh(es) a frame%s\n", o.step, o.hz,
                  o.periods, nOpts > 1 ? "; Flip: the whole-screen step" : "");
    if (best < 0)                          // only the whole-screen option
        add_text(b, FMT("The balls are fine at the normal rate. Sketches that animate the whole "
                        "screen would tear at it; slowed to about %.0f refreshes per second, "
                        "they won't.", o.hz));
    else if (stockFps > 0)
        add_text(b, FMT("Slowing your screen to %.0f refreshes per second would make the balls "
                        "smoother: %s frames per second instead of %s.", o.hz,
                        rate_text(o.hz, o.periods), rate_text(stockHz, stockPlan.periods)));
    else
        add_text(b, FMT("A slower screen is easier to keep up with. Slowed from %.0f to about "
                        "%.0f refreshes per second, the balls won't tear: %s frames per "
                        "second, %s.", stockHz, o.hz, rate_text(o.hz, o.periods),
                        o.periods == 2 ? "a frame every two refreshes" : "a frame every refresh"));
    add_text(b, "The cost: some patterns may flicker. It's only a setting, undone by a "
                "restart, and the library won't go slow enough to harm the screen.",
             0xFFD040);
    if (nOpts > 1)
        add_text(b, FMT("Sketches that animate the whole screen need it slower still: about "
                        "%.0f refreshes per second. Flip shows that choice too.", opts[1].hz));
    add_text(b, "Try it to see test patterns at the slower speed.");
    if (page_wait() != 1) { cydr_clearRefreshChoice(); return; }
    int pick = try_options(opts, nOpts, fullOpt);
    if (pick < 0 || !remeasure()) {
        back_to_stock();
        cydr_clearRefreshChoice();
        return;
    }
    g_slowed = true;
    cydr_refresh_choice_t c = {(uint8_t)display.panel(), (uint8_t)opts[pick].step, stockHz,
                               (float)g_model.hz()};
    cydr_saveRefreshChoice(c);
    b = page("A slower refresh", "Next");
    add_text(b, FMT("Slowed to %.0f refreshes per second, and remembered.", g_model.hz()), 0x80FF80);
    add_text(b, "Your other sketches stay normal unless they ask for it with "
                "display.applyRefreshChoice().");
    page_wait();
}

/* ================================================================ 5. verdict */

static void verdict_page() {
    raster_plan(g_plan);
    const RasterPlan &p = g_plan;
    const double P = p.model.periodUs();
    lv_obj_t *b = page("The verdict", "Start");
    if (!p.scanKnown) {
        add_text(b, "The balls will tear: this screen can't say which line it's drawing.",
                 0xFFD040);
    } else if (!p.tearFree) {
        add_text(b, "The balls will tear: frames can't be sent fast enough to stay ahead "
                    "of the screen.", 0xFFD040);
    } else {
        if (p.periods == 1)
            add_text(b, FMT("No tearing! We can run at %s frames per second (the full "
                            "refresh rate), one frame per refresh.", rate_text(p.model.hz(), 1)),
                     0x80FF80);
        else
            add_text(b, FMT("No tearing! We can run at %s frames per second (half the "
                            "refresh rate) by timing one frame to occupy two refresh cycles "
                            "without a tear in between.", rate_text(p.model.hz(), 2)), 0x80FF80);
        add_text(b, "One intentional compromise: the controls at the bottom are allowed to "
                    "tear for a moment while they are changed, to prioritize fluidity of the "
                    "animation.");
    }
    Serial.printf("Plan: scan %d, tear-free %d, periods %d, transfer %.0f us, span %.0f us, "
                  "guard %.0f us, period %.0f us, start +%.0f us; half rate paced at %.1f us a "
                  "row (%.0f us a frame), start +%.0f us\n",
                  p.scanKnown, p.tearFree, p.periods, p.transferUs, p.spanUs, p.guardUs, P,
                  p.startOffsetUs, p.pacedRowUs, p.pacedRowUs * p.ballRows, p.pacedStartOffsetUs);
    page_wait();
}

/* ================================================================ the animation */

// LVGL from here on: full-width areas only, so every transfer is whole rows
// and a multiple of 4 bytes.  Only the width: LVGL also sends this event
// with a trial area from row 0 to size its render chunks, so the rows must
// be left alone.  The flush drops any rows in the ball area instead.
static void rounder_cb(lv_event_t *e) {
    lv_area_t *a = (lv_area_t *)lv_event_get_param(e);
    a->x1 = 0;
    a->x2 = W - 1;
}

static void raster_flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *pixelmap) {
    int y1 = a->y1 < BALL_ROWS ? BALL_ROWS : a->y1;
    if (y1 <= a->y2 && a->x1 == 0 && a->x2 == W - 1) {
        uint16_t *px = (uint16_t *)pixelmap + (size_t)(y1 - a->y1) * W;
        uint32_t *q = (uint32_t *)px;                       // big-endian for the panel
        for (size_t i = 0, n = (size_t)(a->y2 - y1 + 1) * W / 2; i < n; i++) {
            uint32_t v = q[i];
            q[i] = ((v >> 8) & 0x00FF00FFu) | ((v << 8) & 0xFF00FF00u);
        }
        raster_send_rows(px, y1, a->y2);                    // waits until it's out
    }
    lv_display_flush_ready(d);
}

static lv_obj_t *g_countLabel, *g_speedLabel, *g_perf;
static lv_obj_t *g_countSlider = nullptr, *g_speedSlider = nullptr;
static bool g_statusPrint = true, g_readout = true;   // Serial keys 'q' and 'p'

static void set_count(int v) {
    balls_set_count(v);
    lv_label_set_text_fmt(g_countLabel, "%d balls", v);
}
static void set_speed(int v) {
    balls_set_speed(v);
    lv_label_set_text_fmt(g_speedLabel, "%d.%02dx", v * 5 / 100, v * 5 % 100);
}

static void count_cb(lv_event_t *e) { set_count(lv_slider_get_value((lv_obj_t *)lv_event_get_target(e))); }
static void speed_cb(lv_event_t *e) { set_speed(lv_slider_get_value((lv_obj_t *)lv_event_get_target(e))); }

// Both sliders (and what they set) to a value; 'm' / 'l' on Serial.
static void sliders_to(int count, int speed) {
    if (!g_countSlider) return;
    lv_slider_set_value(g_countSlider, count, LV_ANIM_OFF);
    lv_slider_set_value(g_speedSlider, speed, LV_ANIM_OFF);
    set_count(count);
    set_speed(speed);
}
static void see_cb(lv_event_t *e) {
    balls_set_see_through(lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED));
}

static void perf_update(lv_timer_t *) {
    static uint32_t lastMs = millis();
    RasterStats s;
    raster_take_stats(s);
    uint32_t now = millis(), dt = now - lastMs;
    lastMs = now;
    float fps = dt ? s.frames * 1000.0f / dt : 0;
    uint32_t chunkUs = balls_take_max_chunk_us();
    // Counting whole frames over about a second wobbles by one: within 15 %
    // of the rate the plan runs at (the refresh rate, or half of it), show
    // that rate.
    char shown[12];
    snprintf(shown, sizeof shown, "%d", (int)(fps + 0.5f));
    if (g_plan.tearFree) {
        const int periods = raster_periods();
        float target = s.hz / periods;
        if (fabsf(fps - target) <= 0.15f * target)
            snprintf(shown, sizeof shown, "%s", rate_text(s.hz, periods));
    }
    if (!g_readout)
        ;                                          // Serial 'p': readout frozen
    else if (g_plan.tearFree)
        lv_label_set_text_fmt(g_perf, "%s frames per second\nscreen: %d refreshes per second",
                              shown, (int)lroundf(s.hz));
    else
        lv_label_set_text_fmt(g_perf, "%d frames per second\n(may tear)", (int)(fps + 0.5f));
    if (g_statusPrint) Serial.printf("fps %.1f  late %u  slips %u  scan err %.2f ln  panel %.3f Hz  chunk %u us  "
                  "lvgl rows %u (forced %u)  scan skips %u  touch samples %u  lvgl max %u us\n",
                  fps, (unsigned)s.late, (unsigned)s.slips, s.scanErrRms, s.hz,
                  (unsigned)chunkUs, (unsigned)s.lvglRows, (unsigned)s.lvglForced,
                  (unsigned)s.scanSkips, (unsigned)s.touchSamples, (unsigned)g_lvMaxUs);
    g_lvMaxUs = 0;
}

static lv_obj_t *strip_slider(lv_obj_t *scr, int y, int lo, int hi, int v, lv_event_cb_t cb) {
    lv_obj_t *s = lv_slider_create(scr);
    lv_obj_set_size(s, W - 92, 10);
    lv_obj_set_pos(s, 80, y + 4);
    lv_slider_set_range(s, lo, hi);
    lv_slider_set_value(s, v, LV_ANIM_OFF);
    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_ext_click_area(s, 8);
    return s;
}

static void build_strip() {
    lv_obj_t *scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x181820), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    int y = BALL_ROWS + 6;
    g_countLabel = lv_label_create(scr);
    lv_obj_set_style_text_color(g_countLabel, lv_color_white(), 0);
    lv_obj_set_pos(g_countLabel, 8, y);
    lv_label_set_text_fmt(g_countLabel, "%d balls", START_BALLS);
    g_countSlider = strip_slider(scr, y, 1, BALLS_MAX, START_BALLS, count_cb);
    y += 24;
    g_speedLabel = lv_label_create(scr);
    lv_obj_set_style_text_color(g_speedLabel, lv_color_white(), 0);
    lv_obj_set_pos(g_speedLabel, 8, y);
    lv_label_set_text_fmt(g_speedLabel, "%d.%02dx", START_SPEED * 5 / 100, START_SPEED * 5 % 100);
    g_speedSlider = strip_slider(scr, y, 1, 100, START_SPEED, speed_cb);
    y += 24;
    lv_obj_t *sw = lv_switch_create(scr);
    lv_obj_set_size(sw, 40, 20);
    lv_obj_set_pos(sw, 8, y);
    if (START_SEE_THROUGH) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, see_cb, LV_EVENT_VALUE_CHANGED, NULL);
    g_perf = lv_label_create(scr);
    lv_obj_set_style_text_font(g_perf, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(g_perf, lv_color_hex(0xA0A8B8), 0);
    lv_obj_set_pos(g_perf, 56, y - 1);
    lv_label_set_text(g_perf, "");
    lv_timer_create(perf_update, 1000, NULL);
    lv_screen_load(scr);
    if (g_page) lv_obj_delete(g_page);
    g_page = nullptr;
}

static void start_animation() {
    lv_obj_t *b = page("Starting", nullptr);
    add_text(b, "...");
    page_show_now();
    display.waitFlush();
    // Re-time the scan just before starting: the fit is seconds old by now,
    // and a rate error of a fraction of a percent grows to hundreds of lines
    // in that time.  (The raster task's corrections would pull it in, but
    // only after a few seconds out of step.)
    if (g_plan.tearFree) {
        ScanModel m = g_plan.model;
        if (scan_measure(m, 500)) g_plan.model = m;
    }
    // The raster task owns the bus from here: LVGL's rows go through it, and
    // on a board whose touch shares the bus, touch is sampled only by it.
    lv_display_t *d = lv_display_get_default();
    lv_display_set_flush_cb(d, raster_flush_cb);
    lv_display_add_event_cb(d, rounder_cb, LV_EVENT_INVALIDATE_AREA, nullptr);
    lv_setup.touch().setCacheOnly(true);

    balls_init(W, BALL_ROWS);
    balls_set_count(START_BALLS);
    balls_set_speed(START_SPEED);
    balls_set_see_through(START_SEE_THROUGH);
    const int core = display.spiCore();     // the SPI interrupt's core
    TaskHandle_t render = balls_start(core, RASTER_PRIORITY - 1);
    // The raster task watches the render task: if it overruns, starving LVGL,
    // the watchdog makes that obvious (see raster.h).  To turn it off:
    //raster_watchdog(false);
    raster_watch(render, RASTER_PRIORITY - 1, uxTaskPriorityGet(nullptr));   // setup() runs at loop()'s priority
    raster_start(g_plan, core, RASTER_PRIORITY);
    // A sketch that does more work per frame than this one can give itself
    // twice the time: run at half rate (a frame every two refreshes), or let
    // the raster task drop to it if frames start coming out late.
    //raster_set_half_rate(true);
    //raster_auto_half_rate(true);
    build_strip();
    Serial.printf("Running: core %d, internal RAM free %u\n", core,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

/* ================================================================ setup / loop */

void setup() {
    // Serial must never hold the sketch up.  On a UART (the classic and C5
    // boards' USB-serial chip) a print waits for its bytes to go out unless
    // there's a buffer to put them in; on the S3's own USB port it waits for a
    // timeout when no program has the port open.
#if ARDUINO_USB_CDC_ON_BOOT
    Serial.setTxTimeoutMs(0);
#else
    Serial.setTxBufferSize(4096);
#endif
    Serial.begin(115200);
    lv_setup.begin(0);
    W = display.width();
    H = display.height();
    BALL_ROWS = (H - STRIP_ROWS) / CHUNK_ROWS * CHUNK_ROWS;
    Serial.printf("%s, %s, %dx%d, SPI %u Hz\n", display.modelName(), display.panelName(),
                  W, H, (unsigned)display.spiClock());
    if (!raster_alloc(W)) {
        Serial.println("Bounce buffer allocation failed");
        while (1) delay(100);
    }

    lv_obj_t *b = page("Raster balls", "Next");
    add_text(b, "Bouncing balls, drawn a strip at a time just before each strip is sent "
                "to the screen, and timed so you never see half an old frame and half a "
                "new one.");
    add_text(b, "First, a few tests of your board. Press Next, or the BOOT button.");
    add_text(b, FMT("%s, %s panel, %d x %d.", display.modelName(), display.panelName(), W, H),
             0x90A0B0);
    page_wait();

    spi_clock_pages();
    g_plan.W = W;
    g_plan.H = H;
    g_plan.ballRows = BALL_ROWS;
    g_plan.scanKnown = scan_pages() && refresh_pages();
    g_plan.model = g_model;
    transfer_pages();
    refresh_tuning_pages();
    verdict_page();
    start_animation();
}

void loop() {
    // From the Serial Monitor: 'm' both sliders to the top, 'l' back to the start;
    // 'h' makes the render task overrun (to see the watchdog), 'k' stops it;
    // 'w' turns the watchdog off, 'W' on.
    int c = Serial.available() ? Serial.read() : -1;
    if (c == 'm') sliders_to(BALLS_MAX, 100);
    if (c == 'l') sliders_to(START_BALLS, START_SPEED);
    if (c == 'h') { balls_set_overrun_us(30000); Serial.println("render task: overrunning 30 ms a chunk"); }
    if (c == 'j') { balls_set_overrun_us(1200); Serial.println("render task: 1.2 ms extra a chunk (too slow for full rate, fine at paced half rate)"); }
    if (c == 'k') { balls_set_overrun_us(0); Serial.println("render task: normal"); }
    if (c == 'w') { raster_watchdog(false); Serial.println("watchdog off"); }
    if (c == 'W') { raster_watchdog(true); Serial.println("watchdog on"); }
    if (c == '1' || c == '2')
        Serial.printf("%s\n", raster_set_half_rate(c == '2')
                      ? (c == '2' ? "half rate: a frame every two refreshes" : "full rate: a frame every refresh")
                      : "this board's plan can't do a frame every refresh");
    if (c == 'a') {
        static bool autoHalf = false;
        autoHalf = !autoHalf;
        raster_auto_half_rate(autoHalf);
        Serial.printf("automatic half rate %s\n", autoHalf ? "on" : "off");
    }
    if (c == 'q') { g_statusPrint = !g_statusPrint; Serial.printf("status line %s\n", g_statusPrint ? "on" : "off"); }
    if (c == 'p') { g_readout = !g_readout; Serial.printf("readout %s\n", g_readout ? "updating" : "frozen"); }
    lv_handler_timed();
    delay(5);
}
