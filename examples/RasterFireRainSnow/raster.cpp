/*
 * raster.cpp - The raster task.  See raster.h.
 *
 * Tear-free timing ("beam chasing"), as in the RasterBouncingBalls example.
 * The panel scans row r at
 *     S + (a + r) * L        (S: the moment it reaches line 0, L: one line,
 *                             a: lines of blanking before the first row)
 * and again one refresh P later.  Row r is written at about
 *     t0 + c + r * w         (t0: start, c: the window command, w: one row)
 * The whole frame changes on the same refresh -- no tear -- if every row is
 * written between its scan on refresh n and its scan on refresh n+1:
 *     0 < (t0 + c + r*w) - (S + (a + r)*L) < P     for every row r.
 * That difference changes by (w - L) per row, so it can be done when
 * |B*(w - L)| plus a guard each side fits in P; t0 is placed to centre it.
 * The frame period (a whole number of refreshes) doesn't enter into that, so
 * it is free to adapt to the render task's load.
 *
 * Copyright (c) 2026 chipguyhere
 * MIT License
 */

#include "raster.h"
#include <math.h>
#include <esp_timer.h>
#include <driver/spi_master.h>
#include <chipguy_CYD_resistive_display.h>

extern chipguy_CYD_resistive_display *g_display;

uint16_t        *g_bounce[RASTER_MAX_BUFS];
int              g_nBufs = 0;
QueueHandle_t    g_freeQ, g_readyQ;
volatile int32_t g_abortFrame = -1;
volatile uint32_t g_rasterFrame = 0;

static spi_transaction_t s_trans[RASTER_MAX_BUFS];
static int s_W = 0;
static RasterPlan s_plan;
static TaskHandle_t s_task = nullptr;
static esp_timer_handle_t s_timer = nullptr;

static volatile uint32_t s_frames, s_late, s_slips;
static volatile float    s_errSq;
static volatile uint32_t s_errN;
static portMUX_TYPE      s_statMux = portMUX_INITIALIZER_UNLOCKED;
static volatile double   s_lineUs;
static volatile int      s_periods = 1;
static int               s_minPeriods = 1;
static volatile float    s_freeFrameUs = 30000;    // unsynced: measured frame time
static volatile uint32_t s_renderUs = 0;           // max busy time since the governor looked
// Too slow for the per-chunk deadlines even at RELAX_PERIODS refreshes a
// frame: then chunks are waited for (up to RELAXED_WAIT_US each) and the
// picture may tear below the point where the render task fell behind --
// better than no picture.
#define RELAX_PERIODS   4
#define RELAXED_WAIT_US 60000
static volatile bool     s_relaxed = false;
// Chunks that arrived before their turn (two render workers), by chunk.
#define MAX_CHUNKS 32
static uint8_t s_pend[MAX_CHUNKS];
static volatile bool s_retry = false;
static volatile int  s_retryFloor = 0;     // the current scene's fewest refreshes a frame
static volatile bool s_quiet = false;      // the screen is dark: late frames don't count
static volatile uint32_t s_scanRej = 0, s_spanMax = 0;

int raster_alloc(int width, int maxBufs, size_t keepFree) {
    s_W = width;
    if (maxBufs > RASTER_MAX_BUFS) maxBufs = RASTER_MAX_BUFS;
    const size_t bytes = (size_t)width * CHUNK_ROWS * 2;
    int n = 0;
    while (n < maxBufs && heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) > keepFree + bytes) {
        g_bounce[n] = (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_DMA);
        if (!g_bounce[n]) break;
        s_trans[n] = {};
        s_trans[n].length = bytes * 8;
        s_trans[n].tx_buffer = g_bounce[n];
        s_trans[n].user = (void *)1;       // DC = data
        n++;
    }
    if (n < 3) {                                  // too few to run: give back what was taken
        while (n) heap_caps_free(g_bounce[--n]);
        return 0;
    }
    g_nBufs = n;
    g_freeQ = xQueueCreate(n, sizeof(uint8_t));
    g_readyQ = xQueueCreate(n + 2, sizeof(RasterChunk));   // + abandonments
    return g_freeQ && g_readyQ ? n : 0;
}

static inline spi_device_handle_t dev() { return g_display->spiDevice(); }

static int collect() {
    spi_transaction_t *t = nullptr;
    spi_device_get_trans_result(dev(), &t, portMAX_DELAY);
    for (int i = 0; i < g_nBufs; i++) if (t == &s_trans[i]) return i;
    return -1;
}

bool raster_measure(int width, int rows, double &rowUs, double &windowUs) {
    g_display->waitFlush();
    for (int i = 0; i < g_nBufs; i++)
        for (int k = 0; k < width * CHUNK_ROWS; k++) g_bounce[i][k] = 0;
    const int chunks = rows / CHUNK_ROWS;
    double best = 1e18, bestWin = 1e18;
    for (int rep = 0; rep < 6; rep++) {
        int64_t t0 = esp_timer_get_time();
        g_display->setWindow(0, 0, width - 1, rows - 1);
        int64_t t1 = esp_timer_get_time();
        int inflight = 0;
        for (int c = 0; c < chunks; c++) {
            spi_device_queue_trans(dev(), &s_trans[c % g_nBufs], portMAX_DELAY);
            if (++inflight == 2) { collect(); inflight--; }
        }
        while (inflight) { collect(); inflight--; }
        int64_t t2 = esp_timer_get_time();
        if (t2 - t1 < best) best = (double)(t2 - t1);
        if (t1 - t0 < bestWin) bestWin = (double)(t1 - t0);
    }
    rowUs = best / (chunks * CHUNK_ROWS);
    windowUs = bestWin;
    return true;
}

/* ----------------------------------------------------------------- the plan */

#define SCAN_READ_US 150.0
// A scan reading is timed at its middle; one that took longer than this is
// not used.  The render tasks' flash traffic stretches the reads (the read
// code runs from flash), so this is above the example's 200 us.
#define SCAN_SPAN_MAX 350

void raster_plan(RasterPlan &p) {
    p.transferUs = p.windowUs + p.rows * p.rowUs;
    p.tearFree = false;
    p.periods = 0;
    if (!p.scanKnown) return;
    const ScanModel &m = p.model;
    const double P = m.periodUs(), L = m.lineUs;
    const int blank = m.lines - p.H;
    p.guardUs = 300 + (blank / 2.0 + 1) * L + 3 * m.rmsLines * L;
    p.spanUs = fabs(p.rows * (p.rowUs - L));
    if (p.spanUs + 2 * p.guardUs > P) return;
    p.periods = (int)ceil((p.transferUs + p.windowUs + SCAN_READ_US + 500) / P);
    if (p.periods < 1) p.periods = 1;
    p.tearFree = true;
    const double a = blank / 2.0;
    p.startOffsetUs = P / 2 + a * L - p.windowUs - (p.rows / 2.0) * (p.rowUs - L);
}

/* ----------------------------------------------------------------- the task */

static void timer_cb(void *) { xTaskNotifyGive(s_task); }

static void wait_until(int64_t t) {
    int64_t d = t - esp_timer_get_time();
    if (d <= 20) return;
    ulTaskNotifyTake(pdTRUE, 0);
    esp_timer_start_once(s_timer, (uint64_t)d);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

static int64_t next_start(const ScanModel &m, int64_t after) {
    double u = m.lineAt(after - (int64_t)s_plan.startOffsetUs);
    double k = ceil(u / m.lines);
    return m.timeOf(k * m.lines) + (int64_t)s_plan.startOffsetUs;
}

static inline void release(uint8_t b) { if (b != RASTER_NO_BUF) xQueueSend(g_freeQ, &b, 0); }

// The adaptive frame period.  Late frames: one more refresh per frame at
// once.  A long run of on-time frames with the render task's busy time well
// inside one refresh fewer: one fewer.
#define GOV_LATE_WINDOW   30
#define GOV_LATE_LIMIT    2
#define GOV_GOOD_RUN      90
#define GOV_MAX_PERIODS   8
static void governor(bool dropped, double P) {
    static int window = 0, lates = 0, good = 0;
    static uint32_t maxBusy = 0;
    if (s_quiet) {                   // dark (a scene change): nothing it does can be seen
        window = lates = good = 0; maxBusy = 0; s_renderUs = 0;
        return;
    }
    uint32_t r = s_renderUs;
    s_renderUs = 0;
    if (r > maxBusy) maxBusy = r;
    window++;
    if (dropped) { lates++; good = 0; } else good++;
    if (lates >= GOV_LATE_LIMIT && s_periods >= RELAX_PERIODS && !s_relaxed) {
        s_relaxed = true;
        Serial.printf("raster: the render can't keep up with the chunks at %d refreshes a frame: "
                      "waiting for them instead (the picture may tear)\n", s_periods);
        window = lates = good = 0; maxBusy = 0;
        return;
    }
    if (lates >= GOV_LATE_LIMIT && s_periods < GOV_MAX_PERIODS) {
        s_periods = s_periods + 1;
        Serial.printf("raster: frames late, now %d refreshes a frame (%.1f fps)\n",
                      s_periods, 1e6 / (s_periods * P));
        window = lates = good = 0; maxBusy = 0;
        return;
    }
    if (window >= GOV_LATE_WINDOW) { window = lates = 0; }
    if (good >= GOV_GOOD_RUN) {
        const int floor = s_retryFloor > s_minPeriods ? s_retryFloor : s_minPeriods;
        if (s_periods > floor && maxBusy < 0.72 * (s_periods - 1) * P) {
            s_periods = s_periods - 1;
            Serial.printf("raster: render has time to spare (%u us), now %d refreshes a frame (%.1f fps)\n",
                          (unsigned)maxBusy, s_periods, 1e6 / (s_periods * P));
        }
        good = 0; maxBusy = 0;
    }
}

static void raster_task(void *) {
    ScanModel m = s_plan.model;
    const bool synced = s_plan.tearFree;
    const int chunks = s_plan.rows / CHUNK_ROWS;
    const double w = s_plan.rowUs;
    int64_t t0 = 0, lastStart = 0;

    for (int i = 0; i < g_nBufs; i++) { uint8_t b = i; xQueueSend(g_freeQ, &b, 0); }
    memset(s_pend, RASTER_NO_BUF, sizeof s_pend);

    for (uint32_t frame = 0;; frame++) {
        const double P = m.periodUs();
        int64_t now = esp_timer_get_time();
        if (synced) {
            const double frameUs = s_periods * P;
            int64_t after = frame == 0 ? now + 5000 : t0 + (int64_t)(frameUs - P / 2);
            t0 = next_start(m, after);
            if (t0 < now + 200) {
                s_slips = s_slips + 1;
                t0 = next_start(m, now + 500);
            }
            wait_until(t0);
        } else {
            // No scan to follow: just send frames back to back (they tear).
            vTaskDelay(1);
            t0 = esp_timer_get_time();
            if (lastStart) s_freeFrameUs = s_freeFrameUs + 0.1f * ((t0 - lastStart) - s_freeFrameUs);
        }
        lastStart = t0;

        g_rasterFrame = frame;
        const int64_t S = t0 - (int64_t)s_plan.startOffsetUs;
        const double a = (m.lines - s_plan.H) / 2.0;
        g_display->setWindow(0, 0, s_W - 1, s_plan.rows - 1);
        int inflight = 0;
        bool dropped = false;
        for (int c = 0; c < chunks && !dropped; c++) {
            int64_t deadline;
            if (synced && !s_relaxed) {
                int rEnd = (c + 1) * CHUNK_ROWS;
                deadline = S + (int64_t)(P - s_plan.guardUs + (a + rEnd) * m.lineUs
                                         - CHUNK_ROWS * w)
                           - (inflight ? (int64_t)(CHUNK_ROWS * w) : 0);
            } else {
                deadline = esp_timer_get_time() + RELAXED_WAIT_US;
            }
            RasterChunk rc;
            if (s_pend[c] != RASTER_NO_BUF) {             // already here
                rc = {s_pend[c], (uint16_t)c, frame};
                s_pend[c] = RASTER_NO_BUF;
            } else for (;;) {
                int64_t left = deadline - esp_timer_get_time();
                TickType_t ticks = left > 0 ? pdMS_TO_TICKS(left / 1000) : 0;
                if (xQueueReceive(g_readyQ, &rc, ticks ? ticks : 1) != pdTRUE) {
                    if (esp_timer_get_time() < deadline) continue;
                    dropped = true;
                    break;
                }
                if (rc.chunk == RASTER_ABANDONED || rc.frame < frame) {
                    release(rc.buf);
                    continue;
                }
                if (rc.chunk != c && rc.chunk < MAX_CHUNKS) {   // early: keep it for its turn
                    s_pend[rc.chunk] = rc.buf;
                    continue;
                }
                break;
            }
            if (dropped) break;
            spi_device_queue_trans(dev(), &s_trans[rc.buf], portMAX_DELAY);
            if (++inflight == 2) { release((uint8_t)collect()); inflight--; }
        }
        while (inflight) { release((uint8_t)collect()); inflight--; }
        for (int c = 0; c < MAX_CHUNKS; c++)                  // a dropped frame's early chunks
            if (s_pend[c] != RASTER_NO_BUF) { release(s_pend[c]); s_pend[c] = RASTER_NO_BUF; }
        if (dropped) {
            g_abortFrame = frame;
            s_late = s_late + 1;
        } else {
            s_frames = s_frames + 1;
        }
        if (synced) governor(dropped, P);
        if (s_retry) {                         // the load changed: try tear-free again
            s_retry = false;
            if (s_relaxed || s_periods > s_minPeriods)
                Serial.printf("raster: retrying at %d refreshes a frame, tear-free\n",
                              s_retryFloor > s_minPeriods ? s_retryFloor : s_minPeriods);
            s_relaxed = false;
            s_periods = s_retryFloor > s_minPeriods ? s_retryFloor : s_minPeriods;
        }

        // Where is the scan really?  Correct the model.
        if (synced) {
            uint32_t word, span;
            int64_t t;
            bool ok = scan_read_raw(word, &t, &span);
            if (ok && span > s_spanMax) s_spanMax = span;
            if (!ok || span >= SCAN_SPAN_MAX) s_scanRej = s_scanRej + 1;
            if (ok && span < SCAN_SPAN_MAX) {
                int v = scan_decode(word, m.skipBits);
                if (v >= 0 && v < m.lines) {
                    double e = scan_correct(m, v, t);
                    portENTER_CRITICAL(&s_statMux);
                    s_errSq = s_errSq + (float)(e * e);
                    s_errN = s_errN + 1;
                    s_lineUs = m.lineUs;
                    portEXIT_CRITICAL(&s_statMux);
                }
            }
        }
    }
}

void raster_start(const RasterPlan &p, int core, int priority, float maxFps) {
    s_plan = p;
    s_lineUs = p.model.lineUs;
    if (p.tearFree) {
        const double P = p.model.periodUs();
        int byFps = (int)ceil(1e6 / maxFps / P - 0.02);
        s_minPeriods = p.periods > byFps ? p.periods : byFps;
        s_periods = s_minPeriods;
    }
    s_freeFrameUs = (float)(p.transferUs + 3000);
    esp_timer_create_args_t ta = {};
    ta.callback = timer_cb;
    ta.name = "raster";
    esp_timer_create(&ta, &s_timer);
    if (xTaskCreatePinnedToCore(raster_task, "raster", 4096, nullptr, priority, &s_task, core) != pdPASS)
        Serial.println("raster: couldn't start the raster task (out of memory)");
}

float raster_frame_us(void) {
    if (!s_plan.tearFree) return s_freeFrameUs;
    return (float)(s_periods * s_lineUs * s_plan.model.lines);
}

void raster_retry(int minPeriods) { s_retryFloor = minPeriods; s_retry = true; }

void raster_quiet(bool quiet) {
    if (s_quiet && !quiet) s_retry = true;   // visible again: from the scene's floor
    s_quiet = quiet;
}

void raster_report_render_us(uint32_t us) {
    if (us > s_renderUs) s_renderUs = us;
}

void raster_take_stats(RasterStats &s) {
    portENTER_CRITICAL(&s_statMux);
    s.frames = s_frames; s_frames = 0;
    s.late = s_late; s_late = 0;
    s.slips = s_slips; s_slips = 0;
    s.scanErrRms = s_errN ? sqrtf(s_errSq / s_errN) : 0;
    s_errSq = 0; s_errN = 0;
    s.hz = s_lineUs > 0 ? (float)(1e6 / (s_lineUs * s_plan.model.lines)) : 0;
    s.periods = s_periods;
    s.relaxed = s_relaxed;
    s.scanRejected = s_scanRej; s_scanRej = 0;
    s.spanMax = s_spanMax; s_spanMax = 0;
    portEXIT_CRITICAL(&s_statMux);
}
