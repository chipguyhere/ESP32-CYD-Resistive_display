/*
 * TouchCalibrate - guided resistive-touch setup tool for the resistive-touch
 * CYD boards (ESP32-2432S028; ESP32-32E 2.8", 3.2", 3.5", 4.0"; ESP32-024; the board is
 * detected at start-up).
 *
 * A standalone "tool" sketch you flash once, then flash your real application
 * over.  It walks through three stages and saves the results to the ESP32's
 * non-volatile storage preferences partition (NVS) - the small flash area the
 * Arduino `Preferences` library reads and writes, which survives reboots and
 * reflashing - so your application picks them up automatically (the bundled
 * LVGL examples call touch.loadCalibration() / touch.loadFilters() at startup):
 *
 *   1. CALIBRATION - tap a series of crosshairs with a stylus.  A linear fit of
 *      the raw XPT2046 readings is saved as the calibration rectangle.  Sloppy
 *      taps (too brief, or the stylus slid) are rejected and re-asked; if the
 *      overall fit is poor it restarts with more points.
 *
 *   2. FILTER SETTINGS - two LVGL sliders set the touch noise filters:
 *        - Min pressure: ignore touches lighter than this (resistive panels
 *          report a phantom-ish light touch; raise this to suppress it).
 *        - Jump filter:  ignore readings that teleport more than N pixels from
 *          the last point (kills the occasional spike/jump).
 *      The sliders preload the saved values (or sensible defaults).  A live
 *      pressure readout helps you choose.  "Save & continue" stores them.
 *
 *   3. DRAWING TEST - draw on the screen to confirm tracking is smooth.  The
 *      BOOT button clears it; flash your application sketch when satisfied.
 *
 * UI text is drawn with LVGL.  The crosshairs and the drawing dots are blitted
 * straight to the panel: we pause lv_timer_handler, draw rectangles, and only
 * run another LVGL frame (lv_refr_now) when we need the text re-rendered.
 */

#include <lvgl.h>
#include <chipguy_CYD_resistive_display.h>
#include <chipguy_CYD_resistive_touch.h>
#include <esp_heap_caps.h>

chipguy_CYD_resistive_display display;
chipguy_CYD_resistive_touch  touch;

// Screen size in portrait, set from the detected panel in setup():
// 240x320 (2.4", 2.8", 3.2") or 320x480 (3.5", 4.0").
static int SCRW = 240;
static int SCRH = 320;

// Direct-blit colors (standard RGB565; the panel's MADCTL color order is set by
// the display driver, so the same values render correctly as the LVGL path).
static const uint16_t C_BG    = 0x0000;   // black
static const uint16_t C_CROSS = 0xFFFF;   // white
static const uint16_t C_CTR   = 0xF800;   // red
static const uint16_t C_DOT   = 0x07FF;   // cyan

// Example-level default filter values when nothing is saved yet.  The pressure
// metric is now position-normalized, so a modest threshold works everywhere;
// tune it with the live readout on the slider screen.
static const uint16_t DEF_MINP = 100;
static const uint16_t DEF_JUMP = 120;

// Capture-quality thresholds.
static const int   JITTER_RAW  = 130;   // max raw spread within one tap
static const int   MIN_SAMPLES = 8;     // min samples for a valid tap
static const float ACCEPT_RMS  = 6.0f;  // px - accept fit if RMS error <= this
static const float ACCEPT_MAX  = 16.0f; // px - ...and worst-point error <= this

#define CAL_BOOT_PIN 0          // BOOT button (GPIO0)

static bool gCanSkip = false;   // true once a saved calibration exists
static bool gSkipCal = false;   // set when BOOT is pressed to skip calibration

// ── LVGL plumbing ──────────────────────────────────────────────────────────
static lv_display_t *disp;
#define BUF_ROWS 40
static lv_color_t *buf1, *buf2;

static void flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *px) {
    display.flushRegion(area->x1, area->y1, area->x2, area->y2, (uint16_t *)px);
    lv_display_flush_ready(d);
}
static void indev_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    uint16_t x, y;
    if (touch.read(x, y)) {
        data->point.x = x; data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}
static uint32_t tick_cb(void) { return millis(); }

// Render the active screen synchronously, then wait for the blit to finish so
// we can safely draw rectangles straight on top of it.
static void renderFrame() {
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(disp);
    display.waitFlush();
}

// ── direct-blit rectangle (DMA-safe) ───────────────────────────────────────
static void fillRect(int x1, int y1, int x2, int y2, uint16_t color) {
    if (x1 < 0) x1 = 0; if (y1 < 0) y1 = 0;
    if (x2 > SCRW - 1) x2 = SCRW - 1; if (y2 > SCRH - 1) y2 = SCRH - 1;
    if (x2 < x1 || y2 < y1) return;
    int w = x2 - x1 + 1, h = y2 - y1 + 1;
    uint16_t *b = (uint16_t *)heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_DMA);
    if (!b) return;
    for (int i = 0; i < w * h; i++) b[i] = color;
    display.flushRegion(x1, y1, x2, y2, b);
    display.waitFlush();
    heap_caps_free(b);
}
static void drawCrosshair(int cx, int cy) {
    fillRect(0, cy - 1, SCRW - 1, cy + 1, C_CROSS);
    fillRect(cx - 1, 0, cx + 1, SCRH - 1, C_CROSS);
    fillRect(cx - 4, cy - 4, cx + 4, cy + 4, C_CTR);
}

// ── touch capture helpers (raw, for calibration) ───────────────────────────
static void waitRelease() {
    uint16_t a, b;
    uint32_t lastDown = millis();
    while (millis() - lastDown < 150) {
        if (touch.readRaw(a, b)) lastDown = millis();
        delay(5);
    }
}
// Capture one clean tap; returns false if it was too brief or the stylus slid.
static bool captureTap(float &rx, float &ry) {
    waitRelease();
    uint16_t a, b;
    while (!touch.readRaw(a, b)) {                   // wait for press
        if (gCanSkip && digitalRead(CAL_BOOT_PIN) == LOW) { gSkipCal = true; return false; }
        delay(5);
    }

    uint32_t sx = 0, sy = 0; int n = 0;
    uint16_t minx = 4095, maxx = 0, miny = 4095, maxy = 0;
    uint32_t start = millis();
    while (millis() - start < 300) {                // average the hold window
        if (touch.readRaw(a, b)) {
            sx += a; sy += b; n++;
            if (a < minx) minx = a; if (a > maxx) maxx = a;
            if (b < miny) miny = b; if (b > maxy) maxy = b;
        }
        delay(8);
    }
    waitRelease();
    if (n < MIN_SAMPLES) return false;
    if ((maxx - minx) > JITTER_RAW || (maxy - miny) > JITTER_RAW) return false;
    rx = (float)sx / n; ry = (float)sy / n;
    return true;
}

// ── least-squares line fit: D = a + b*I ────────────────────────────────────
static void linfit(const int *I, const float *D, int n, float &a, float &b) {
    float sI = 0, sD = 0, sII = 0, sID = 0;
    for (int i = 0; i < n; i++) {
        sI += I[i]; sD += D[i];
        sII += (float)I[i] * I[i];
        sID += (float)I[i] * D[i];
    }
    float den = n * sII - sI * sI;
    b = den != 0 ? (n * sID - sI * sD) / den : 0;
    a = (sD - b * sI) / n;
}
static uint16_t clampRaw(float v) {
    if (v < 0) v = 0; if (v > 4095) v = 4095;
    return (uint16_t)(v + 0.5f);
}

// ── crosshair target grid ──────────────────────────────────────────────────
#define MAXPTS 30
static int gx[MAXPTS], gy[MAXPTS];
static float rawXs[MAXPTS], rawYs[MAXPTS];

static int genGrid(int cols, int rows) {
    const int M = 26;
    int n = 0;
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            gx[n] = cols <= 1 ? SCRW / 2 : M + c * (SCRW - 1 - 2 * M) / (cols - 1);
            gy[n] = rows <= 1 ? SCRH / 2 : M + r * (SCRH - 1 - 2 * M) / (rows - 1);
            n++;
        }
    return n;
}

// ── screens / widgets ──────────────────────────────────────────────────────
static lv_obj_t *scrCal, *lblCalTitle, *lblCalStatus, *lblCalSkip;
static lv_obj_t *scrSet, *sliMinP, *sliJump, *lblMinP, *lblJump, *lblPress;
static lv_obj_t *scrDraw;
static volatile bool gSaved = false;

static lv_obj_t *makeScreen(uint32_t bgHex) {
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s, lv_color_hex(bgHex), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s, 0, LV_PART_MAIN);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    return s;
}
static lv_obj_t *makeLabel(lv_obj_t *parent, const char *txt, int w) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    if (w > 0) { lv_obj_set_width(l, w); lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP); }
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, txt);
    return l;
}

// The pressure slider has an EXPONENTIAL response: real readings hover near the
// low end (~100), so most of the slider travel maps to small values while the
// top still reaches PSLIDER_PMAX.  The slider widget stays linear (0..PSLIDER_MAX
// steps); these convert between slider step and actual pressure.
static const int   PSLIDER_MAX  = 1000;     // slider steps
static const float PSLIDER_PMAX = 2000.0f;  // max pressure the slider can reach
static const float PSLIDER_K    = 4.0f;     // curve steepness (higher = more low-end)

static uint16_t sliderToPressure(int s) {
    float t = (float)s / PSLIDER_MAX;                                  // 0..1
    float v = (expf(PSLIDER_K * t) - 1.0f) / (expf(PSLIDER_K) - 1.0f); // 0..1, curved
    return (uint16_t)(v * PSLIDER_PMAX + 0.5f);
}
static int pressureToSlider(uint16_t p) {
    float v = (float)p / PSLIDER_PMAX;
    if (v < 0) v = 0; if (v > 1) v = 1;
    float t = logf(1.0f + v * (expf(PSLIDER_K) - 1.0f)) / PSLIDER_K;   // inverse
    return (int)(t * PSLIDER_MAX + 0.5f);
}

static void evSave(lv_event_t *e) { gSaved = true; }
static void evMinP(lv_event_t *e) {
    lv_label_set_text_fmt(lblMinP, "Min pressure: %d",
                          (int)sliderToPressure(lv_slider_get_value(sliMinP)));
}
static void evJump(lv_event_t *e) {
    int v = lv_slider_get_value(sliJump);
    if (v == 0) lv_label_set_text(lblJump, "Jump filter: off");
    else        lv_label_set_text_fmt(lblJump, "Ignore jumps over %d px", v);
}

// ── stage 1: calibration ───────────────────────────────────────────────────
// Returns RMS error in px and fills the calibration rectangle out-params.
static float runCalibration(int cols, int rows,
                            uint16_t &xMin, uint16_t &xMax,
                            uint16_t &yMin, uint16_t &yMax, float &maxErr) {
    int n = genGrid(cols, rows);
    for (int i = 0; i < n; i++) {
        bool ok = false;
        while (!ok) {
            lv_label_set_text_fmt(lblCalStatus, "Point %d / %d\nTap & hold the crosshair", i + 1, n);
            renderFrame();
            drawCrosshair(gx[i], gy[i]);
            ok = captureTap(rawXs[i], rawYs[i]);
            if (gSkipCal) return -1.0f;       // BOOT pressed: skip calibration
            if (!ok) {
                lv_label_set_text(lblCalStatus, "Too shaky - try again\nhold the stylus still");
                renderFrame();
                delay(700);
            }
        }
    }

    // On every board the raw axes are NOT swapped vs. the screen: raw X tracks
    // the screen X axis, raw Y the screen Y axis.  Fit each raw channel
    // against the matching screen coordinate.
    float ax, bx, ay, by;
    linfit(gx, rawXs, n, ax, bx);   // rawX = ax + bx*Sx (X axis)
    linfit(gy, rawYs, n, ay, by);   // rawY = ay + by*Sy (Y axis)

    // read() mirrors raw axes depending on the board (own-touch boards and the
    // ESP32-024 mirror X, the 3.2" and ESP32-035 both, the 3.5"/4.0" Y; the ESP32-024's
    // readRaw() has already swapped its X and Y).  On a mirrored axis the max raw value is at
    // the screen's low edge (S=0) and the min at the high edge.
    const bool mx = touch.mirrorsX(), my = touch.mirrorsY();
    uint16_t rx0 = clampRaw(ax), rx1 = clampRaw(ax + bx * (SCRW - 1));   // rawX at left, right
    uint16_t ry0 = clampRaw(ay), ry1 = clampRaw(ay + by * (SCRH - 1));   // rawY at top, bottom
    xMin = mx ? rx1 : rx0;  xMax = mx ? rx0 : rx1;
    yMin = my ? ry1 : ry0;  yMax = my ? ry0 : ry1;
    if (xMin > xMax) { uint16_t t = xMin; xMin = xMax; xMax = t; }
    if (yMin > yMax) { uint16_t t = yMin; yMin = yMax; yMax = t; }

    // Residual error in screen pixels (mirrors read()'s mapping).
    float sumSq = 0; maxErr = 0;
    for (int i = 0; i < n; i++) {
        float preSx  = (xMax != xMin) ? (rawXs[i] - xMin) * (SCRW - 1.0f) / (xMax - xMin) : 0;
        float preSy  = (yMax != yMin) ? (rawYs[i] - yMin) * (SCRH - 1.0f) / (yMax - yMin) : 0;
        float predSx = mx ? (SCRW - 1) - preSx : preSx;
        float predSy = my ? (SCRH - 1) - preSy : preSy;
        float ex = predSx - gx[i], ey = predSy - gy[i];
        float err = sqrtf(ex * ex + ey * ey);
        sumSq += err * err;
        if (err > maxErr) maxErr = err;
    }
    return sqrtf(sumSq / n);
}

void setup() {
    Serial.begin(115200);
    delay(200);

    display.begin(0);                 // portrait
    SCRW = display.width();
    SCRH = display.height();
    Serial.printf("Panel %s, %dx%d\n", display.panelName(), SCRW, SCRH);
    touch.begin(display);             // after the display: shared-touch boards share its bus
    touch.setRotation(0);
    pinMode(CAL_BOOT_PIN, INPUT_PULLUP);

    // If a calibration is already stored, it's applied now and BOOT may skip
    // stage 1 (go straight to the filter settings).
    gCanSkip = touch.loadCalibration();

    // Capture starting filter values to preload the sliders later.
    bool hadFilt = touch.loadFilters();
    uint16_t initMinP, initJump;
    touch.getFilters(initMinP, initJump);
    if (!hadFilt) { initMinP = DEF_MINP; initJump = DEF_JUMP; }

    // LVGL init.
    lv_init();
    disp = lv_display_create(SCRW, SCRH);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    size_t bufBytes = (size_t)SCRW * BUF_ROWS * 2;
    buf1 = (lv_color_t *)heap_caps_malloc(bufBytes, MALLOC_CAP_DMA);
    buf2 = (lv_color_t *)heap_caps_malloc(bufBytes, MALLOC_CAP_DMA);
    lv_display_set_buffers(disp, buf1, buf2, bufBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, indev_cb);
    lv_tick_set_cb(tick_cb);

    // Build the calibration screen.
    scrCal = makeScreen(0x000000);
    lblCalTitle = makeLabel(scrCal, "TOUCH CALIBRATION", 220);
    lv_obj_align(lblCalTitle, LV_ALIGN_TOP_MID, 0, 6);
    lblCalStatus = makeLabel(scrCal, "", 220);
    lv_obj_align(lblCalStatus, LV_ALIGN_TOP_MID, 0, 34);
    lblCalSkip = makeLabel(scrCal, gCanSkip ? "Press BOOT to skip touch calibration" : "", 220);
    lv_obj_align(lblCalSkip, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_screen_load(scrCal);

    // Stage 1: calibrate, escalating points until the fit is good enough.
    // (BOOT skips this entirely when a saved calibration already exists.)
    uint16_t xMin, xMax, yMin, yMax;
    float rms = 0, maxErr = 0;
    bool skipped = false;
    const int cols[] = {3, 4, 5}, rows[] = {4, 5, 6};
    for (int t = 0; t < 3; t++) {
        if (t > 0) {
            lv_label_set_text(lblCalStatus, "Not accurate enough\nadding more points...");
            renderFrame();
            delay(1400);
        }
        rms = runCalibration(cols[t], rows[t], xMin, xMax, yMin, yMax, maxErr);
        if (rms < 0) { skipped = true; break; }          // BOOT pressed
        Serial.printf("round %d: rms=%.1f max=%.1f\n", t, rms, maxErr);
        if (rms <= ACCEPT_RMS && maxErr <= ACCEPT_MAX) break;
    }
    if (skipped) {
        while (digitalRead(CAL_BOOT_PIN) == LOW) delay(10);  // debounce the skip press
        Serial.println("Calibration skipped; keeping saved values.");
    } else {
        touch.setCalibration(xMin, xMax, yMin, yMax);
        touch.saveCalibration();
        Serial.printf("Saved calibration: xMin=%u xMax=%u yMin=%u yMax=%u\n",
                      xMin, xMax, yMin, yMax);
    }

    // Stage 2: filter sliders.  Disable filters now so the UI stays responsive;
    // the slider values are applied & saved on "Save & continue".
    touch.setFilters(0, 0);
    scrSet = makeScreen(0x101418);
    lv_obj_t *t2 = makeLabel(scrSet, "TOUCH FILTERS", 220);
    lv_obj_align(t2, LV_ALIGN_TOP_MID, 0, 8);

    lblMinP = makeLabel(scrSet, "", 220);
    lv_obj_align(lblMinP, LV_ALIGN_TOP_MID, 0, 40);
    sliMinP = lv_slider_create(scrSet);
    lv_obj_set_width(sliMinP, 200);
    lv_slider_set_range(sliMinP, 0, PSLIDER_MAX);
    lv_slider_set_value(sliMinP, pressureToSlider(initMinP), LV_ANIM_OFF);
    lv_obj_align(sliMinP, LV_ALIGN_TOP_MID, 0, 64);
    lv_obj_add_event_cb(sliMinP, evMinP, LV_EVENT_VALUE_CHANGED, NULL);

    lblJump = makeLabel(scrSet, "", 220);
    lv_obj_align(lblJump, LV_ALIGN_TOP_MID, 0, 108);
    sliJump = lv_slider_create(scrSet);
    lv_obj_set_width(sliJump, 200);
    lv_slider_set_range(sliJump, 0, 200);
    lv_slider_set_value(sliJump, initJump, LV_ANIM_OFF);
    lv_obj_align(sliJump, LV_ALIGN_TOP_MID, 0, 132);
    lv_obj_add_event_cb(sliJump, evJump, LV_EVENT_VALUE_CHANGED, NULL);

    lblPress = makeLabel(scrSet, "Pressure: 0", 220);
    lv_obj_align(lblPress, LV_ALIGN_TOP_MID, 0, 176);

    lv_obj_t *btn = lv_button_create(scrSet);
    lv_obj_set_size(btn, 180, 46);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_add_event_cb(btn, evSave, LV_EVENT_CLICKED, NULL);
    lv_obj_t *btnl = lv_label_create(btn);
    lv_label_set_text(btnl, "Save & continue");
    lv_obj_center(btnl);

    evMinP(NULL); evJump(NULL);        // set initial value labels
    lv_screen_load(scrSet);

    gSaved = false;
    while (!gSaved) {
        lv_timer_handler();
        lv_label_set_text_fmt(lblPress, "Pressure: %u", touch.pressure());
        delay(5);
    }
    uint16_t valMinP = sliderToPressure(lv_slider_get_value(sliMinP));
    uint16_t valJump = lv_slider_get_value(sliJump);
    touch.setFilters(valMinP, valJump);
    touch.saveFilters();
    Serial.printf("Saved filters: minPressure=%u maxJump=%u\n", valMinP, valJump);

    // Stage 3: drawing test.  LVGL renders the instructions once; from here the
    // loop blits dots directly and only re-renders text on a BOOT clear.
    scrDraw = makeScreen(0x000000);
    lv_obj_t *t3 = makeLabel(scrDraw,
        "Drawing test\n\nBOOT: clear screen\nRESET: redo calibration\n\n"
        "or flash your application\nsketch to continue", 220);
    lv_obj_align(t3, LV_ALIGN_TOP_MID, 0, 8);
    lv_screen_load(scrDraw);
    renderFrame();
}

void loop() {
    // Drawing test: blit a dot wherever a (filtered) touch lands.
    uint16_t x, y;
    if (touch.read(x, y)) {
        fillRect((int)x - 2, (int)y - 2, (int)x + 2, (int)y + 2, C_DOT);
    }
    // BOOT clears the canvas by re-rendering the LVGL instruction frame.
    if (digitalRead(CAL_BOOT_PIN) == LOW) {
        renderFrame();
        while (digitalRead(CAL_BOOT_PIN) == LOW) delay(10);   // debounce release
    }
}
