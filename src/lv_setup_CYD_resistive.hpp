/*
 * lv_setup_CYD_resistive.hpp - Connects LVGL to the display and touch of the
 * resistive-touch CYD family (ESP32-2432S028; ESP32-32E 2.8", 3.2" 240x320,
 * 3.5" / 4.0" 320x480; ESP32-024 2.4" 240x320; the board is detected in begin()).
 *
 * This is the *actual* LVGL-to-driver glue for this board.  It ships inside the
 * library (src/) so every example can share one copy.  Each example's sketch
 * folder contains a small lv_setup.hpp that simply #includes this file; that
 * thin shim is the file you edit if you want to take this code over and
 * customize it (see the comments in any example's lv_setup.hpp).
 *
 * Include this file (or, normally, the example's lv_setup.hpp) after lvgl.h.
 *
 * The panel is driven in LVGL PARTIAL render mode: LVGL renders into a small
 * in-RAM buffer and hands us regions to blit over SPI.  Touch comes from the
 * XPT2046 resistive controller: on its own pins on the 2432S028 and 32E 2.8",
 * on the display's SPI bus on the 3.2" / 3.5" / 4.0" / ESP32-024 (the touch driver
 * then waits for any in-flight display DMA before each read).
 */

#pragma once

#include <algorithm>
#include <chipguy_CYD_resistive_display.h>
#include <chipguy_CYD_resistive_touch.h>

static chipguy_CYD_resistive_display display;

// Height (in rows) of the LVGL partial-render buffer.  Bigger = fewer flushes
// (smoother) at the cost of RAM.  Per buffer: 240*40*2 = ~19 KB on a 240x320
// panel, 320*40*2 = ~25 KB on a 320x480 one (portrait; landscape is wider).
// begin()'s maxBufferBytes can cap it lower.
#define LV_SETUP_BUFFER_ROWS 40

class lv_setup_class {
public:
    // Initializes display, touch input, and LVGL.
    // rotation: 0 = portrait (240x320 or 320x480), 1 = landscape,
    //           2 = portrait flipped, 3 = landscape flipped.  Degrees
    //           (0/90/180/270) are accepted as aliases for 0/1/2/3.
    // maxBufferBytes: if not 0, the most RAM each of LVGL's two render buffers
    //           may take (rounded down to whole rows) - for a sketch that needs
    //           the DMA-capable RAM for something else.  0 = LV_SETUP_BUFFER_ROWS rows.
    void begin(uint16_t rotation = 0, size_t maxBufferBytes = 0) {
        if (!display.begin(rotation)) {
            Serial.println("Display init failed!");
            while (1) delay(100);
        }
        Serial.printf("Display initialized: %s, %dx%d\n", display.panelName(),
                      display.nativeWidth(), display.nativeHeight());

        // On the shared-touch boards touch shares the display's SPI bus, so it must
        // come up AFTER the display's begin() has initialized that bus.
        if (!_touch.begin(display)) {
            Serial.println("Touch init failed!");
            while (1) delay(100);
        }
        _touch.loadCalibration();   // apply saved calibration if TouchCalibrate has run
        _touch.loadFilters();       // and the saved pressure / jump-filter settings
        _touch.setRotation(rotation);

        lv_init();

        static lv_display_t *disp = lv_display_create(display.width(), display.height());
        lv_display_set_flush_cb(disp, _disp_flush);
        lv_display_set_user_data(disp, this);
        // Force RGB565 so our flush byte-swap (2 bytes/pixel) is always correct,
        // independent of how LVGL's own lv_conf.h color depth resolved.
        lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);

        // Two DMA-capable partial-render buffers (RGB565 = 2 bytes/pixel).
        // The second buffer is what lets the driver DMA one region to the panel
        // while LVGL renders the next region into the other — see flushRegion().
        size_t row_bytes = (size_t)display.width() * 2;
        size_t buf_bytes = row_bytes * LV_SETUP_BUFFER_ROWS;
        if (maxBufferBytes && buf_bytes > maxBufferBytes)
            buf_bytes = std::max<size_t>(1, maxBufferBytes / row_bytes) * row_bytes;
        lv_color_t *buf1 = (lv_color_t *)heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA);
        lv_color_t *buf2 = (lv_color_t *)heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA);
        if (!buf1 || !buf2) {
            Serial.println("LVGL buffer allocation failed!");
            while (1) delay(100);
        }
        lv_display_set_buffers(disp, buf1, buf2, buf_bytes,
                               LV_DISPLAY_RENDER_MODE_PARTIAL);
        _buf[0] = (uint16_t *)buf1;
        _buf[1] = (uint16_t *)buf2;
        _bufBytes = buf_bytes;

        static lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, _touchpad_read);
        lv_indev_set_user_data(indev, this);

        lv_tick_set_cb(_tick_get);
    }

    // Access the touch driver to tune calibration, e.g. from setup():
    //   lv_setup.touch().setCalibration(xMin, xMax, yMin, yMax);
    chipguy_CYD_resistive_touch &touch() { return _touch; }

    // LVGL's two render buffers (DMA-capable, RGB565, drawBufferBytes() each).
    // A sketch that sometimes draws to the panel itself, with
    // display.flushRegion() while LVGL is not rendering, can use these rather
    // than allocate its own.  Call display.waitFlush() first: LVGL's last
    // region may still be going out.
    uint16_t *drawBuffer(int i) { return _buf[i & 1]; }
    size_t drawBufferBytes() const { return _bufBytes; }

private:
    uint16_t *_buf[2] = {nullptr, nullptr};
    size_t _bufBytes = 0;
    chipguy_CYD_resistive_touch _touch;

    static uint32_t _tick_get(void) { return millis(); }

    static void _disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *pixelmap) {
        // flushRegion byte-swaps in place, waits for the previous region's DMA,
        // then QUEUES this region's DMA and returns before it finishes.  Calling
        // flush_ready now lets LVGL render the next region into the other buffer
        // while this one transmits; the driver serializes the bus internally.
        display.flushRegion(area->x1, area->y1, area->x2, area->y2,
                            (uint16_t *)pixelmap);
        lv_display_flush_ready(disp);
    }

    static void _touchpad_read(lv_indev_t *indev, lv_indev_data_t *data) {
        auto *self = (lv_setup_class *)lv_indev_get_user_data(indev);
        uint16_t x, y;
        if (self->_touch.read(x, y)) {
            data->point.x = x;
            data->point.y = y;
            data->state = LV_INDEV_STATE_PRESSED;
        } else {
            data->state = LV_INDEV_STATE_RELEASED;
        }
    }
};

static lv_setup_class lv_setup;
