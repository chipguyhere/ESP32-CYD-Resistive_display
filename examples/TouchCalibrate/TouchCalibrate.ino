/*
 * TouchCalibrate - guided resistive-touch setup tool for the resistive-touch
 * CYD boards (ESP32-2432S028; ESP32-32E 2.8", 3.2", 3.5", 4.0"; ESP32-024;
 * ESP32-035; ESP32-2432S032; the board is detected at start-up).
 *
 * On the capacitive boards (ESP32-2432S032C, -2432S024C, JC2432W328, -3248S035C) there is
 * nothing to calibrate: the GT911 / CST820 reports screen pixels, so the sketch
 * says so and goes straight to stage 3.
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
 * SETTINGS - hold BOOT (or the on-screen BOOT button, for an enclosure that
 * hides the real one) for 2 s on the drawing test.  The settings screen names
 * the board as detected and leads to two lists of overrides, each field with
 * a page of its own listing its choices:
 *   - Options: what can't break the screen or touch -- the screen size (e.g.
 *     a 32E 3.5" / 4.0" is really a 4.0"), the LED pins, GPIO34, the power key
 *     and how it turns the board off (or "absent" when an enclosure hides it).
 *   - Advanced (after a warning): the model itself, panel, color order,
 *     inversion, mirroring, SPI clock, backlight, touch bus, touch axes.
 * Saved to NVS; every sketch using the library then picks them up.  See
 * README-overrides.md.
 *   Navigation: the band around the screen edge is for navigation.  Next is
 *   drawn lower right, but every corner answers; Back is drawn bottom center,
 *   but the top center and the middles of both sides answer too -- so a touch
 *   with mirrored or swapped axes still means the same thing.  Every touch is
 *   drawn on these screens, so it shows where presses land.
 *   Recovery: if a saved override leaves the screen unusable, press RST, then
 *   at once press and hold BOOT for a second or two.  This sketch then starts
 *   with every override ignored and opens the settings screen.  (Holding
 *   BOOT *while* pressing RST instead enters the ESP32's bootloader.)
 *   The settings screens work with touch or with BOOT alone: a short press
 *   moves to the next item, a long press selects it.
 *
 * The panel runs at a conservative 20 MHz SPI clock here, whatever the clock
 * override says, and both possible backlight pins are lit (unless a
 * capacitive touch chip answers on I2C), so this tool stays usable.
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
static const uint16_t C_DOT   = 0xFFE0;   // yellow, as the settings screens' touch trail

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
        trailAdd(x, y);                   // settings screens: show where it landed
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
static lv_obj_t *scrDraw, *btnBoot;
static lv_indev_t *gPointer;
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

// ── touch trail ────────────────────────────────────────────────────────────
// On the settings screens every touch LVGL receives is drawn on top of
// everything else, so it is plain where a press lands relative to the buttons
// (a wrong touch axis shows at once).  Drawn from LVGL's top layer, so the
// dots survive every redraw of the screen beneath; each one goes after
// TRAIL_MS.  A ring buffer, oldest at the tail.
#define TRAIL_MAX 400
#define TRAIL_MS  3000
struct TrailDot { int16_t x, y; uint32_t t; };
static TrailDot gTrail[TRAIL_MAX];
static int  gTrailN = 0, gTrailHead = 0;
static bool gTrailOn = false;
static const uint32_t C_TRAIL = 0xFFFF00;   // yellow; C_DOT is the same in RGB565
static void trailInvalidate(int x, int y) {
    lv_area_t a = {x - 2, y - 2, x + 2, y + 2};
    lv_obj_invalidate_area(lv_layer_top(), &a);
}
static int trailTail() { return (gTrailHead + TRAIL_MAX - gTrailN) % TRAIL_MAX; }
static void trailDropOldest() {
    const TrailDot &o = gTrail[trailTail()];
    trailInvalidate(o.x, o.y);
    gTrailN--;
}
static void trailAdd(int x, int y) {
    if (!gTrailOn) return;
    if (gTrailN) {
        const TrailDot &l = gTrail[(gTrailHead + TRAIL_MAX - 1) % TRAIL_MAX];
        if (abs(l.x - x) < 2 && abs(l.y - y) < 2) return;
    }
    if (gTrailN == TRAIL_MAX) trailDropOldest();
    gTrail[gTrailHead] = {(int16_t)x, (int16_t)y, millis()};
    gTrailHead = (gTrailHead + 1) % TRAIL_MAX;
    gTrailN++;
    trailInvalidate(x, y);
}
// Drop the dots older than TRAIL_MS.
static void trailExpire() {
    uint32_t now = millis();
    while (gTrailN && now - gTrail[trailTail()].t > TRAIL_MS) trailDropOldest();
}
static void evTrailDraw(lv_event_t *e) {
    lv_layer_t *layer = lv_event_get_layer(e);
    const lv_area_t &c = layer->_clip_area;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(C_TRAIL);
    d.bg_opa = LV_OPA_COVER;
    for (int k = 0, i = trailTail(); k < gTrailN; k++, i = (i + 1) % TRAIL_MAX) {
        lv_area_t a = {gTrail[i].x - 2, gTrail[i].y - 2, gTrail[i].x + 2, gTrail[i].y + 2};
        if (a.x2 < c.x1 || a.x1 > c.x2 || a.y2 < c.y1 || a.y1 > c.y2) continue;  // not in this strip
        lv_draw_rect(layer, &d, &a);
    }
}

// ── navigation zones ───────────────────────────────────────────────────────
// The band around the screen edge (BAND deep) is for navigation only.  All
// four of its corners mean NEXT (the button is drawn lower right) and the
// middles of all four sides mean BACK (drawn bottom center).  A touch whose
// axes are mirrored or swapped still lands in a zone that means the same
// thing.  Page content stays inside the band.
#define Z_NONE 0
#define Z_NEXT 1
#define Z_BACK 2
static const float BAND = 0.15f, MID0 = 0.30f, MID1 = 0.70f;
static int zoneAt(int x, int y) {
    float u = (float)x / SCRW, v = (float)y / SCRH;
    bool edgeU = u < BAND || u > 1 - BAND, edgeV = v < BAND || v > 1 - BAND;
    if (!edgeU && !edgeV) return Z_NONE;
    bool midU = u >= MID0 && u <= MID1, midV = v >= MID0 && v <= MID1;
    return (edgeV && midU) || (edgeU && midV) ? Z_BACK : Z_NEXT;
}
// A button in the bottom band, from u0 to u1 of the width.
static lv_obj_t *bandButton(lv_obj_t *scr, const char *txt, float u0, float u1) {
    int x0 = (int)(SCRW * u0) + 3, x1 = (int)(SCRW * u1) - 3;
    int y0 = (int)(SCRH * (1 - BAND)) + 3, y1 = SCRH - 3;
    lv_obj_t *b = lv_button_create(scr);
    lv_obj_set_pos(b, x0, y0);
    lv_obj_set_size(b, x1 - x0, y1 - y0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    return b;
}

// ── settings: options and advanced overrides ───────────────────────────────
// BOOT as a two-key keypad for the settings screens: a short press is NEXT
// (move focus), a long press ENTER (activate), so they work without touch.
static uint32_t gBootDown = 0;
static bool     gBootLong = false;
static uint8_t  gKeyPhase = 0;          // 2 = send press, 1 = send release
static uint32_t gKey = LV_KEY_NEXT;
static void bootKeypad_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    bool down = digitalRead(CAL_BOOT_PIN) == LOW;
    if (down && !gBootDown) { gBootDown = millis(); gBootLong = false; }
    if (down && !gBootLong && millis() - gBootDown > 600) {
        gBootLong = true; gKey = LV_KEY_ENTER; gKeyPhase = 2;
    }
    if (!down && gBootDown) {
        if (!gBootLong) { gKey = LV_KEY_NEXT; gKeyPhase = 2; }
        gBootDown = 0;
    }
    data->key = gKey;
    if (gKeyPhase == 2)      { data->state = LV_INDEV_STATE_PRESSED;  gKeyPhase = 1; }
    else if (gKeyPhase == 1) { data->state = LV_INDEV_STATE_RELEASED; gKeyPhase = 0; }
    else                     data->state = LV_INDEV_STATE_RELEASED;
}

struct OvrField {
    const char *name;
    size_t      off;          // offset in cydr_overrides_t
    const char *const *opt;   // names of values 1..nOpt
    uint8_t     nOpt;
    bool        live;         // previewed at once; the rest need a restart
    bool        adv;          // Advanced: a wrong value can break the screen or touch
    const char *help;
};
// Model names in cydr_model_t order, from 1.  The model names the board
// outright, for one detection gets wrong or can't tell from another; its
// wiring, LED pins, touch chip, touch directions and size then follow, and
// the other fields can still adjust them.  That makes it an Advanced field.
static const char *const kModels[] = {
    "ESP32-2432S028", "ESP32-32E 2.8\"", "ESP32-32E 3.2\"", "ESP32-32E 3.5/4.0\"",
    "ESP32-024 2.4\"", "ESP32-035 3.5\"", "ESP32-2432S032", "ESP32-2432S032C",
    "ESP32-2432S024C", "JC2432W328 2.8\"", "ESP32-3248S035C",
};
static_assert(sizeof kModels / sizeof kModels[0] == CYDR_MODEL_COUNT - 1,
              "kModels must list every cydr_model_t");
static const char *const kSizes[] = {"2.4\"", "2.8\"", "3.2\"", "3.5\"", "4.0\""};
static const char *const kPanels[] = {"ILI9341", "ST7789", "ST7796"};
static const char *const kRgbBgr[] = {"RGB", "BGR"};
static const char *const kOffOn[] = {"off", "on"};
static const char *const kClearSet[] = {"clear", "set"};
static const char *const kClocks[] = {"27 MHz", "20 MHz"};
static const char *const kBacklights[] = {"GPIO21", "GPIO27"};
static const char *const kTouchBus[] = {"own pins", "display bus"};
static const char *const kRedLeds[] = {"GPIO4", "GPIO22"};
static const char *const kGreenBlue[] = {"16 / 17", "17 / 16"};
static const char *const kGpio34[] = {"light sensor", "battery"};
// "absent / hidden" also covers a key the enclosure makes unreachable: the
// board then reports no power key.
static const char *const kPowerKeys[] = {"absent / hidden", "press twice", "hold 10 s",
                                         "press twice or hold"};
// Options (adv false) change what the library reports, or an LED pin: none
// can break the screen or touch.  Advanced fields can.
static const OvrField kFields[] = {
    {"Screen size",          offsetof(cydr_overrides_t, size),         kSizes,      5, false, false,
     "Names the screen's size. The 32E 3.5\" and 4.0\" are built alike, so detection "
     "can't tell them apart. Changes no pins and no drawing."},
    {"Red LED",              offsetof(cydr_overrides_t, redLed),       kRedLeds,    2, false, false,
     "The pin that drives the red LED. Where it is GPIO22, GPIO4 switches the "
     "speaker amplifier instead."},
    {"Green / blue LED",     offsetof(cydr_overrides_t, greenBlue),    kGreenBlue,  2, false, false,
     "Which pins are green and which blue. Change this if the two colors come "
     "out swapped."},
    {"GPIO34",               offsetof(cydr_overrides_t, gpio34),       kGpio34,     2, false, false,
     "What GPIO34 reads: a light sensor, or the battery voltage (cell / 2)."},
    {"Power key (SW1) off",  offsetof(cydr_overrides_t, powerKey),     kPowerKeys,  4, false, false,
     "How SW1 turns the board off on battery, as told to sketches. Choose "
     "\"absent / hidden\" if an enclosure covers it."},
    {"Model",                offsetof(cydr_overrides_t, model),        kModels,     CYDR_MODEL_COUNT - 1, false, true,
     "Names the board when detection gets it wrong. Also sets the wiring, "
     "backlight, touch chip and touch directions."},
    {"Panel",                offsetof(cydr_overrides_t, panel),        kPanels,     3, false, true,
     "The display controller. Only offered when the panel didn't answer with its ID."},
    {"Color order",          offsetof(cydr_overrides_t, colorOrder),   kRgbBgr,     2, true,  true,
     "The Back and Next buttons should be blue. Red and blue swapped? Change this."},
    {"Inversion",            offsetof(cydr_overrides_t, inversion),    kOffOn,      2, true,  true,
     "Colors look like a photo negative? Change this."},
    {"Mirror X (MX)",        offsetof(cydr_overrides_t, mirrorX),      kClearSet,   2, true,  true,
     "Picture mirrored left to right? Change this."},
    {"Mirror Y (MY)",        offsetof(cydr_overrides_t, mirrorY),      kClearSet,   2, true,  true,
     "Hold the board with its USB connector at the bottom: text should read upright. Picture mirrored top to bottom? Change this."},
    {"SPI clock",            offsetof(cydr_overrides_t, spiClock),     kClocks,     2, false, true,
     "Lower it if the picture has speckles or smeared pixels. This tool always "
     "runs at 20 MHz."},
    {"Backlight",            offsetof(cydr_overrides_t, backlight),    kBacklights, 2, false, true,
     "The pin that lights the screen."},
    {"Touch bus",            offsetof(cydr_overrides_t, touchBus),     kTouchBus,   2, false, true,
     "Which pins the XPT2046 resistive touch chip is on."},
    {"Touch mirror X",       offsetof(cydr_overrides_t, touchMirrorX), kOffOn,      2, true,  true,
     "Touches land mirrored left to right? Change this."},
    {"Touch mirror Y",       offsetof(cydr_overrides_t, touchMirrorY), kOffOn,      2, true,  true,
     "Touches land mirrored top to bottom? Change this."},
    {"Touch swap X/Y",       offsetof(cydr_overrides_t, touchSwapXY),  kOffOn,      2, true,  true,
     "Moving across makes the touch go up and down? Change this."},
};
static const int kNFields = sizeof kFields / sizeof kFields[0];
static cydr_overrides_t gEdit, gAuto;

static uint8_t &fieldRef(cydr_overrides_t &o, int i) { return ((uint8_t *)&o)[kFields[i].off]; }
static bool isFixedPanel(int i) {
    return kFields[i].off == offsetof(cydr_overrides_t, panel) && !display.panelGuessed();
}
static const char *autoName(int i) {
    const OvrField &f = kFields[i];
    uint8_t a = fieldRef(gAuto, i);
    return a ? f.opt[a - 1] :
           f.off == offsetof(cydr_overrides_t, spiClock) ? "40 MHz" :
           f.off == offsetof(cydr_overrides_t, model) ? "unknown" :
           f.off == offsetof(cydr_overrides_t, powerKey) ? "unknown" :
           f.off == offsetof(cydr_overrides_t, size) ? "not known" : "?";
}
static String valueText(int i) {
    uint8_t v = fieldRef(gEdit, i);
    if (isFixedPanel(i)) return String(display.panelName()) + " (ID read)";
    if (!v) return String("auto (") + autoName(i) + ")";
    return String(kFields[i].opt[v - 1]) + "  [forced]";
}

// Show the edited display / touch settings on the hardware now.
static void applyLive() {
    auto eff = [](size_t off) {
        uint8_t v = ((uint8_t *)&gEdit)[off];
        return v ? v : ((uint8_t *)&gAuto)[off];
    };
    display.setColorOrder(eff(offsetof(cydr_overrides_t, colorOrder)) == 2);
    display.setInversion(eff(offsetof(cydr_overrides_t, inversion)) == 2);
    display.setMirror(eff(offsetof(cydr_overrides_t, mirrorX)) == 2,
                      eff(offsetof(cydr_overrides_t, mirrorY)) == 2);
    touch.setAxes(eff(offsetof(cydr_overrides_t, touchMirrorX)) == 2,
                  eff(offsetof(cydr_overrides_t, touchMirrorY)) == 2,
                  eff(offsetof(cydr_overrides_t, touchSwapXY)) == 2);
    lv_obj_invalidate(lv_screen_active());
}

// ── pages ──────────────────────────────────────────────────────────────────
// Every settings screen is a page: a title in the top band, a scrollable body
// inside the band, a Back button bottom center and, where there is one, a
// Next button lower right.  waitPage() returns NAV_BACK, NAV_NEXT, or the
// number of the body row that was tapped.
#define NAV_NEXT -2
#define NAV_BACK -3
static lv_group_t *gGroup;
static volatile int gNav = 0;           // 1 next, 2 back
static volatile int gPick = -1;         // body row tapped
static void evNav(lv_event_t *e)  { gNav = (int)(intptr_t)lv_event_get_user_data(e); }
static void evPick(lv_event_t *e) { gPick = (int)(intptr_t)lv_event_get_user_data(e); }
// A press on the bare screen: act on the zone it fell in.  The screen's user
// data says which of Next (bit 0) and Back (bit 1) the page has.
static void evScreenClick(lv_event_t *e) {
    lv_obj_t *scr = (lv_obj_t *)lv_event_get_current_target(e);
    if ((lv_obj_t *)lv_event_get_target(e) != scr) return;
    lv_indev_t *in = lv_indev_active();
    if (!in || lv_indev_get_type(in) != LV_INDEV_TYPE_POINTER) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    uintptr_t has = (uintptr_t)lv_obj_get_user_data(scr);
    int z = zoneAt(p.x, p.y);
    if (z == Z_NEXT && (has & 1)) gNav = 1;
    if (z == Z_BACK && (has & 2)) gNav = 2;
}

// nextTxt nullptr: the page has no Next.
static lv_obj_t *makePage(const char *title, const char *backTxt, const char *nextTxt,
                          lv_obj_t **body) {
    lv_group_remove_all_objs(gGroup);
    lv_obj_t *scr = makeScreen(0x101418);
    lv_obj_set_user_data(scr, (void *)(uintptr_t)((nextTxt ? 1 : 0) | 2));
    lv_obj_add_event_cb(scr, evScreenClick, LV_EVENT_CLICKED, NULL);

    int bw = (int)(SCRW * BAND), bh = (int)(SCRH * BAND);
    lv_obj_t *t = makeLabel(scr, title, SCRW - 8);
    lv_obj_set_style_text_color(t, lv_color_hex(0x80D0FF), 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, bh / 2 - 9);

    lv_obj_t *b = lv_obj_create(scr);
    lv_obj_set_pos(b, bw, bh);
    lv_obj_set_size(b, SCRW - 2 * bw, SCRH - 2 * bh);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_pad_row(b, 4, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    *body = b;

    lv_obj_t *back = bandButton(scr, backTxt, MID0, MID1);
    lv_obj_add_event_cb(back, evNav, LV_EVENT_CLICKED, (void *)(intptr_t)2);
    if (nextTxt) {
        lv_obj_t *next = bandButton(scr, nextTxt, MID1, 1.0f);
        lv_obj_add_event_cb(next, evNav, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    }
    return scr;
}
// Text in a page body.
static lv_obj_t *bodyText(lv_obj_t *body, const char *txt, uint32_t color) {
    lv_obj_t *l = lv_label_create(body);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, txt);
    return l;
}
// A tappable row in a page body; waitPage() returns pick when it is tapped.
static lv_obj_t *bodyRow(lv_obj_t *body, const char *txt, int pick) {
    lv_obj_t *b = lv_button_create(body);
    lv_obj_set_width(b, LV_PCT(100));
    lv_obj_set_style_pad_ver(b, 6, 0);
    lv_obj_set_style_pad_hor(b, 6, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_label_set_text(l, txt);
    lv_obj_add_event_cb(b, evPick, LV_EVENT_CLICKED, (void *)(intptr_t)pick);
    lv_group_add_obj(gGroup, b);
    return b;
}
// Load a page (deleting the one before) with a clean touch trail.
static void showPage(lv_obj_t *scr) {
    // The nav buttons go after the body rows in BOOT's focus order.
    uint32_t n = lv_obj_get_child_count(scr);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(scr, i);
        if (lv_obj_check_type(c, &lv_button_class)) lv_group_add_obj(gGroup, c);
    }
    gTrailN = gTrailHead = 0;
    lv_screen_load_anim(scr, LV_SCREEN_LOAD_ANIM_NONE, 0, 0, true);
}
static void (*gPageTick)() = nullptr;   // called while a page waits, if set
static int waitPage() {
    gNav = 0; gPick = -1;
    while (!gNav && gPick < 0) {
        trailExpire();
        if (gPageTick) gPageTick();
        lv_timer_handler();
        delay(5);
    }
    return gPick >= 0 ? gPick : gNav == 1 ? NAV_NEXT : NAV_BACK;
}

// The backlight page holds the selected pin on and flashes the other, so a
// flashing screen says the selection is wrong.  GPIO21 can be a capacitive
// touch chip's INT output (the 2432S024C's CST820), so where one answered it
// is only pulled up or down, never driven.
static bool gCapChip = false;           // a capacitive touch chip answered on I2C
static int  gBlSel = 21, gBlPhase = -1;
static void blDrive(int pin, bool on) {
    if (gCapChip && pin == 21) pinMode(pin, on ? INPUT_PULLUP : INPUT_PULLDOWN);
    else { pinMode(pin, OUTPUT); digitalWrite(pin, on ? HIGH : LOW); }
}
static void blTick() {
    int ph = (millis() / 400) & 1;
    if (ph == gBlPhase) return;
    gBlPhase = ph;
    blDrive(gBlSel, true);
    blDrive(gBlSel == 21 ? 27 : 21, ph);
}
// Back to how setup() left the pins: the backlight on, and the other pin on
// too unless a capacitive chip answered (then left as an input).
static void blRestore() {
    int bl = display.backlightPin(), other = bl == 21 ? 27 : 21;
    blDrive(bl, true);
    if (gCapChip) pinMode(other, INPUT);
    else          blDrive(other, true);
}

// One field's own page: its choices as a list.  Returns NAV_BACK or NAV_NEXT.
static int runField(int i, bool hasNext) {
    const OvrField &f = kFields[i];
    lv_obj_t *body;
    lv_obj_t *scr = makePage(f.name, LV_SYMBOL_LEFT " Back", hasNext ? "Next " LV_SYMBOL_RIGHT : nullptr, &body);
    bodyText(body, f.help, 0xB0B8C0);
    lv_obj_t *rows[CYDR_MODEL_COUNT];   // auto + the most options any field has
    int nRows = 0;
    if (isFixedPanel(i)) {
        String s = String("This panel answered with its ID (") + display.panelName() +
                   "), so it is never overridden.";
        bodyText(body, s.c_str(), 0xFFFFFF);
    } else {
        for (int k = 0; k <= f.nOpt; k++) rows[nRows++] = bodyRow(body, "", k);
        bodyText(body, f.live ? "Shown at once." : "Takes effect after Save & restart.", 0xB0B8C0);
    }
    // GPIO21 is the touch chip's INT line where one is known to use it (the
    // 2432S024C's CST820): it can't be the backlight there.
    bool blPage = f.off == offsetof(cydr_overrides_t, backlight);
    bool int21 = blPage && touch.capacitive() && display.model() == CYDR_MODEL_2432S024C;
    int offRow = int21 ? 1 : -1;            // row 1 is GPIO21
    if (int21) {
        lv_obj_add_state(rows[offRow], LV_STATE_DISABLED);
        lv_group_remove_obj(rows[offRow]);
        String s = String("GPIO21 is the ") + touch.chipName() + " touch chip's interrupt "
                   "line on this board: a conflict with touch, so GPIO21 cannot be selected.";
        bodyText(body, s.c_str(), 0xFFD040);
    }
    auto refresh = [&]() {
        uint8_t v = fieldRef(gEdit, i);
        for (int k = 0; k < nRows; k++) {
            if (k == offRow) {
                String s = String(k == v ? LV_SYMBOL_OK " " : "") + f.opt[k - 1];
                lv_label_set_text(lv_obj_get_child(rows[k], 0), s.c_str());
                lv_obj_set_style_bg_color(rows[k], lv_color_hex(0x1A1E22), 0);
                lv_obj_set_style_text_color(rows[k], lv_color_hex(0x606870), 0);
                continue;
            }
            String s = k == v ? LV_SYMBOL_OK " " : "";
            if (k) s += f.opt[k - 1];
            else   s += String("Auto: ") + autoName(i);
            lv_label_set_text(lv_obj_get_child(rows[k], 0), s.c_str());
            lv_obj_set_style_bg_color(rows[k], lv_color_hex(k == v ? 0x2E7D32 : 0x2A3440), 0);
        }
    };
    auto blSelect = [&]() {
        uint8_t v = fieldRef(gEdit, i);
        if (!v) v = fieldRef(gAuto, i);
        gBlSel = v == 2 ? 27 : 21;
        gBlPhase = -1;
    };
    if (int21) blPage = false;              // GPIO27 is the only choice: nothing to flash
    if (blPage) {
        bodyText(body, "The pin NOT selected is flashing now. If the screen is "
                       "flashing, the selection is wrong.", 0xFFD040);
        blSelect();
        gPageTick = blTick;
    }
    refresh();
    showPage(scr);
    while (true) {
        int r = waitPage();
        if (r < 0) {
            if (blPage) { gPageTick = nullptr; blRestore(); }
            return r;
        }
        fieldRef(gEdit, i) = (uint8_t)r;
        refresh();
        if (f.live) applyLive();
        if (blPage) blSelect();
    }
}
// The next field in the same list after i, or -1.
static int nextField(int i, bool adv) {
    for (int k = i + 1; k < kNFields; k++) if (kFields[k].adv == adv) return k;
    return -1;
}
// The Options or Advanced list.  Returns on Back.
static void runList(bool adv) {
    while (true) {
        lv_obj_t *body;
        lv_obj_t *scr = makePage(adv ? "ADVANCED" : "OPTIONS", LV_SYMBOL_LEFT " Back", nullptr, &body);
        for (int i = 0; i < kNFields; i++) {
            if (kFields[i].adv != adv) continue;
            String s = String(kFields[i].name) + "\n  " + valueText(i);
            bodyRow(body, s.c_str(), i);
        }
        showPage(scr);
        int i = waitPage();
        if (i < 0) return;
        while (true) {
            int nxt = nextField(i, adv);
            if (runField(i, nxt >= 0) != NAV_NEXT || nxt < 0) break;
            i = nxt;
        }
    }
}
// Never returns: the screens end in a restart.
static void runOverrides(bool safeStart) {
    lv_indev_t *kp = lv_indev_create();
    lv_indev_set_type(kp, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(kp, bootKeypad_cb);
    gGroup = lv_group_create();
    lv_indev_set_group(kp, gGroup);
    // Open at once, but let the press that got here go unheard: LVGL
    // ignores the touch until it lifts (the on-screen BOOT key sits on
    // Cancel), and a held BOOT counts as a long press already reported.
    lv_indev_wait_release(gPointer);
    if (digitalRead(CAL_BOOT_PIN) == LOW) { gBootDown = millis(); gBootLong = true; }

    cydr_loadOverrides(gEdit);
    display.autoValues(gAuto);
    touch.autoValues(gAuto);
    if (!safeStart) applyLive();
    lv_obj_add_event_cb(lv_layer_top(), evTrailDraw, LV_EVENT_DRAW_POST, NULL);
    gTrailOn = true;

    bool warned = false;
    while (true) {
        lv_obj_t *body;
        lv_obj_t *scr = makePage("SETTINGS", "Cancel", nullptr, &body);
        String info = String("Board: ") + display.modelName() +
                      "\nPanel: " + display.panelName() + (display.panelGuessed() ? " (guessed)" : "") +
                      "\nTouch: " + touch.chipName() +
                      "\n" + SCRW + " x " + SCRH;
        bodyText(body, info.c_str(), 0xFFFFFF);
        if (safeStart)
            bodyText(body, "Recovery start: saved overrides are NOT in effect now.", 0xFFD040);
        bodyRow(body, "Options", 0);
        bodyRow(body, "Advanced", 1);
        bodyRow(body, "Save & restart", 2);
        bodyRow(body, "Clear all & restart", 3);
        bodyText(body, "Cancel restarts without saving. BOOT: short press = next, "
                       "long press = select.", 0xB0B8C0);
        showPage(scr);
        int r = waitPage();
        if (r == NAV_BACK) { Serial.println("Overrides unchanged."); break; }
        if (r == 0) runList(false);
        if (r == 1) {
            if (!warned) {
                scr = makePage("ADVANCED", LV_SYMBOL_LEFT " Back", "Next " LV_SYMBOL_RIGHT, &body);
                lv_obj_set_style_bg_color(scr, lv_color_hex(0x300000), 0);
                bodyText(body,
                    "These replace what the library detects, for a board variant it gets "
                    "wrong. Saved settings apply to every sketch on this board.\n\n"
                    "A wrong one can leave the screen blank or garbled, or touch dead. "
                    "To recover: press RST, then at once hold BOOT for a second or two.",
                    0xFFFFFF);
                showPage(scr);
                warned = waitPage() == NAV_NEXT;
            }
            if (warned) runList(true);
        }
        if (r == 2) { cydr_saveOverrides(gEdit); Serial.println("Overrides saved."); break; }
        if (r == 3) { cydr_clearOverrides();     Serial.println("Overrides cleared."); break; }
    }
    delay(100);
    ESP.restart();
}

// Recovery start: BOOT pressed within 1.5 s of reset and held for 1 s.
static bool bootHeldAtStart() {
    pinMode(CAL_BOOT_PIN, INPUT_PULLUP);
    Serial.println("TouchCalibrate: hold BOOT now to start with overrides ignored.");
    uint32_t t0 = millis();
    while (millis() - t0 < 1500) {
        if (digitalRead(CAL_BOOT_PIN) == LOW) {
            uint32_t d = millis();
            while (digitalRead(CAL_BOOT_PIN) == LOW)
                if (millis() - d > 1000) return true;
        }
        delay(5);
    }
    return false;
}
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
    // ST7789 ESP32-024 mirror X, the 3.2", ILI9341 ESP32-024 and ESP32-035 both, the
    // 3.5"/4.0" Y; the ST7789 ESP32-024's readRaw() has already swapped its X and Y).  On a mirrored axis the max raw value is at
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

// Stage 3: drawing test.  LVGL renders the instructions once; from here the
// loop blits dots directly and only re-renders text on a BOOT clear.  An
// on-screen BOOT button (bottom center) stands in for the real one, for an
// enclosure that hides it; like the settings screens' Back, the top center
// and the middles of both sides answer too, so it works with wrong touch axes.
static void startDrawingTest(const char *intro) {
    String text = String(intro) + "Drawing test\n\nBOOT: clear screen\n"
                  "Hold BOOT 2 s: settings\n" +
                  (touch.capacitive() ? "" : "RESET: redo calibration\n") +
                  "\nor flash your application\nsketch to continue";
    scrDraw = makeScreen(0x000000);
    lv_obj_t *t3 = makeLabel(scrDraw, text.c_str(), 220);
    lv_obj_align(t3, LV_ALIGN_TOP_MID, 0, (int)(SCRH * BAND));
    btnBoot = bandButton(scrDraw, "BOOT", MID0, MID1);
    lv_screen_load(scrDraw);
    renderFrame();
}
// The real BOOT button, or a touch on the on-screen one.
static bool bootOrButton() {
    uint16_t x, y;
    return digitalRead(CAL_BOOT_PIN) == LOW || (touch.read(x, y) && zoneAt(x, y) == Z_BACK);
}

void setup() {
    Serial.begin(115200);
    delay(200);

    bool safeStart = bootHeldAtStart();
    if (safeStart) Serial.println("Recovery start: overrides ignored.");
    display.useOverrides(!safeStart);
    touch.useOverrides(!safeStart);
    display.setSpiClock(20000000);    // conservative, whatever the clock override says
    display.begin(0);                 // portrait
    SCRW = display.width();
    SCRH = display.height();
    Serial.printf("%s, panel %s, %dx%d\n", display.modelName(), display.panelName(), SCRW, SCRH);
    // Any capacitive chip on SDA 33 / SCL 32 (GT911 or CST820), on any board,
    // whatever a model override says.  Before touch.begin(): the probe resets
    // GPIO32 / 33, which touch then sets up.
    bool capChip = cydr_probeI2cTouch() != CYDR_TOUCH_XPT2046;
    gCapChip = capChip;
    touch.begin(display);             // after the display: shared-touch boards share its bus
    // Light the other candidate backlight pin too, so a wrong backlight answer
    // (detected or overridden) can't leave this tool dark.  Safe on every known
    // board: that pin floats, has a pull-up to a header, or is the 32E 2.8"'s SPI
    // header CS.  Not where a capacitive chip answers: GPIO21 is the CST820's
    // INT output on the 2432S024C, and could be on others.
    if (!capChip) {
        int other = display.backlightPin() == 21 ? 27 : 21;
        pinMode(other, OUTPUT);
        digitalWrite(other, HIGH);
    }
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
    lv_indev_t *indev = gPointer = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, indev_cb);
    lv_tick_set_cb(tick_cb);

    if (safeStart) runOverrides(true);

    if (touch.capacitive()) {
        Serial.printf("%s capacitive touch: no calibration needed.\n", touch.chipName());
        startDrawingTest("Capacitive touch:\nno calibration needed\n\n");
        return;
    }

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

    startDrawingTest("");
}

void loop() {
    // Drawing test: blit a dot wherever a (filtered) touch lands.
    // Not in the on-screen BOOT button's zones.
    uint16_t x, y;
    bool down = touch.read(x, y);
    if (down && zoneAt(x, y) != Z_BACK) {
        fillRect((int)x - 2, (int)y - 2, (int)x + 2, (int)y + 2, C_DOT);
        down = false;
    }
    // BOOT (real or on-screen) clears the canvas by re-rendering the LVGL
    // instruction frame; held for 2 s it opens the settings screens.  Up
    // means 150 ms with neither pressed, riding out touch dropouts.  The
    // on-screen key shows pressed meanwhile (the clear happens at once).
    if (down || digitalRead(CAL_BOOT_PIN) == LOW) {
        lv_obj_add_state(btnBoot, LV_STATE_PRESSED);
        renderFrame();
        lv_obj_remove_state(btnBoot, LV_STATE_PRESSED);
        uint32_t t = millis(), last = millis();
        while (millis() - last < 150) {
            if (bootOrButton()) last = millis();
            if (millis() - t > 2000) runOverrides(false);
            delay(10);
        }
        renderFrame();
    }
}
