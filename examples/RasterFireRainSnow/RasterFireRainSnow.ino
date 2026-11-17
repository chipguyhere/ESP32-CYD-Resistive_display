/*******************************************************************************
 * Resistive-touch CYD -- RasterFireRainSnow: three animated scenes, tear-free
 *
 *   1. Fire: a campfire with rising smoke and sparks, its light flickering on
 *      the ground, under a night sky whose stars wheel slowly toward the
 *      horizon while the moon sets.
 *   2. A thunderstorm at sunset: rain splashing on wet ground that reflects
 *      the sky, lightning, and the sun setting through the clouds as the light
 *      reddens and then fades toward night.
 *   3. Snowfall, with drifting clouds and snow-capped mountains that slowly
 *      come closer.
 * Each over a ground of perspective lines creeping toward the viewer, 10
 * seconds each, fading out and into the next (the fade is the backlight).
 *
 * The whole screen is animated at up to about 30 frames per second with no
 * frame buffer and no LVGL, built on the raster technique of the
 * RasterBouncingBalls example: each 16-row chunk of the panel is drawn into a
 * small DMA "bounce buffer" just before it is sent, and the sending is timed
 * against the panel's own scan (command 0x45), so the picture never tears.
 * Both cores draw, each taking the next chunk.  See raster.h and scene.h.
 *
 * The picture is landscape (320 x 240 on 2.4" to 3.2" panels, 480 x 320 on
 * 3.5" and 4.0" ones) but the panel is driven in its native portrait order,
 * so the scan still runs along the chunks: a chunk of panel rows is a strip of
 * landscape columns.  Hold the board with its portrait right edge up (the
 * library's rotation 1); FLIP_LANDSCAPE below turns it the other way.
 *
 * Run RasterBouncingBalls (or TouchCalibrate's speed test) on the board
 * first.  This sketch uses what they save on the board: whether the screen
 * passed the 80 MHz speed test, and a slower refresh rate if you chose one.
 * Some boards are only tear-free at a slower refresh; in RasterBouncingBalls,
 * pick the choice marked "Whole screen tear-free too".  A board with no saved
 * speed test shows a notice and runs at the normal 40 MHz.
 *
 * The backdrops -- sky, moon or sun, mountains, clouds -- are pre-rendered by
 * tools/gen_assets.py (Python 3 with numpy and Pillow) into assets.h, which
 * is in flash.  tools/host_preview.cpp builds the scene renderer on a computer
 * and writes frames as images, to try changes without a board.
 *
 * How to design for this kind of rendering, and how it's done, is in the
 * library's README-raster-graphics.md.
 *
 * Serial (115200): a status line every second (frame rate, late frames, and
 * where the time goes).  Type 'n' to skip to the next scene.
 *
 * Arduino IDE board settings:
 *   - Classic CYDs (ESP32): "ESP32 Dev Module", Partition Scheme "Huge APP
 *     (3MB No OTA/1MB SPIFFS)" -- the pictures for both panel sizes take about
 *     1.5 MB, too much for the default scheme.
 *     (esp32:esp32:esp32:PartitionScheme=huge_app)
 *   - ES3C28P (ESP32-S3): as the other examples, with Partition Scheme
 *     "16M Flash (3MB APP/9.9MB FATFS)".
 *     (esp32:esp32:esp32s3:CDCOnBoot=cdc,PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB)
 *
 * Copyright (c) 2026 chipguyhere
 * MIT License
 ******************************************************************************/

#include <chipguy_CYD_resistive_display.h>
#include <esp_timer.h>
#include "scan.h"
#include "raster.h"
#include "scene.h"

chipguy_CYD_resistive_display display;
chipguy_CYD_resistive_display *g_display = &display;   // for scan.cpp and raster.cpp

#define FLIP_LANDSCAPE  false   // true: view it turned clockwise instead
#define MAX_FPS         45      // the frame rate adapts below this to the render load
// Build with -DFORCE_NOTICE=1 to see the no-speed-test notice on a tested board.
#ifndef FORCE_NOTICE
#define FORCE_NOTICE 0
#endif
#define KEEP_FREE       12000   // heap left after the bounce buffers and sky (the raster task's stack, queues, timer)

#define RASTER_PRIORITY (configMAX_PRIORITIES - 4)
#define RENDER_PRIORITY (configMAX_PRIORITIES - 5)

static RasterPlan g_plan;
static ScanModel  g_model;
static int        g_blPin = -1;

/* ================================================================ backlight */

// 12-bit PWM with a gamma curve, so the fades are smooth down to black.
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
// is free.  The raster task puts the
// chunks back in order.  The scene isn't stepped again until both are done.

static volatile uint32_t g_renderMaxUs = 0;   // busy time in a frame, for the status line
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

// Draw this worker's share of the frame; returns the busy time.
// The next chunk of the frame nobody has taken: each worker takes the next
// one as it gets a free buffer, so neither waits on the other's expensive
// chunks.  (The raster task puts them back in order.)
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
    xSemaphoreTake(g_start, portMAX_DELAY);   // created early, started once the raster task is
    for (;;) {
        xSemaphoreTake(g_go, portMAX_DELAY);
        int64_t t = esp_timer_get_time();
        scene_step_helper();                 // its share of the step, beside the main one
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
        xSemaphoreGive(g_go);                // the helper starts its share of the step
        scene_step_main();
        backlight(scene_brightness());
        raster_quiet(scene_brightness() < 0.25f);
        xSemaphoreTake(g_stepDone, portMAX_DELAY);
        g_nextChunk = 0;
        xSemaphoreGive(g_drawGo);            // both shares done: draw
        uint32_t busy = (uint32_t)(esp_timer_get_time() - t);
        uint32_t mine = draw_share(frame, 0);
        xSemaphoreTake(g_done, portMAX_DELAY);
        busy += mine > g_helperBusy ? mine : g_helperBusy;   // the two run side by side
        raster_report_render_us(busy);
        if (busy > g_renderMaxUs) g_renderMaxUs = busy;
    }
}

/* ================================================================ setup */

// "Performance data not saved to board NVS. Run stress test sketch first.
// Demo in degraded mode." -- for a few seconds, before the demo starts.
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

// Time the scan, the transfer, and make the plan.
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
    // Serial must never hold up the raster task: on a UART (the classic
    // boards' USB-serial chip) give prints a buffer; on the S3's own USB, no
    // waiting when no program has the port open.
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
    Serial.printf("\nRasterFireRainSnow: %s, %s, native %dx%d\n", display.modelName(),
                  display.panelName(), display.nativeWidth(), display.nativeHeight());
    const int PW = display.nativeWidth();
    if (!scene_supports(PW, display.nativeHeight()))
        halt("This sketch draws 240 x 320 and 320 x 480 panels only.");
    const int SW = PW == 320 ? 480 : 320;          // the picture's width = panel rows used

    // The SPI clock: 80 MHz if this board has passed the speed test.  With no
    // record at all (never tested, or tested by an older version of the
    // test), say so on screen and go on at 40 MHz, as for a failed test.
    cydr_speed_test_t rec;
    const bool tested = cydr_loadSpeedTest(rec);
    if (tested && rec.result == CYDR_SPEED_PASSED) display.changeSpiClock(80000000);
    if (!tested || FORCE_NOTICE) show_untested_notice(PW, PW == 320 ? 480 : 320);
    Serial.printf("SPI clock %u Hz\n", (unsigned)display.spiClock());

    // The scan line and the refresh rate (before the big allocations: the
    // detection needs a few KB of its own).
    g_plan.W = PW;
    g_plan.H = display.nativeHeight();
    g_plan.rows = SW;
    char why[160] = "";
    bool scan = !display.rowsReversed() &&
                scan_detect(g_model, display.nativeHeight(), why, sizeof why) &&
                scan_measure(g_model, 1500);
    if (!scan) Serial.printf("No usable scan line (%s): the picture will tear.\n", why);

    // The render tasks first (they wait for g_start), so the bounce buffers
    // can have what heap is left.
    const int core = display.spiCore();
    // The main render worker on the other core (single-core chips: the same one).
    const int other = portNUM_PROCESSORS > 1 ? core ^ 1 : core;
    g_go = xSemaphoreCreateBinary();
    g_done = xSemaphoreCreateBinary();
    g_stepDone = xSemaphoreCreateBinary();
    g_drawGo = xSemaphoreCreateBinary();
    g_start = xSemaphoreCreateCounting(2, 0);
    // The helper shares the raster task's core, one priority below it.
    if (!g_go || !g_done || !g_stepDone || !g_drawGo || !g_start ||
        xTaskCreatePinnedToCore(render_task, "render", 6144, nullptr, RENDER_PRIORITY, nullptr, other) != pdPASS ||
        xTaskCreatePinnedToCore(helper_task, "render2", 6144, nullptr, RENDER_PRIORITY, nullptr, core) != pdPASS)
        halt("Couldn't start the render tasks (out of memory)");
    // The scene's tables, then bounce buffers (up to a whole frame), then
    // the sky in RAM if it fits.  Counted as internal byte-addressable heap:
    // the classic ESP32's internal heap also has IRAM that only takes 32-bit
    // accesses, and the S3's 8-bit heap includes its PSRAM.  (The larger
    // panels' sky, 160 KB, never fits.)
    scene_init(PW, FLIP_LANDSCAPE);
    const size_t skyBytes = scene_sky_bytes(PW);
    // The storm's sky in RAM only with room to spare: bounce buffers come
    // first (8 of them), or the classic ESP32 runs out on 240 x 320 panels.
    const size_t bufBytes = (size_t)PW * CHUNK_ROWS * 2;
    const bool trySky = skyBytes < 100000 &&
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= skyBytes + KEEP_FREE + 8 * bufBytes;
    int n = raster_alloc(PW, RASTER_MAX_BUFS, KEEP_FREE + (trySky ? skyBytes : 0));
    if (!n) halt("Bounce buffer allocation failed");
    uint16_t *skyRam = nullptr;
    if (trySky && heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= skyBytes &&
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= skyBytes + KEEP_FREE)
        skyRam = (uint16_t *)heap_caps_malloc(skyBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (skyRam) scene_use_sky_ram(skyRam);
    Serial.printf("%d bounce buffers (%u bytes), sky in %s, heap free %u (largest block %u)\n", n,
                  (unsigned)(n * PW * CHUNK_ROWS * 2), skyRam ? "RAM" : "flash",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    double rowUs, winUs;
    raster_measure(PW, SW, rowUs, winUs);
    g_plan.rowUs = rowUs;
    g_plan.windowUs = winUs;
    Serial.printf("Transfer: %.2f us a row, window %.0f us\n", rowUs, winUs);
    plan_it();
    // Too slow for the stock refresh: the user's saved slower refresh, if any.
    if (scan && !g_plan.tearFree && display.applyRefreshChoice()) {
        Serial.println("Applying the saved slower refresh");
        if (scan_measure(g_model, 1500)) plan_it();
    }
    display.fillScreen(0);


    // Re-time the scan just before starting: the fit is seconds old.
    if (g_plan.tearFree) {
        ScanModel m = g_plan.model;
        if (scan_measure(m, 500)) g_plan.model = m;
    }
    raster_start(g_plan, core, RASTER_PRIORITY, MAX_FPS);
    xSemaphoreGive(g_start);
    xSemaphoreGive(g_start);
    Serial.printf("Running: raster on core %d, render on cores %d and %d\n", core, other, core);
}

void loop() {
    static uint32_t last = millis();
    int c = Serial.available() ? Serial.read() : -1;
    if (c == 'n') { scene_next_phase(); Serial.println("next scene"); }
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
        auto ms = [&](int k) { return pr[k] / 240e3f / fr; };
        Serial.printf("%-5s fps %.1f  late %u  slips %u  refr/frame %d%s  %.2f Hz  scan err %.1f ln "
                      "rej %u max %u us  render max %.1f ms | cpu ms/frame: sky %.1f ground %.1f "
                      "over %.1f (logs %.1f puffs %.1f flames %.1f) step %.1f | skycopy %.1f cloud/sun %.1f mtn %.1f\n",
                      scene_phase_name(), s.frames * 1000.0f / dt, (unsigned)s.late,
                      (unsigned)s.slips, s.periods, s.relaxed ? " relaxed" : "", s.hz, s.scanErrRms,
                      (unsigned)s.scanRejected, (unsigned)s.spanMax, busy / 1000.0f,
                      ms(SCENE_PROF_SKY), ms(SCENE_PROF_GROUND), ms(SCENE_PROF_OVER),
                      ms(SCENE_PROF_LOGS), ms(SCENE_PROF_PUFFS), ms(SCENE_PROF_FLAMES), ms(SCENE_PROF_STEP),
                      ms(SCENE_PROF_SKYCOPY), ms(SCENE_PROF_CLOUDSUN), ms(SCENE_PROF_MOUNTAINS));
    }
    delay(10);
}
