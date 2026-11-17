/*
 * scan.cpp - Reading the panel's scan line (0x45) and fitting it to the
 * ESP32's clock.  See scan.h.
 *
 * Copied unchanged from the RasterBouncingBalls example of
 * chipguy_ESP32-CYD-Resistive_display.
 *
 * Copyright (c) 2026 chipguyhere
 * MIT License
 */

#include "scan.h"
#include <Arduino.h>
#include <math.h>
#include <esp_timer.h>
#include <chipguy_CYD_resistive_display.h>

// lv_setup.hpp's display (static there), set by the sketch.
extern chipguy_CYD_resistive_display *g_display;

// Register reads are slow on these panels (a 150 ns read cycle on the
// ILI9341 and ST7789): 4 MHz is well inside it.
#define SCAN_READ_HZ 4000000

bool scan_read_raw(uint32_t &word, int64_t *t, uint32_t *spanUs) {
    uint8_t b[4];
    int64_t t0 = esp_timer_get_time();
    if (!g_display->readCommand(0x45, b, 4, SCAN_READ_HZ)) return false;
    int64_t t1 = esp_timer_get_time();
    word = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
    if (t) *t = (t0 + t1) / 2;
    if (spanUs) *spanUs = (uint32_t)(t1 - t0);
    return true;
}

// The datasheets give the reply as a dummy, then 0000 00 L9 L8, then L7..L0.
// Whether the dummy is a byte or a single clock differs between panels, so
// the field is looked for at every offset.
int scan_decode(uint32_t word, int skip) {
    if (skip < 0 || skip > 16) return -1;
    uint32_t f = (word >> (16 - skip)) & 0xFFFF;
    return (f >> 10) ? -1 : (int)(f & 0x3FF);
}

#define DETECT_N 1500

bool scan_detect(ScanModel &m, int visibleRows, char *why, size_t whyLen) {
    m.valid = false;
    uint32_t *w = (uint32_t *)malloc(DETECT_N * sizeof(uint32_t));
    if (!w) { snprintf(why, whyLen, "out of memory"); return false; }
    for (int i = 0; i < DETECT_N; i++) {
        if (!scan_read_raw(w[i])) {
            free(w);
            snprintf(why, whyLen, "this board can't read from its panel");
            return false;
        }
    }
    // Score each layout: a real line counter gives valid numbers, steps
    // forward by a few lines between back-to-back readings, wraps once per
    // refresh, and tops out a little past the visible rows.
    int bestSkip = -1, bestMax = 0;
    float bestScore = 0;
    for (int skip = 0; skip <= 16; skip++) {
        int valid = 0, maxV = -1, fwd = 0, distinct = 0;
        static uint8_t seen[1024 / 8];
        memset(seen, 0, sizeof seen);
        for (int i = 0; i < DETECT_N; i++) {
            int v = scan_decode(w[i], skip);
            if (v < 0) continue;
            valid++;
            if (v > maxV) maxV = v;
            if (!(seen[v >> 3] & (1 << (v & 7)))) { seen[v >> 3] |= 1 << (v & 7); distinct++; }
        }
        if (maxV < visibleRows - 1 || maxV > visibleRows * 3 / 2) continue;
        const int n = maxV + 1;
        int pairs = 0;
        for (int i = 1; i < DETECT_N; i++) {
            int a = scan_decode(w[i - 1], skip), b = scan_decode(w[i], skip);
            if (a < 0 || b < 0) continue;
            pairs++;
            int d = (b - a + n) % n;
            if (d > 0 && d < n / 8) fwd++;
        }
        float score = (float)valid / DETECT_N * (pairs ? (float)fwd / pairs : 0);
        // Must pass through many lines -- not every one: where a read takes
        // several lines' time (the S3: about three) half of them are never seen.
        if (distinct < visibleRows / 8) score = 0;
        Serial.printf("scan detect: skip %2d: valid %4d, max %4d, distinct %4d, forward %4d/%4d, score %.3f\n",
                      skip, valid, maxV, distinct, fwd, pairs, score);
        if (score > bestScore) { bestScore = score; bestSkip = skip; bestMax = maxV; }
    }
    // The ST7789's counter steps less cleanly (about 94 % of back-to-back
    // readings step forward); the ILI9341's and ST7796's 99-100 %.
    if (bestSkip < 0 || bestScore < 0.8f) {
        snprintf(why, whyLen, "no reply looks like a line counter (first readings %08lX %08lX %08lX)",
                 (unsigned long)w[0], (unsigned long)w[1], (unsigned long)w[2]);
        free(w);
        return false;
    }
    free(w);
    m.skipBits = bestSkip;
    m.lines = bestMax + 1;
    return true;
}

bool scan_measure(ScanModel &m, uint32_t ms, void (*progress)()) {
    int64_t nextProgress = esp_timer_get_time() + 200000;
    // Least squares of the unwrapped line u against time t (seconds from the
    // first reading, so the sums stay well inside a double's precision).
    double St = 0, Su = 0, Stt = 0, Stu = 0, Suu = 0;
    uint32_t n = 0, rejected = 0;
    int64_t t0 = 0;
    int prev = -1;
    double u = 0;
    int64_t tPrev = 0;
    bool gap = false;      // progress() just ran: readings paused
    const int N = m.lines;
    const int64_t end = esp_timer_get_time() + (int64_t)ms * 1000;
    while (esp_timer_get_time() < end) {
        if (progress && esp_timer_get_time() >= nextProgress) {
            progress();
            nextProgress = esp_timer_get_time() + 200000;
            gap = true;
        }
        uint32_t word, span;
        int64_t t;
        if (!scan_read_raw(word, &t, &span)) return false;
        int v = scan_decode(word, m.skipBits);
        // A read that was interrupted (another task ran) has a vague time.
        if (v < 0 || v >= N || span > 200) { rejected++; continue; }
        if (prev < 0) { t0 = t; u = v; }
        else if (gap && n > 100) {
            // After a pause the scan may have gone round any number of
            // times: predict where it is from the slope fitted so far, and
            // take the wrap of v nearest that.
            double den = n * Stt - St * St;
            double slope = den > 0 ? (n * Stu - St * Su) / den : 0;
            double pred = u + slope * (t - tPrev) * 1e-6;
            u = v + N * floor((pred - v) / N + 0.5);
        } else {
            int d = (v - prev + N) % N;
            if (d > N / 2) { rejected++; continue; }   // went backwards: a bad read
            u += d;
        }
        gap = false;
        prev = v;
        tPrev = t;
        double x = (t - t0) * 1e-6;
        St += x; Su += u; Stt += x * x; Stu += x * u; Suu += u * u;
        n++;
    }
    if (n < 100) return false;
    double den = n * Stt - St * St;
    if (den <= 0) return false;
    double b = (n * Stu - St * Su) / den;          // lines per second
    double a = (Su - b * St) / n;                  // line at t0
    double ss = Suu - 2 * a * Su - 2 * b * Stu + n * a * a + 2 * a * b * St + b * b * Stt;
    if (b <= 0) return false;
    m.lineUs = 1e6 / b;
    m.refT = t0;
    m.refLine = a;
    m.rmsLines = sqrt(ss > 0 ? ss / n : 0);
    m.samples = n;
    m.valid = true;
    Serial.printf("scan fit: %u readings (%u rejected), %d lines, %.3f us a line, %.3f Hz, rms %.2f lines\n",
                  (unsigned)n, (unsigned)rejected, N, m.lineUs, m.hz(), m.rmsLines);
    return true;
}

double scan_correct(ScanModel &m, int line, int64_t t) {
    const int N = m.lines;
    double u = m.lineAt(t);
    double pm = fmod(u, N);
    if (pm < 0) pm += N;
    double err = line - pm;
    if (err > N / 2.0) err -= N;
    if (err < -N / 2.0) err += N;
    // An outlier (a bad read) moves the model only a little.
    double e = err;
    if (e > 8) e = 8;
    if (e < -8) e = -8;
    // Line time: an error that persists is the oscillators' rate differing.
    // Lines since the last correction = how long that error took to build.
    double since = u - m.refLine;
    if (since > N / 2.0) m.lineUs *= 1.0 - 0.05 * e / since;
    // Phase: re-anchor at this reading, a third of the way to it.
    m.refLine = u + e / 3;
    m.refT = t;
    return err;
}
