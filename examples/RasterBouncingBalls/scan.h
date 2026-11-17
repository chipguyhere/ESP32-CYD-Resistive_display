/*
 * scan.h - Where is the panel scanning?  Reading command 0x45 and keeping a
 * model of the scan against the ESP32's clock.
 *
 * The panel redraws its picture from its own memory, one line at a time, on
 * its own oscillator.  Command 0x45 ("get scan line") returns the line being
 * drawn.  Reading it is slow (tens of microseconds) and needs the SPI bus, so
 * it is read now and then and the line in between is worked out from a
 * straight-line fit: line = refLine + (time - refT) / lineUs, counting on
 * past the end of each refresh (an "unwrapped" line number; the panel's own
 * number is that modulo `lines`).
 *
 * Copyright (c) 2026 chipguyhere
 * MIT License
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

struct ScanModel {
    bool    valid = false;
    int     skipBits = 8;      // bits clocked out before the line number's 16-bit field
    int     lines = 0;         // lines per refresh, blanking included
    double  lineUs = 0;        // one line's time, in ESP32 microseconds
    int64_t refT = 0;          // esp_timer time at which ...
    double  refLine = 0;       // ... the unwrapped line was this
    double  rmsLines = 0;      // how far the readings sat from the fit
    uint32_t samples = 0;      // readings the fit used

    double  periodUs() const { return lineUs * lines; }
    double  hz() const { return lineUs > 0 ? 1e6 / periodUs() : 0; }
    double  lineAt(int64_t t) const { return refLine + (double)(t - refT) / lineUs; }
    // The time the unwrapped line is u.
    int64_t timeOf(double u) const { return refT + (int64_t)((u - refLine) * lineUs); }
};

// One reading of 0x45, as the 32 bits clocked out after the command.  False
// if the board can't read from the panel.  *t gets the reading's time (the
// middle of the read), *spanUs how long the read took.
bool scan_read_raw(uint32_t &word, int64_t *t = nullptr, uint32_t *spanUs = nullptr);

// The line number in a raw reading for a given skip, or -1 if the field's top
// six bits aren't zero (so it can't be a line number).
int scan_decode(uint32_t word, int skip);

// Find how the reply is laid out (the skip) and how many lines a refresh has.
// Reads for ~100 ms.  `visibleRows` is the panel's height in rows: the line
// count must be a little more.  False if no layout gives a counter that
// steps through every line; `why` then says what was seen.
bool scan_detect(ScanModel &m, int visibleRows, char *why, size_t whyLen);

// Time the scan against the ESP32's clock for `ms` and fit the model.
// progress, if given, is called about five times a second (it may draw).
bool scan_measure(ScanModel &m, uint32_t ms, void (*progress)() = nullptr);

// Fold one fresh reading into the model (the raster task does this after
// every frame): the phase moves part of the way to the reading, and the line
// time a little, so the model follows the panel's oscillator as it drifts.
// Returns the reading's error against the model, in lines.
double scan_correct(ScanModel &m, int line, int64_t t);
