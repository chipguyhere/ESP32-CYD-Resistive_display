/*******************************************************************************
 * Resistive-touch CYD -- RasterSpeedometer: the old way and the raster way
 *
 *   1. The old way: a speedometer drawn the way a typical TFT_eSPI sketch
 *      draws one -- its 5 x 7 font (copied from TFT_eSPI, which isn't
 *      needed), one-pixel lines, and a needle and number that are erased
 *      and drawn again in front of you.  The painting is slowed down so you
 *      can see it happen.  It struggles up to about 60 mph.
 *   2. The raster way: a chrome-and-glass speedometer over a warp-speed
 *      starfield.  The needle moves smoothly, with a shadow and a glow; the
 *      LED arc and the sector behind the needle light up, with a reflection
 *      sliding along; numbers light up and swell as the speed reaches them;
 *      a glint crosses the glass at each gear change.  It runs through six
 *      gears past 200 mph and jumps to warp.
 * Then it fades out and starts over.
 *
 * Touch the screen to drive the raster speedometer yourself: left to right
 * across the screen is 0 to 225 mph (a margin each side, so neither end
 * needs the very edge).  Everything follows the speed -- the lights, the
 * stars, the red-zone pulse, the needle pinned and buzzing past 200, the
 * warp flash near the top -- and it stays where you leave it.  Press BOOT
 * and the run carries on from the moment it would have reached that speed.
 *
 * Both are drawn the same way underneath -- this sketch has no frame
 * buffer and no LVGL.  It uses the raster technique of the RasterBouncingBalls
 * and RasterFireRainSnow examples: each 16-row chunk of the panel is drawn
 * into a small DMA "bounce buffer" just before it is sent, timed against the
 * panel's own scan (command 0x45), so the picture never tears.  Both cores
 * draw, each taking the next chunk.  About 30 frames a second is the target;
 * features would be simplified before the frame rate is given up.
 *
 * The picture is landscape (320 x 240 on 2.4" to 3.2" panels, 480 x 320 on
 * 3.5" and 4.0" ones) but the panel is driven in its native portrait order.
 * Hold the board with its portrait right edge up (the library's rotation 1);
 * FLIP_LANDSCAPE below turns it the other way.
 *
 * Run RasterBouncingBalls (or TouchCalibrate's speed test) on the board
 * first: this sketch uses what it saves -- whether the screen passed the
 * 80 MHz speed test, and a slower refresh rate if you chose one ("Whole
 * screen tear-free too").  A board with no saved speed test shows a notice
 * and runs at the normal 40 MHz.
 *
 * The dial, hub, digits and the map of what the speed lights are
 * pre-rendered by tools/gen_assets.py (Python 3 with numpy and Pillow) into
 * assets.h.  tools/host_preview.cpp builds the drawing code on a computer and
 * writes frames as images.  How to design for this kind of rendering is in
 * the library's README-raster-graphics.md.
 *
 * Run TouchCalibrate once if touches land in the wrong place; this sketch
 * loads its calibration.
 *
 * Serial (115200): a status line every second (frame rate, late frames, and
 * where the time goes).  Type 'n' to skip to the other speedometer.
 *
 * Arduino IDE board settings: as the other examples ("ESP32 Dev Module" for
 * the classic CYDs; the ES3C28P's ESP32-S3 settings for that board).  The
 * pictures for both panel sizes fit the default partition scheme.
 *
 * Fonts: TFT_eSPI's font 1 is the Adafruit_GFX 5 x 7 font (BSD licence, see
 * tft_espi_glcdfont.h).  The raster digits are Source Code Pro (SIL Open
 * Font License 1.1).
 *
 * Copyright (c) 2026 chipguyhere
 * MIT License
 ******************************************************************************/

#include <chipguy_CYD_resistive_display.h>
#include <chipguy_CYD_resistive_touch.h>
#include <esp_timer.h>
#include "scan.h"
#include "raster.h"
#include "scene.h"

chipguy_CYD_resistive_display display;
chipguy_CYD_resistive_display *g_display = &display;   // for scan.cpp and raster.cpp
chipguy_CYD_resistive_touch   touch;

#define FLIP_LANDSCAPE  false   // true: view it turned clockwise instead
#define TOUCH_MARGIN    0.08f   // of the width, each side: 0 and full speed without reaching the very edge
#if CONFIG_IDF_TARGET_ESP32C5
#define BOOT_PIN        28      // NM-CYD-C5
#else
#define BOOT_PIN        0
#endif
#define MAX_FPS         45      // the frame rate adapts below this to the render load
#ifndef FORCE_NOTICE
#define FORCE_NOTICE 0          // build with -DFORCE_NOTICE=1 to see the no-speed-test notice
#endif
#define KEEP_FREE       12000   // heap left after the bounce buffers

#define RASTER_PRIORITY (configMAX_PRIORITIES - 4)
#define RENDER_PRIORITY (configMAX_PRIORITIES - 5)

static RasterPlan g_plan;
static ScanModel  g_model;
static int        g_blPin = -1;

/* ================================================================ backlight */

static void backlight(float b) {
    static int last = -1;
    int duty = (int)(powf(b < 0 ? 0 : b > 1 ? 1 : b, 2.2f) * 4095 + 0.5f);
    if (duty == last || g_blPin < 0) return;
    last = duty;
    analogWrite(g_blPin, duty);
}

/* ================================================================ render tasks */

// Two workers, one per core: they share the scene's step each frame, then
// both draw, each taking the next undrawn chunk into whichever bounce buffer
// is free.  The raster task puts the chunks back in order.

static volatile uint32_t g_renderMaxUs = 0;
static SemaphoreHandle_t g_go, g_done;
static volatile uint32_t g_workFrame = 0;
static volatile uint32_t g_helperBusy = 0;

static void report_abandoned(uint32_t frame) {
    RasterChunk rc = {RASTER_NO_BUF, RASTER_ABANDONED, frame};
    xQueueSend(g_readyQ, &rc, portMAX_DELAY);
}

static inline bool given_up(uint32_t frame) {
    return g_abortFrame == (int32_t)frame || (int32_t)(g_rasterFrame - frame) > 0;
}

static volatile int g_nextChunk = 0;

static uint32_t draw_share(uint32_t frame, int worker) {
    uint32_t busy = 0;
    const int chunks = scene_chunks();
    for (;;) {
        if (given_up(frame)) { report_abandoned(frame); break; }
        if (g_nextChunk >= chunks) break;
        uint8_t b;
        xQueueReceive(g_freeQ, &b, portMAX_DELAY);
        const int c = __atomic_fetch_add(&g_nextChunk, 1, __ATOMIC_SEQ_CST);
        if (c >= chunks || given_up(frame)) {
            xQueueSendToFront(g_freeQ, &b, 0);
            if (c < chunks) report_abandoned(frame);
            break;
        }
        int64_t t = esp_timer_get_time();
        scene_render_chunk(c, g_bounce[b], worker);
        busy += (uint32_t)(esp_timer_get_time() - t);
        RasterChunk rc = {b, (uint16_t)c, frame};
        xQueueSend(g_readyQ, &rc, portMAX_DELAY);
    }
    return busy;
}

static SemaphoreHandle_t g_stepDone, g_drawGo, g_start;

static void helper_task(void *) {
    xSemaphoreTake(g_start, portMAX_DELAY);
    for (;;) {
        xSemaphoreTake(g_go, portMAX_DELAY);
        int64_t t = esp_timer_get_time();
        scene_step_helper();
        uint32_t busy = (uint32_t)(esp_timer_get_time() - t);
        xSemaphoreGive(g_stepDone);
        xSemaphoreTake(g_drawGo, portMAX_DELAY);
        g_helperBusy = busy + draw_share(g_workFrame, 1);
        xSemaphoreGive(g_done);
    }
}

static void render_task(void *) {
    xSemaphoreTake(g_start, portMAX_DELAY);
    for (uint32_t frame = 0;; frame++) {
        uint32_t rf = g_rasterFrame;
        if ((int32_t)(rf - frame) > 0) frame = rf;
        int64_t t = esp_timer_get_time();
        static int phase = -1;
        scene_step_begin(raster_frame_us() * 1e-6f);
        if (scene_phase() != phase) { phase = scene_phase(); raster_retry(scene_min_periods()); }
        g_workFrame = frame;
        xSemaphoreGive(g_go);
        scene_step_main();
        backlight(scene_brightness());
        raster_quiet(scene_brightness() < 0.25f);
        xSemaphoreTake(g_stepDone, portMAX_DELAY);
        g_nextChunk = 0;
        xSemaphoreGive(g_drawGo);
        uint32_t busy = (uint32_t)(esp_timer_get_time() - t);
        uint32_t mine = draw_share(frame, 0);
        xSemaphoreTake(g_done, portMAX_DELAY);
        busy += mine > g_helperBusy ? mine : g_helperBusy;
        raster_report_render_us(busy);
        if (busy > g_renderMaxUs) g_renderMaxUs = busy;
    }
}

/* ================================================================ setup */

static void show_untested_notice(int panelW, int rows) {
    Serial.println("Performance data not saved to board NVS. Run stress test sketch first "
                   "(RasterBouncingBalls or TouchCalibrate). Demo in degraded mode.");
    uint16_t *row = (uint16_t *)heap_caps_malloc(panelW * 2, MALLOC_CAP_DMA);
    if (!row) return;
    for (int r = 0; r < rows; r++) {
        scene_notice_row(panelW, FLIP_LANDSCAPE, r, row);
        display.flushRegion(0, r, panelW - 1, r, row);
        display.waitFlush();
    }
    heap_caps_free(row);
    backlight(1);
    delay(5000);
    backlight(0);
}

static void halt(const char *why) {
    Serial.println(why);
    backlight(1);
    while (1) delay(1000);
}

static void plan_it() {
    g_plan.model = g_model;
    g_plan.scanKnown = g_model.valid;
    raster_plan(g_plan);
    Serial.printf("Plan: scan %d, tear-free %d, %d refresh(es) a frame minimum, transfer %.1f ms, "
                  "span %.0f us, guard %.0f us, refresh %.2f Hz\n",
                  g_plan.scanKnown, g_plan.tearFree, g_plan.periods, g_plan.transferUs / 1000,
                  g_plan.spanUs, g_plan.guardUs, g_model.hz());
}

void setup() {
#if ARDUINO_USB_CDC_ON_BOOT
    Serial.setTxTimeoutMs(0);
#else
    Serial.setTxBufferSize(4096);
#endif
    Serial.begin(115200);
    if (!display.begin(0)) halt("display.begin() failed");
    g_blPin = display.backlightPin();
    analogWriteResolution(g_blPin, 12);
    backlight(0);
    display.fillScreen(0);
    Serial.printf("\nRasterSpeedometer: %s, %s, native %dx%d\n", display.modelName(),
                  display.panelName(), display.nativeWidth(), display.nativeHeight());
    const int PW = display.nativeWidth();
    if (!scene_supports(PW, display.nativeHeight()))
        halt("This sketch draws 240 x 320 and 320 x 480 panels only.");
    const int SW = PW == 320 ? 480 : 320;          // the picture's width = panel rows used

    // 80 MHz if this board passed the speed test; no record: a notice, 40 MHz.
    cydr_speed_test_t rec;
    const bool tested = cydr_loadSpeedTest(rec);
    if (tested && rec.result == CYDR_SPEED_PASSED) display.changeSpiClock(80000000);
    if (!tested || FORCE_NOTICE) show_untested_notice(PW, SW);
    Serial.printf("SPI clock %u Hz\n", (unsigned)display.spiClock());

    // Touch, read cache-only: on boards where it shares the display's bus,
    // only the raster task samples it, between frames.
    touch.begin(display);
    touch.loadCalibration();       // saved by TouchCalibrate, if it has run
    touch.loadFilters();
    touch.setCacheOnly(true);
    pinMode(BOOT_PIN, INPUT_PULLUP);

    g_plan.W = PW;
    g_plan.H = display.nativeHeight();
    g_plan.rows = SW;
    char why[160] = "";
    bool scan = !display.rowsReversed() &&
                scan_detect(g_model, display.nativeHeight(), why, sizeof why) &&
                scan_measure(g_model, 1500);
    if (!scan) Serial.printf("No usable scan line (%s): the picture will tear.\n", why);

    // The render tasks first (they wait for g_start), then bounce buffers
    // with what heap is left.
    const int core = display.spiCore();
    const int other = portNUM_PROCESSORS > 1 ? core ^ 1 : core;
    g_go = xSemaphoreCreateBinary();
    g_done = xSemaphoreCreateBinary();
    g_stepDone = xSemaphoreCreateBinary();
    g_drawGo = xSemaphoreCreateBinary();
    g_start = xSemaphoreCreateCounting(2, 0);
    if (!g_go || !g_done || !g_stepDone || !g_drawGo || !g_start ||
        xTaskCreatePinnedToCore(render_task, "render", 6144, nullptr, RENDER_PRIORITY, nullptr, other) != pdPASS ||
        xTaskCreatePinnedToCore(helper_task, "render2", 6144, nullptr, RENDER_PRIORITY, nullptr, core) != pdPASS)
        halt("Couldn't start the render tasks (out of memory)");
    scene_init(PW, FLIP_LANDSCAPE);
    int n = raster_alloc(PW, RASTER_MAX_BUFS, KEEP_FREE);
    if (!n) halt("Bounce buffer allocation failed");
    Serial.printf("%d bounce buffers (%u bytes), heap free %u (largest block %u)\n", n,
                  (unsigned)(n * PW * CHUNK_ROWS * 2),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    double rowUs, winUs;
    raster_measure(PW, SW, rowUs, winUs);
    g_plan.rowUs = rowUs;
    g_plan.windowUs = winUs;
    Serial.printf("Transfer: %.2f us a row, window %.0f us\n", rowUs, winUs);
    plan_it();
    if (scan && !g_plan.tearFree && display.applyRefreshChoice()) {
        Serial.println("Applying the saved slower refresh");
        if (scan_measure(g_model, 1500)) plan_it();
    }
    display.fillScreen(0);

    if (g_plan.tearFree) {
        ScanModel m = g_plan.model;
        if (scan_measure(m, 500)) g_plan.model = m;
    }
    raster_start(g_plan, core, RASTER_PRIORITY, MAX_FPS);
    xSemaphoreGive(g_start);
    xSemaphoreGive(g_start);
    Serial.printf("Running: raster on core %d, render on cores %d and %d\n", core, other, core);
}

/* ================================================================ touch and BOOT */

// Touch: across the picture is 0 to full speed, left to right, with a margin
// each side.  The speed holds where the finger leaves it.
static void poll_touch(uint32_t now) {
    static float    hist[8];             // the latest readings, to undo a stray one at lift-off
    static uint32_t histMs[8];
    static int      hn = 0;
    static bool     down = false;
    uint16_t tx, ty;
    if (touch.read(tx, ty)) {
        const int W = scene_width();
        const float x = FLIP_LANDSCAPE ? W - 1 - ty : ty;   // the picture's x is the panel's row
        const float m = TOUCH_MARGIN * W;
        float v = (x - m) / (W - 1 - 2 * m) * scene_hold_max();
        v = v < 0 ? 0 : v > scene_hold_max() ? scene_hold_max() : v;
        hist[hn & 7] = v;
        histMs[hn & 7] = now;
        hn++;
        scene_hold_speed(v);
        down = true;
    } else if (down) {
        // A resistive panel's last readings, as the pressure goes, can be
        // off: settle on the newest one from 40 ms before the end.
        down = false;
        const uint32_t end = histMs[(hn - 1) & 7];
        for (int k = 1; k < 8 && k < hn; k++) {
            const int i = (hn - 1 - k) & 7;
            if (end - histMs[i] >= 40) { scene_hold_speed(hist[i]); break; }
        }
        hn = 0;
        scene_hold_lifted();
    }
}

// BOOT: carry on with the run from the held speed.
static void poll_boot(uint32_t now) {
    static bool     was = false;
    static uint32_t changed = 0;
    const bool on = digitalRead(BOOT_PIN) == LOW;
    if (on != was) {
        if (on && now - changed > 50 && scene_holding()) { scene_resume(); Serial.println("resume"); }
        changed = now;
        was = on;
    }
}

void loop() {
    static uint32_t last = millis();
    int c = Serial.available() ? Serial.read() : -1;
    if (c == 'n') { scene_next_phase(); Serial.println("next"); }
    poll_touch(millis());
    poll_boot(millis());
    if (millis() - last >= 1000) {
        uint32_t now = millis(), dt = now - last;
        last = now;
        RasterStats s;
        raster_take_stats(s);
        uint32_t busy = g_renderMaxUs;
        g_renderMaxUs = 0;
        uint32_t pr[SCENE_PROF_N];
        scene_take_profile(pr);
        const float fr = s.frames + s.late > 0 ? (float)(s.frames + s.late) : 1;
        const float mhz = ESP.getCpuFreqMHz() * 1e3f;
        auto ms = [&](int k) { return pr[k] / mhz / fr; };
        Serial.printf("%-3s %3.0f mph  fps %.1f  late %u  slips %u  refr/frame %d%s  %.2f Hz  "
                      "render max %.1f ms | cpu ms/frame: bg %.1f stars %.1f dial %.1f light %.1f "
                      "needle %.1f text %.1f step %.1f\n",
                      scene_phase_name(), scene_speed_mph(), s.frames * 1000.0f / dt, (unsigned)s.late,
                      (unsigned)s.slips, s.periods, s.relaxed ? " relaxed" : "", s.hz, busy / 1000.0f,
                      ms(SCENE_PROF_BG), ms(SCENE_PROF_STARS), ms(SCENE_PROF_DIAL), ms(SCENE_PROF_LIGHT),
                      ms(SCENE_PROF_NEEDLE), ms(SCENE_PROF_TEXT), ms(SCENE_PROF_STEP));
    }
    delay(10);
}
