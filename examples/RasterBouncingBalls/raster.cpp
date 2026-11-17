/*
 * raster.cpp - The raster task.  See raster.h for the division of work.
 *
 * Tear-free timing ("beam chasing").  The panel scans row r at
 *     S + (a + r) * L        (S: the moment it reaches line 0, L: one line,
 *                             a: lines of blanking before the first row)
 * and again one refresh P later.  We write row r at about
 *     t0 + c + r * w         (t0: start, c: the window command, w: one row)
 * A row shows the new frame from the first scan after it was written, so the
 * whole area changes on the same refresh -- no tear -- if every row is
 * written between its scan on refresh n and its scan on refresh n+1:
 *     0 < (t0 + c + r*w) - (S + (a + r)*L) < P     for every row r.
 * That difference changes by (w - L) per row: B*(w - L) over the B rows of
 * the ball area.  So it can be done when |B*(w - L)| plus a guard each side
 * fits in P, and the start t0 is placed to centre the range in the window.
 * If sending the area takes longer than one refresh (w > L*N/B), a frame
 * takes two refreshes: still tear-free, at half the rate.
 *
 * Copyright (c) 2026 chipguyhere
 * MIT License
 */

#include "raster.h"
#include "balls.h"
#include <math.h>
#include <esp_timer.h>
#include <driver/spi_master.h>
#include <chipguy_CYD_resistive_display.h>

extern chipguy_CYD_resistive_display *g_display;

uint16_t        *g_bounce[RASTER_BUFS];
QueueHandle_t    g_freeQ, g_readyQ;
volatile int32_t g_abortFrame = -1;
volatile uint32_t g_rasterFrame = 0;

static spi_transaction_t s_trans[RASTER_BUFS];
static spi_transaction_t s_lvglTrans;
static int s_W = 0;
static RasterPlan s_plan;
static TaskHandle_t s_task = nullptr;
static esp_timer_handle_t s_timer = nullptr;

// LVGL's rows, handed over by raster_send_rows().
struct LvglJob { const uint16_t *px; int y1, y2; };
static QueueHandle_t     s_lvglQ;
static SemaphoreHandle_t s_lvglDone;

// Stats.
static volatile uint32_t s_frames, s_late, s_slips, s_touch, s_lvglRows, s_forced, s_scanSkips;
static volatile float    s_errSq;
static volatile uint32_t s_errN;
static portMUX_TYPE      s_statMux = portMUX_INITIALIZER_UNLOCKED;
static volatile double   s_lineUs;   // the model's, for the stats

bool raster_alloc(int width) {
    s_W = width;
    for (int i = 0; i < RASTER_BUFS; i++) {
        g_bounce[i] = (uint16_t *)heap_caps_malloc((size_t)width * CHUNK_ROWS * 2, MALLOC_CAP_DMA);
        if (!g_bounce[i]) return false;
        s_trans[i] = {};
        s_trans[i].length = (size_t)width * CHUNK_ROWS * 16;
        s_trans[i].tx_buffer = g_bounce[i];
        s_trans[i].user = (void *)1;       // DC = data
    }
    g_freeQ = xQueueCreate(RASTER_BUFS, sizeof(uint8_t));
    g_readyQ = xQueueCreate(RASTER_BUFS + 2, sizeof(RasterChunk));   // + abandonments
    s_lvglQ = xQueueCreate(1, sizeof(LvglJob));
    s_lvglDone = xSemaphoreCreateBinary();
    return g_freeQ && g_readyQ && s_lvglQ && s_lvglDone;
}

static inline spi_device_handle_t dev() { return g_display->spiDevice(); }

// Wait for one queued transaction; returns its buffer index (or -1 for LVGL's).
static int collect() {
    spi_transaction_t *t = nullptr;
    spi_device_get_trans_result(dev(), &t, portMAX_DELAY);
    for (int i = 0; i < RASTER_BUFS; i++) if (t == &s_trans[i]) return i;
    return -1;
}

bool raster_measure(int width, int rows, double &rowUs, double &windowUs) {
    g_display->waitFlush();
    for (int i = 0; i < RASTER_BUFS; i++)
        for (int k = 0; k < width * CHUNK_ROWS; k++) g_bounce[i][k] = 0x2108;   // dark grey
    const int chunks = rows / CHUNK_ROWS;
    double best = 1e18, bestWin = 1e18;
    for (int rep = 0; rep < 6; rep++) {
        int64_t t0 = esp_timer_get_time();
        g_display->setWindow(0, 0, width - 1, rows - 1);
        int64_t t1 = esp_timer_get_time();
        int inflight = 0;
        for (int c = 0; c < chunks; c++) {
            spi_device_queue_trans(dev(), &s_trans[c % RASTER_BUFS], portMAX_DELAY);
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

// Every frame keeps time for LVGL: this many of its rows (full width), plus
// the scan reading and two window commands.  So a plan never leaves LVGL
// without a turn, even before the starvation rule in serve_gap() kicks in.
#define LVGL_ROWS_PER_FRAME 16
#define SCAN_READ_US 150.0

void raster_plan(RasterPlan &p) {
    p.transferUs = p.windowUs + p.ballRows * p.rowUs;
    p.tearFree = false;
    p.periods = 0;
    p.stripInStep = false;
    if (!p.scanKnown) return;
    const ScanModel &m = p.model;
    const double P = m.periodUs(), L = m.lineUs;
    const int blank = m.lines - p.H;
    // The guard: wake-up latency, not knowing where in the blanking the
    // first row is, and the scatter of the readings.
    p.guardUs = 300 + (blank / 2.0 + 1) * L + 3 * m.rmsLines * L;
    p.spanUs = fabs(p.ballRows * (p.rowUs - L));
    if (p.spanUs + 2 * p.guardUs > P) return;
    const double reserve = 2 * p.windowUs + SCAN_READ_US + LVGL_ROWS_PER_FRAME * p.rowUs;
    if (p.transferUs + reserve <= P) p.periods = 1;
    else if (p.transferUs + reserve <= 2 * P) p.periods = 2;
    else return;
    p.tearFree = true;
    p.stripInStep = fabs(p.H * (p.rowUs - L)) + 2 * p.guardUs <= P;
    // Centre the write's offset against the scan, over the area, at P/2:
    //   t0 + c + (B/2) w - (S + (a + B/2) L) = P/2
    const double a = blank / 2.0;
    p.startOffsetUs = P / 2 + a * L - p.windowUs - (p.ballRows / 2.0) * (p.rowUs - L);
    // Half rate, paced: the write's offset against the scan runs from one
    // guard after the scan passes row 0 to 90 % of the way to the next pass,
    // evenly, and the frame plus LVGL's share fits in two refreshes.
    p.pacedRowUs = 0;
    if (p.periods == 1) {
        double rowUs = L + 0.9 * (P - 2 * p.guardUs) / p.ballRows;
        const double maxRowUs = (2 * P - reserve - p.windowUs) / p.ballRows;
        if (rowUs > maxRowUs) rowUs = maxRowUs;
        if (rowUs > p.rowUs) {
            p.pacedRowUs = rowUs;
            p.pacedStartOffsetUs = a * L + p.guardUs - p.windowUs;
        }
    }
}

/* ----------------------------------------------------------------- the task */

static void timer_cb(void *) { xTaskNotifyGive(s_task); }

static void wait_until(int64_t t) {
    int64_t d = t - esp_timer_get_time();
    if (d <= 20) return;
    ulTaskNotifyTake(pdTRUE, 0);               // clear a stale wake-up
    esp_timer_start_once(s_timer, (uint64_t)d);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

// The first start at or after `after`: S + startOffset, S a time the scan is at line 0.
static int64_t next_start(const ScanModel &m, int64_t after, double offsetUs) {
    double u = m.lineAt(after - (int64_t)offsetUs);
    double k = ceil(u / m.lines);
    return m.timeOf(k * m.lines) + (int64_t)offsetUs;
}

// The longest touch sample seen, so one isn't started without time for it.
static uint32_t s_touchUs = 1500;
static int s_touchSkipped = 0;   // gaps in a row with no room for a touch sample

// One touch sample, if there's room before `deadline`.  It does nothing
// unless the screen is pressed and the cached sample is old.
static bool touch_sample(int64_t deadline) {
    if (deadline - esp_timer_get_time() <= (int64_t)s_touchUs + 200) return false;
    int64_t t = esp_timer_get_time();
    g_display->runTouchSampler();
    uint32_t us = (uint32_t)(esp_timer_get_time() - t);
    if (us > 100) { s_touch = s_touch + 1; if (us > s_touchUs) s_touchUs = us; }
    return true;
}

// LVGL rows sent at least, past the deadline if need be, once LVGL has
// waited this many gaps without a row.  The frame then starts a refresh late
// (a repeated frame: a stutter, not a tear), so LVGL can't freeze.
#define LVGL_STARVED_GAPS 3
#define LVGL_MIN_ROWS     8
static int s_lvglStarved = 0;    // gaps in a row with rows waiting and none sent

// Use the bus until `deadline`: LVGL's rows first, then a touch sample with
// what's left -- unless touch has gone three gaps without one, then it goes
// first.  Returns true if LVGL rows are still waiting.
static bool serve_gap(int64_t deadline, LvglJob &job, bool &haveJob) {
    const double w = s_plan.rowUs, c = s_plan.windowUs + 30;
    bool touched = false, sent = false;
    const bool force = haveJob && s_lvglStarved >= LVGL_STARVED_GAPS;
    if (s_touchSkipped >= 3) touched = touch_sample(deadline);
    for (;;) {
        int64_t now = esp_timer_get_time();
        int64_t left = deadline - now;
        if (!haveJob) {
            // Waiting rows are taken at once; with time to spare, wait for some.
            TickType_t ticks = left >= 3000 ? pdMS_TO_TICKS((left - 1000) / 1000) : 0;
            if (xQueueReceive(s_lvglQ, &job, ticks) != pdTRUE) break;
            haveJob = true;
            continue;
        }
        int rows = (int)((left - c) / w);
        if (force && !sent && rows < LVGL_MIN_ROWS) { rows = LVGL_MIN_ROWS; s_forced = s_forced + 1; }
        int rest = job.y2 - job.y1 + 1;
        if (rows > rest) rows = rest;
        if (rows <= 0) break;
        sent = true;
        g_display->setWindow(0, job.y1, s_W - 1, job.y1 + rows - 1);
        s_lvglTrans = {};
        s_lvglTrans.length = (size_t)rows * s_W * 16;
        s_lvglTrans.tx_buffer = job.px;
        s_lvglTrans.user = (void *)1;
        spi_device_queue_trans(dev(), &s_lvglTrans, portMAX_DELAY);
        collect();
        s_lvglRows = s_lvglRows + rows;
        job.px += (size_t)rows * s_W;
        job.y1 += rows;
        if (job.y1 > job.y2) {
            haveJob = false;
            xSemaphoreGive(s_lvglDone);
        }
    }
    // Rows still waiting: the touch sample yields (but not for long).
    if (!touched && !haveJob) touched = touch_sample(deadline);
    s_touchSkipped = touched ? 0 : s_touchSkipped + 1;
    s_lvglStarved = haveJob && !sent ? s_lvglStarved + 1 : 0;
    return haveJob;
}

/* ----------------------------------------------------------------- half rate */

static volatile int  s_periods = 0;        // refreshes per frame now (0: not timed)
static volatile bool s_autoHalf = false;

bool raster_set_half_rate(bool on) {
    if (!s_plan.tearFree || s_plan.periods != 1) return false;
    s_periods = on ? 2 : 1;
    return true;
}
int raster_periods() { return s_periods; }
void raster_auto_half_rate(bool on) { s_autoHalf = on; }

// Give a buffer back to the render task.
static inline void release(uint8_t b) { if (b != RASTER_NO_BUF) xQueueSend(g_freeQ, &b, 0); }

/* ----------------------------------------------------------------- watchdog */

#define WD_SILENT_US    250000   // no report from the render task this long, with a frame waiting
#define WD_LATE_FRAMES  20       // or this many late frames in a row
#define WD_GOOD_FRAMES  60       // good frames in a row that end it
#define WD_TOGGLE_US    300000   // how often the render task's priority flips
#define WD_MESSAGE_US   5000000

static TaskHandle_t  s_render = nullptr;
static UBaseType_t   s_renderPrio = 0, s_lowPrio = 0, s_lvglPrio = 0;
static volatile bool s_wdOn = true;
static int64_t       s_lastReport = 0;       // the render task's last report, either kind
static int           s_lateRun = 0, s_goodRun = 0;
static bool          s_tripped = false, s_low = false;
static int64_t       s_nextToggle = 0, s_nextMessage = 0;

void raster_watch(TaskHandle_t render, UBaseType_t normalPrio, UBaseType_t lvglPrio) {
    s_render = render;
    s_renderPrio = normalPrio;
    s_lvglPrio = lvglPrio;
    s_lowPrio = lvglPrio > 0 ? lvglPrio - 1 : 0;
}

void raster_watchdog(bool on) { s_wdOn = on; }

static void wd_restore() {
    if (s_low && s_render) vTaskPrioritySet(s_render, s_renderPrio);
    s_tripped = s_low = false;
}

// After each frame: dropped (a chunk wasn't delivered by its deadline).
static void watchdog_frame(bool dropped) {
    if (!s_render) return;
    if (!s_wdOn) { if (s_tripped) wd_restore(); s_lateRun = s_goodRun = 0; return; }
    const int64_t now = esp_timer_get_time();
    s_lateRun = dropped ? s_lateRun + 1 : 0;
    s_goodRun = dropped ? 0 : s_goodRun + 1;
    const bool silent = dropped && now - s_lastReport > WD_SILENT_US;
    if (!s_tripped && (silent || s_lateRun >= WD_LATE_FRAMES)) {
        s_tripped = true;
        s_nextToggle = now;
        s_nextMessage = now;
        Serial.printf("raster watchdog: %s.\n", silent
                      ? "the render task has stopped reporting chunks"
                      : "the render task has been late for many frames in a row");
    }
    if (!s_tripped) return;
    if (s_goodRun >= WD_GOOD_FRAMES) {
        wd_restore();
        Serial.println("raster watchdog: the render task is keeping up again; its priority is back to normal.");
        return;
    }
    if (now >= s_nextToggle) {
        s_low = !s_low;
        vTaskPrioritySet(s_render, s_low ? s_lowPrio : s_renderPrio);
        s_nextToggle = now + WD_TOGGLE_US;
    }
    if (now >= s_nextMessage) {
        s_nextMessage = now + WD_MESSAGE_US;
        Serial.printf("raster watchdog: a task above LVGL's priority (the render task, %u) is "
                      "running overtime and starving the tasks below it (LVGL, %u). Its "
                      "priority is being switched above and below LVGL's every %u ms to make "
                      "that visible: the balls and the controls will freeze by turns. Make the "
                      "render task finish each chunk in time, or call raster_watchdog(false) "
                      "to stop this.\n", (unsigned)s_renderPrio, (unsigned)s_lvglPrio,
                      (unsigned)(WD_TOGGLE_US / 1000));
    }
}

static void raster_task(void *) {
    ScanModel m = s_plan.model;
    const bool synced = s_plan.tearFree;
    const int chunks = s_plan.ballRows / CHUNK_ROWS;
    const double P = m.periodUs(), w = s_plan.rowUs;
    int periodsNow = 0;                        // what the physics was last told
    bool paced = false;                        // this frame's chunks are paced
    bool slipped = false;                      // this frame's start was missed
    double offset = s_plan.startOffsetUs;      // this frame's start against the scan
    uint32_t autoWindowStart = 0, autoLate = 0;
    LvglJob job = {};
    bool haveJob = false;
    int64_t t0 = 0, lastStart = 0;
    bool backlog = false;
    double freeFrameUs = s_plan.transferUs + 3000;

    for (int i = 0; i < RASTER_BUFS; i++) { uint8_t b = i; xQueueSend(g_freeQ, &b, 0); }
    s_lastReport = esp_timer_get_time();

    for (uint32_t frame = 0;; frame++) {
        // 1. When this frame starts.
        int64_t now = esp_timer_get_time();
        if (synced) {
            const int periods = s_periods;
            if (periods != periodsNow) {          // half rate switched: the physics follows
                periodsNow = periods;
                balls_set_frame_us((float)(periods * P));
            }
            const double frameUs = periods * P;
            // Half rate is paced, from an earlier start (see raster_plan()).
            paced = periods == 2 && s_plan.pacedRowUs > 0;
            offset = paced ? s_plan.pacedStartOffsetUs : s_plan.startOffsetUs;
            int64_t after = frame == 0 ? now + 5000 : t0 + (int64_t)(frameUs - P / 2);
            t0 = next_start(m, after, offset);
            if (t0 < now + 200) {              // missed: the next refresh's slot
                s_slips = s_slips + 1;
                slipped = true;
                t0 = next_start(m, now + 500, offset);
            }
            // 2. LVGL and touch until shortly before then.
            backlog = serve_gap(t0 - 150, job, haveJob);
            wait_until(t0);
        } else {
            // No scan to follow: a few ms of LVGL after each frame, then on.
            backlog = serve_gap(now + 3000, job, haveJob);
            t0 = esp_timer_get_time();
            if (lastStart) freeFrameUs += 0.1 * ((t0 - lastStart) - freeFrameUs);
            balls_set_frame_us((float)freeFrameUs);
        }
        lastStart = t0;

        // 3. The ball area, chunk by chunk as the render task delivers.
        g_rasterFrame = frame;
        const int64_t S = t0 - (int64_t)offset;   // scan at line 0
        const double a = (m.lines - s_plan.H) / 2.0;
        g_display->setWindow(0, 0, s_W - 1, s_plan.ballRows - 1);
        int inflight = 0;
        bool dropped = false;
        for (int c = 0; c < chunks && !dropped; c++) {
            // The latest the chunk may start so its last row still lands
            // before the scan's next pass over it.
            int64_t deadline;
            if (synced) {
                int rEnd = (c + 1) * CHUNK_ROWS;
                deadline = S + (int64_t)(P - s_plan.guardUs + (a + rEnd) * m.lineUs
                                         - CHUNK_ROWS * w)
                           - (inflight ? (int64_t)(CHUNK_ROWS * w) : 0);
            } else {
                // No scan to keep to, but a chunk is still expected in time:
                // four times its own send time (at least 5 ms).
                double d = 4 * CHUNK_ROWS * w;
                deadline = esp_timer_get_time() + (int64_t)(d > 5000 ? d : 5000);
            }
            // Paced: not before this chunk's share of the frame.
            if (paced) wait_until(t0 + (int64_t)(s_plan.windowUs + c * CHUNK_ROWS * s_plan.pacedRowUs));
            RasterChunk rc;
            for (;;) {
                int64_t left = deadline - esp_timer_get_time();
                TickType_t ticks = left > 0 ? pdMS_TO_TICKS(left / 1000) : 0;
                if (xQueueReceive(g_readyQ, &rc, ticks ? ticks : 1) != pdTRUE) {
                    if (esp_timer_get_time() < deadline) continue;
                    dropped = true;            // do or die: this frame is lost from here down
                    break;
                }
                s_lastReport = esp_timer_get_time();   // drawn or abandoned: it's alive
                if (rc.chunk == RASTER_ABANDONED || rc.frame < frame) {
                    release(rc.buf);           // left from a dropped frame
                    continue;
                }
                break;
            }
            if (dropped) break;
            spi_device_queue_trans(dev(), &s_trans[rc.buf], portMAX_DELAY);
            if (++inflight == 2) { release((uint8_t)collect()); inflight--; }
        }
        while (inflight) { release((uint8_t)collect()); inflight--; }
        if (dropped) {
            g_abortFrame = frame;
            s_late = s_late + 1;
        } else {
            s_frames = s_frames + 1;
        }
        watchdog_frame(dropped);
        // Automatic half rate: 3 late or slipped frames within 60.
        if (s_autoHalf && s_periods == 1) {
            if (frame - autoWindowStart >= 60) { autoWindowStart = frame; autoLate = 0; }
            if ((dropped || slipped) && ++autoLate >= 3) {
                s_periods = 2;
                Serial.println("raster: frames are coming out late at the full rate; "
                               "dropped to half rate (raster_auto_half_rate).");
            }
        }

        slipped = false;

        // 4. Where is the scan really?  Correct the model -- every other
        // frame while LVGL has rows waiting, to leave it the time.
        if (synced && backlog && (frame & 1)) s_scanSkips = s_scanSkips + 1;
        else if (synced) {
            uint32_t word, span;
            int64_t t;
            if (scan_read_raw(word, &t, &span) && span < 200) {
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

void raster_start(const RasterPlan &p, int core, int priority) {
    s_plan = p;
    s_periods = p.tearFree ? p.periods : 0;
    s_lineUs = p.model.lineUs;
    esp_timer_create_args_t ta = {};
    ta.callback = timer_cb;
    ta.name = "raster";
    esp_timer_create(&ta, &s_timer);
    balls_set_frame_us(p.tearFree ? (float)(p.periods * p.model.periodUs())
                                  : (float)(p.transferUs + 3000));
    xTaskCreatePinnedToCore(raster_task, "raster", 4096, nullptr, priority, &s_task, core);
}

void raster_send_rows(const uint16_t *px, int y1, int y2) {
    LvglJob j = {px, y1, y2};
    xQueueSend(s_lvglQ, &j, portMAX_DELAY);
    xSemaphoreTake(s_lvglDone, portMAX_DELAY);
}

void raster_take_stats(RasterStats &s) {
    portENTER_CRITICAL(&s_statMux);
    s.frames = s_frames; s_frames = 0;
    s.late = s_late; s_late = 0;
    s.slips = s_slips; s_slips = 0;
    s.touchSamples = s_touch; s_touch = 0;
    s.lvglRows = s_lvglRows; s_lvglRows = 0;
    s.lvglForced = s_forced; s_forced = 0;
    s.scanSkips = s_scanSkips; s_scanSkips = 0;
    s.scanErrRms = s_errN ? sqrtf(s_errSq / s_errN) : 0;
    s_errSq = 0; s_errN = 0;
    s.hz = s_lineUs > 0 ? (float)(1e6 / (s_lineUs * s_plan.model.lines)) : 0;
    portEXIT_CRITICAL(&s_statMux);
}
