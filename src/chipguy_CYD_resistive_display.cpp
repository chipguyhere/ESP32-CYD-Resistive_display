/*
 * ILI9341 / ST7789 / ST7796 SPI display driver for the resistive-touch CYD
 * family (ESP32-2432S028; ESP32-32E 2.8", 3.2", 3.5", 4.0").
 * See chipguy_CYD_resistive_display.h for the wiring and usage overview.
 */

#include "chipguy_CYD_resistive_display.h"
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp32-hal-periman.h>

// ── Board pin assignments (fixed on every board) ───────────────────────────
#define CYDR_TFT_MOSI  13
#define CYDR_TFT_SCLK  14
#define CYDR_TFT_CS    15
#define CYDR_TFT_DC     2
#define CYDR_TFT_MISO  12   // panel SDO; on shared-touch boards also the XPT2046's DOUT
#define CYDR_TFT_BL_OWN    21   // backlight on own-touch boards, active HIGH
#define CYDR_TFT_BL_SHARED 27   // backlight on shared-touch boards, active HIGH
#define CYDR_TOUCH_CS  33   // held HIGH during detection so the XPT2046 can't drive GPIO12
// No reset GPIO: holding GPIO12 LOW resets neither 2.8" panel (bench, 2026-10-10),
// and the 3.5"/4.0" ties panel reset to EN, so the init sequences start with a
// software reset.

// The panels take 40 MHz comfortably over the short CYD traces.  On the
// shared-touch boards the touch device adds itself to this bus at a much slower clock;
// the SPI master driver re-clocks between them.
#define CYDR_TFT_SPI_HZ  40000000

// The ILI9341 and ST7796 panels are BGR with the same column order; the ST7789
// panels are RGB with columns the other way.  setRotation() builds
// the ILI9341/ST7796 MADCTL and, for the ST7789, clears BGR and flips MX.
// Rotation bits (MY/MX/MV) are OR'd in per rotation.
#define CYDR_MADCTL_BGR  0x08
#define ILI9341_MADCTL_MY  0x80
#define ILI9341_MADCTL_MX  0x40
#define ILI9341_MADCTL_MV  0x20

static cydr_board_t _cydr_activeBoard = CYDR_BOARD_UNKNOWN;
cydr_board_t cydr_activeBoard() { return _cydr_activeBoard; }
static cydr_panel_t _cydr_activePanel = CYDR_PANEL_AUTO;
cydr_panel_t cydr_activePanel() { return _cydr_activePanel; }

// CS is driven by the SPI peripheral (hardware CS), but DC is a plain GPIO.
// The driver calls this just before each transaction goes on the bus — for
// queued transactions that's in ISR context — and we set DC from the
// transaction's `user` field: 0 = command, 1 = data.
static void _cydr_spi_pre_cb(spi_transaction_t *t) {
    gpio_set_level((gpio_num_t)CYDR_TFT_DC, (int)(intptr_t)t->user);
}

chipguy_CYD_resistive_display::chipguy_CYD_resistive_display() {}

// ── Low-level SPI helpers ──────────────────────────────────────────────────
// DC and CS are handled by the pre-callback and hardware CS respectively, so
// these just submit the bytes.  All command/parameter traffic is synchronous
// (polling); only the pixel blit is queued asynchronously (see flushRegion).

void chipguy_CYD_resistive_display::_cmd(uint8_t cmd) {
    if (!_spi) return;
    spi_transaction_t t = {};
    t.length = 8;
    t.tx_data[0] = cmd;
    t.flags = SPI_TRANS_USE_TXDATA;
    t.user = (void *)0;          // DC = command
    spi_device_polling_transmit(_spi, &t);
}

void chipguy_CYD_resistive_display::_data(const uint8_t *data, size_t len) {
    if (!_spi || len == 0) return;
    spi_transaction_t t = {};
    t.length = len * 8;
    t.tx_buffer = data;
    t.user = (void *)1;          // DC = data
    spi_device_polling_transmit(_spi, &t);
}

void chipguy_CYD_resistive_display::_data8(uint8_t val) {
    _data(&val, 1);
}

// ── Board detection (passive) ──────────────────────────────────────────────
// Each board holds its backlight driver's input LOW with an external
// pull-down that overpowers the ESP32's internal pull-up: GPIO21 on the
// own-touch boards (2432S028, 32E 2.8"), GPIO27 on the shared-touch boards
// (32E 3.2", 3.5", 4.0").  On the other wiring the same pin floats (32E 2.8":
// GPIO27 is the SPI header's CS) or is pulled up (2432S028: GPIO27; 3.2":
// GPIO21).  Measured on one board of each, 2026-10-13: the pulled-down pin
// reads 0.13 V with the internal pull-up on.

static int _cydr_readWithPullup(int pin) {
    gpio_reset_pin((gpio_num_t)pin);
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)pin, GPIO_PULLUP_ONLY);
    delayMicroseconds(200);
    int v = gpio_get_level((gpio_num_t)pin);
    gpio_reset_pin((gpio_num_t)pin);
    return v;
}

cydr_board_t cydr_detectBoardPassive() {
    bool low21 = _cydr_readWithPullup(CYDR_TFT_BL_OWN) == 0;
    bool low27 = _cydr_readWithPullup(CYDR_TFT_BL_SHARED) == 0;
    if (low21 && !low27) return CYDR_BOARD_OWN_TOUCH;
    if (low27 && !low21) return CYDR_BOARD_SHARED_TOUCH;
    return CYDR_BOARD_UNKNOWN;
}

// ── Red LED pin (GPIO4 vs GPIO22) ──────────────────────────────────────────
// With the internal pull-down on, GPIO4 reads ~1.64 V on the 2432S028 (the
// red LED's diode drop from 3.3 V) and ~2.50 V on the 32E boards (the amp
// shutdown pull-up against the internal pull-down); 2.0 V splits them.
// See README-board-detection.md.  A failed ADC2 read (WiFi running) comes
// back as 0, so anything under 500 mV is treated as no reading.

static int8_t   _cydr_redLed = -1;      // latched answer; -1 = not yet measured
static uint32_t _cydr_redLedMv = 0;

static uint32_t _cydr_gpio4PulldownMillivolts() {
    analogReadMilliVolts(4);                       // first read attaches the pin to the ADC
    gpio_set_pull_mode(GPIO_NUM_4, GPIO_PULLDOWN_ONLY);
    delay(2);
    uint32_t mv = 0;
    for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(4);
    gpio_set_pull_mode(GPIO_NUM_4, GPIO_FLOATING);
    perimanClearPinBus(4);                         // release ADC2 before WiFi wants it
    return mv / 8;
}

int8_t cydr_redLedPin() {
    if (_cydr_redLed >= 0) return _cydr_redLed;
    // The 2432S028 has the own-pins touch wiring: a shared-bus board is a 32E
    // without asking.
    if (_cydr_activeBoard == CYDR_BOARD_SHARED_TOUCH) return _cydr_redLed = 22;

    uint32_t mv = _cydr_gpio4PulldownMillivolts();
    if (mv >= 500) {
        _cydr_redLedMv = mv;
        return _cydr_redLed = mv < 2000 ? 4 : 22;
    }
    // No ADC reading: WiFi already holds ADC2.  Not latched, so a later call
    // (after WiFi is stopped) can still measure.
    Serial.printf("cydr display: GPIO4 ADC read failed (WiFi on?); "
                  "red LED pin unknown\n");
    return -1;
}

uint32_t cydr_redLedProbeMillivolts() { return _cydr_redLedMv; }

// ── Panel detection ────────────────────────────────────────────────────────
// Runs before the SPI peripheral owns the pins, so it bit-bangs (~200 kHz,
// mode 0).  Every panel wires its SDO to GPIO12.  The XPT2046 also drives
// GPIO12 on the shared-touch boards, so its CS (GPIO33) is held HIGH throughout.
//
// Sampling is at the END of the clock's low phase, just before the rising
// edge: the ST7796 changes SDO on the rising edge, so sampling just after it
// smears 77 96 into 7F DF, while the ILI9341 and the 2.8" ST7789-like panel
// read the same either way (bench, 2026-10-13).
//
//   ILI9341: its ID only comes back through the undocumented index register:
//            write 0xD9 with 0x10+n, then a one-byte 0xD3 read returns ID
//            byte n (00 93 41 for n=1..3) — the readcommand8() method
//            Adafruit and TFT_eSPI use.
//   ST7796:  answers 0xD3 directly: one dummy bit, then 00 77 96.
//   ST7789:  has no 0xD3, leaves SDO high-Z, and GPIO12 just follows its pull;
//            reading under both pulls separates "the panel answered" from "the
//            pin floated".  As a fallback, RDDID 0x04 — 85 85 52 per the
//            datasheet, 81 81 B3 on the board probed (bench, 2026-10-11).
//            The ILI9341s return 00 00 00 there (no module ID programmed).
//
// The passive board check (backlight pull-downs) is the cross-check: a panel
// that answers with an ID wins, but a silent or unrecognised one is resolved
// by the board.  The wiring itself always comes from the board check, so a
// 3.5"/4.0" whose panel doesn't answer still gets the right pins and is
// assumed to be an ST7796.  A silent ST7789 is recognised only on own-touch
// wiring; the 3.2"'s answers RDDID (bench, 2026-10-13).

static uint8_t _cydr_bbXfer(uint8_t out) {
    uint8_t in = 0;
    for (int i = 7; i >= 0; i--) {
        gpio_set_level((gpio_num_t)CYDR_TFT_MOSI, (out >> i) & 1);
        delayMicroseconds(2);
        in = (in << 1) | gpio_get_level((gpio_num_t)CYDR_TFT_MISO);  // end of low phase
        gpio_set_level((gpio_num_t)CYDR_TFT_SCLK, 1);
        delayMicroseconds(2);
        gpio_set_level((gpio_num_t)CYDR_TFT_SCLK, 0);
    }
    return in;
}

static void _cydr_bbDummyClock() {
    delayMicroseconds(2);
    gpio_set_level((gpio_num_t)CYDR_TFT_SCLK, 1);
    delayMicroseconds(2);
    gpio_set_level((gpio_num_t)CYDR_TFT_SCLK, 0);
}

// Write the 0xD9 index, then read one byte of 0xD3.
static uint8_t _cydr_bbReadIdx(uint8_t idx) {
    gpio_set_level((gpio_num_t)CYDR_TFT_CS, 0);
    gpio_set_level((gpio_num_t)CYDR_TFT_DC, 0);
    _cydr_bbXfer(0xD9);
    gpio_set_level((gpio_num_t)CYDR_TFT_DC, 1);
    _cydr_bbXfer(0x10 + idx);
    gpio_set_level((gpio_num_t)CYDR_TFT_CS, 1);
    delayMicroseconds(5);
    gpio_set_level((gpio_num_t)CYDR_TFT_CS, 0);
    gpio_set_level((gpio_num_t)CYDR_TFT_DC, 0);
    _cydr_bbXfer(0xD3);
    gpio_set_level((gpio_num_t)CYDR_TFT_DC, 1);
    uint8_t v = _cydr_bbXfer(0x00);
    gpio_set_level((gpio_num_t)CYDR_TFT_CS, 1);
    delayMicroseconds(5);
    return v;
}

// A command, one dummy clock, then three bytes: RDDID 0x04 and the ST7796's 0xD3.
static void _cydr_bbRead3(uint8_t cmd, uint8_t out[3]) {
    gpio_set_level((gpio_num_t)CYDR_TFT_CS, 0);
    gpio_set_level((gpio_num_t)CYDR_TFT_DC, 0);
    _cydr_bbXfer(cmd);
    gpio_set_level((gpio_num_t)CYDR_TFT_DC, 1);
    _cydr_bbDummyClock();
    for (int i = 0; i < 3; i++) out[i] = _cydr_bbXfer(0x00);
    gpio_set_level((gpio_num_t)CYDR_TFT_CS, 1);
    delayMicroseconds(5);
}

cydr_panel_t chipguy_CYD_resistive_display::_detectPanel(cydr_board_t boardHint) {
    gpio_reset_pin((gpio_num_t)CYDR_TOUCH_CS);     // GPIO33 is an RTC pin: make it plain GPIO
    gpio_set_level((gpio_num_t)CYDR_TOUCH_CS, 1);
    gpio_set_direction((gpio_num_t)CYDR_TOUCH_CS, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)CYDR_TFT_MOSI, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)CYDR_TFT_SCLK, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)CYDR_TFT_CS, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)CYDR_TFT_SCLK, 0);
    gpio_set_level((gpio_num_t)CYDR_TFT_CS, 1);
    gpio_set_direction((gpio_num_t)CYDR_TFT_MISO, GPIO_MODE_INPUT);

    uint8_t id[2][2];
    uint8_t d3[2][3];
    uint8_t rddid[2][3];
    const gpio_pull_mode_t pulls[2] = {GPIO_PULLUP_ONLY, GPIO_PULLDOWN_ONLY};
    for (int p = 0; p < 2; p++) {
        gpio_set_pull_mode((gpio_num_t)CYDR_TFT_MISO, pulls[p]);
        delayMicroseconds(50);
        id[p][0] = _cydr_bbReadIdx(2);
        id[p][1] = _cydr_bbReadIdx(3);
        _cydr_bbRead3(0xD3, d3[p]);
        _cydr_bbRead3(0x04, rddid[p]);
    }
    // Leave GPIO12 as the chip's reset default: an input with pull-down.
    // It is the MTDI strapping pin, and the panel only drives it during reads.

    if (id[0][0] == 0x93 && id[0][1] == 0x41 && id[1][0] == 0x93 && id[1][1] == 0x41)
        return CYDR_PANEL_ILI9341;
    if (d3[0][1] == 0x77 && d3[0][2] == 0x96 && d3[1][1] == 0x77 && d3[1][2] == 0x96)
        return CYDR_PANEL_ST7796;
    // RDDID bytes 2 and 3 (version, driver ID) under both pulls; byte 1 is the
    // module vendor and varies.
    static const uint8_t st7789Ids[][2] = {{0x85, 0x52}, {0x81, 0xB3}};
    for (const auto &st : st7789Ids) {
        if (rddid[0][1] == st[0] && rddid[0][2] == st[1] &&
            rddid[1][1] == st[0] && rddid[1][2] == st[1])
            return CYDR_PANEL_ST7789;
    }
    bool silent = id[0][0] == 0xFF && id[0][1] == 0xFF && id[1][0] == 0x00 && id[1][1] == 0x00;
    if (silent && boardHint != CYDR_BOARD_SHARED_TOUCH)
        return CYDR_PANEL_ST7789;      // the 2.8" ST7789 answers nothing to 0xD3

    // Nothing recognised: go by the board.
    cydr_panel_t guess = boardHint == CYDR_BOARD_SHARED_TOUCH ? CYDR_PANEL_ST7796
                                                             : CYDR_PANEL_ILI9341;
    Serial.printf("cydr display: unrecognised panel (idx %02X %02X / %02X %02X, "
                  "D3 %02X %02X %02X, RDDID %02X %02X %02X); assuming %s\n",
                  id[0][0], id[0][1], id[1][0], id[1][1],
                  d3[0][0], d3[0][1], d3[0][2], rddid[0][0], rddid[0][1], rddid[0][2],
                  guess == CYDR_PANEL_ST7796 ? "ST7796" : "ILI9341");
    return guess;
}

const char *chipguy_CYD_resistive_display::panelName() const {
    switch (_panel) {
        case CYDR_PANEL_ILI9341: return "ILI9341";
        case CYDR_PANEL_ST7789:  return "ST7789";
        case CYDR_PANEL_ST7796:  return "ST7796";
        default:                   return "unknown";
    }
}

// ── Power-on init sequences ────────────────────────────────────────────────
// MADCTL is written here as a placeholder and then re-issued by setRotation().

// Standard ILI9341 startup (gamma, power, frame rate).
void chipguy_CYD_resistive_display::_initILI9341() {
    _cmd(0x01);                            // Software reset
    delay(150);
    _cmd(0xEF); { const uint8_t d[] = {0x03, 0x80, 0x02}; _data(d, 3); }
    _cmd(0xCF); { const uint8_t d[] = {0x00, 0xC1, 0x30}; _data(d, 3); }
    _cmd(0xED); { const uint8_t d[] = {0x64, 0x03, 0x12, 0x81}; _data(d, 4); }
    _cmd(0xE8); { const uint8_t d[] = {0x85, 0x00, 0x78}; _data(d, 3); }
    _cmd(0xCB); { const uint8_t d[] = {0x39, 0x2C, 0x00, 0x34, 0x02}; _data(d, 5); }
    _cmd(0xF7); _data8(0x20);
    _cmd(0xEA); { const uint8_t d[] = {0x00, 0x00}; _data(d, 2); }
    _cmd(0xC0); _data8(0x23);              // Power control 1
    _cmd(0xC1); _data8(0x10);              // Power control 2
    _cmd(0xC5); { const uint8_t d[] = {0x3E, 0x28}; _data(d, 2); }  // VCOM 1
    _cmd(0xC7); _data8(0x86);              // VCOM 2
    _cmd(0x36); _data8(CYDR_MADCTL_BGR);    // MADCTL (overwritten by setRotation)
    _cmd(0x3A); _data8(0x55);              // Pixel format: 16-bit RGB565
    _cmd(0xB1); { const uint8_t d[] = {0x00, 0x18}; _data(d, 2); }  // Frame rate
    _cmd(0xB6); { const uint8_t d[] = {0x08, 0x82, 0x27}; _data(d, 3); } // Display func
    _cmd(0xF2); _data8(0x00);              // 3Gamma disable
    _cmd(0x26); _data8(0x01);              // Gamma curve selected
    _cmd(0xE0); { const uint8_t d[] = {0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E,
                  0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00}; _data(d, 15); }
    _cmd(0xE1); { const uint8_t d[] = {0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31,
                  0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F}; _data(d, 15); }
    _cmd(0x11);                            // Sleep out
    delay(120);
    _cmd(0x29);                            // Display on
    delay(20);
}

// ST7789: the power-on defaults are right for these panels, so only the pixel
// format and display state are set.  The 2.8" panel shows correct colors with
// inversion OFF, unlike most ST7789 IPS modules; the 3.2" (shared-touch
// wiring) needs it ON — without INVON it shows inverse video (bench, 2026-10-13).
void chipguy_CYD_resistive_display::_initST7789() {
    _cmd(0x01);                            // Software reset
    delay(150);
    _cmd(0x11);                            // Sleep out
    delay(120);
    _cmd(0x3A); _data8(0x55);              // Pixel format: 16-bit RGB565
    _cmd(0x36); _data8(0x00);              // MADCTL (overwritten by setRotation)
    _cmd(_board == CYDR_BOARD_SHARED_TOUCH ? 0x21 : 0x20);   // Inversion on (3.2") / off (2.8")
    _cmd(0x13);                            // Normal display mode
    _cmd(0x29);                            // Display on
    delay(20);
}

// Standard ST7796 startup (the values TFT_eSPI's ST7796 profile uses), as in the
// earlier chipguy_ESP32-32E_CYD_4inch_display.
void chipguy_CYD_resistive_display::_initST7796() {
    _cmd(0x01);                            // Software reset
    delay(120);
    _cmd(0x11);                            // Sleep out
    delay(120);

    _cmd(0xF0); _data8(0xC3);              // Command Set Control — enable ext command 2 part I
    _cmd(0xF0); _data8(0x96);              // Command Set Control — enable ext command 2 part II

    _cmd(0x36); _data8(CYDR_MADCTL_BGR);    // MADCTL (overwritten by setRotation)
    _cmd(0x3A); _data8(0x55);              // Pixel format: 16-bit RGB565

    _cmd(0xB4); _data8(0x01);              // Column inversion
    _cmd(0xB6); { const uint8_t d[] = {0x80, 0x02, 0x3B}; _data(d, 3); }   // Display function control
    _cmd(0xE8); { const uint8_t d[] = {0x40, 0x8A, 0x00, 0x00, 0x29,
                  0x19, 0xA5, 0x33}; _data(d, 8); }                        // Display output ctrl adjust
    _cmd(0xC1); _data8(0x06);              // Power control 2
    _cmd(0xC2); _data8(0xA7);              // Power control 3
    _cmd(0xC5); _data8(0x18);              // VCOM control
    delay(120);

    _cmd(0xE0); { const uint8_t d[] = {0xF0, 0x09, 0x0B, 0x06, 0x04, 0x15, 0x2F,
                  0x54, 0x42, 0x3C, 0x17, 0x14, 0x18, 0x1B}; _data(d, 14); } // Positive gamma
    _cmd(0xE1); { const uint8_t d[] = {0xE0, 0x09, 0x0B, 0x06, 0x04, 0x03, 0x2B,
                  0x43, 0x42, 0x3B, 0x16, 0x14, 0x17, 0x1B}; _data(d, 14); } // Negative gamma
    delay(120);

    _cmd(0xF0); _data8(0x3C);              // Command Set Control — disable ext command 2 part I
    _cmd(0xF0); _data8(0x69);              // Command Set Control — disable ext command 2 part II
    delay(120);

    _cmd(0x29);                            // Display on
    delay(20);
}

void chipguy_CYD_resistive_display::_setAddrWindow(int x0, int y0, int x1, int y1) {
    uint8_t col[4] = {
        (uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF),
        (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF)
    };
    _cmd(0x2A);              // CASET — Column Address Set
    _data(col, 4);

    uint8_t row[4] = {
        (uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF),
        (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF)
    };
    _cmd(0x2B);              // RASET — Row Address Set
    _data(row, 4);

    _cmd(0x2C);              // RAMWR — Memory Write
}

// Synchronous (blocking) pixel push, used by fillScreen().  The interactive
// path uses the queued/async flushRegion() instead.
void chipguy_CYD_resistive_display::_pushPixels(const uint8_t *data, size_t bytes) {
    if (!_spi || bytes == 0) return;
    spi_transaction_t t = {};
    t.length = bytes * 8;
    t.tx_buffer = data;
    t.user = (void *)1;          // DC = data
    spi_device_polling_transmit(_spi, &t);
}

// Block until the in-flight async pixel DMA (if any) has completed, so the bus
// is idle before we touch it directly (or before the touch driver uses it).
void chipguy_CYD_resistive_display::_waitPixelDone() {
    if (_pixelBusy) {
        spi_transaction_t *result = nullptr;
        spi_device_get_trans_result(_spi, &result, portMAX_DELAY);
        _pixelBusy = false;
    }
}

// ── Public API ─────────────────────────────────────────────────────────────

bool chipguy_CYD_resistive_display::begin(uint16_t rotation) {
    // DC is driven by the pre-transaction callback; configure it as output.
    // CS is owned by the SPI peripheral (hardware CS) once the bus is up.
    gpio_set_direction((gpio_num_t)CYDR_TFT_DC, GPIO_MODE_OUTPUT);

    // Which board, then which panel.  Both must run before spi_bus_initialize()
    // takes the pins, and the board check before the backlight is driven.
    cydr_board_t hint = cydr_detectBoardPassive();
    if (_panel == CYDR_PANEL_AUTO) _panel = _detectPanel(hint);
    // The wiring comes from the backlight pins; only if they are
    // inconclusive is it guessed from the panel.
    _board = hint;
    if (_board == CYDR_BOARD_UNKNOWN) {
        _board = _panel == CYDR_PANEL_ST7796 ? CYDR_BOARD_SHARED_TOUCH
                                               : CYDR_BOARD_OWN_TOUCH;
        Serial.printf("cydr display: backlight pins inconclusive; assuming "
                      "backlight GPIO%d from the %s panel\n",
                      _board == CYDR_BOARD_SHARED_TOUCH ? CYDR_TFT_BL_SHARED : CYDR_TFT_BL_OWN,
                      panelName());
    }
    _cydr_activeBoard = _board;
    _cydr_activePanel = _panel;
    cydr_redLedPin();         // latch it now, while ADC2 is still ours (before WiFi)

    if (_panel == CYDR_PANEL_ST7796) { _nativeW = 320; _nativeH = 480; }
    else                               { _nativeW = 240; _nativeH = 320; }
    _blPin = _board == CYDR_BOARD_SHARED_TOUCH ? CYDR_TFT_BL_SHARED : CYDR_TFT_BL_OWN;

    // Register the SPI bus with the ESP-IDF master driver.  MISO is on the
    // bus on every board: the shared-touch boards' XPT2046 returns its samples
    // on it, and it is only ever an input here, so GPIO12's strapping role is
    // unaffected.
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = CYDR_TFT_MOSI;
    buscfg.miso_io_num = CYDR_TFT_MISO;
    buscfg.sclk_io_num = CYDR_TFT_SCLK;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    // Largest single SPI transfer we size DMA descriptors for: a full screen,
    // so any partial flush region fits in one queued transaction.
    buscfg.max_transfer_sz = (int)_nativeW * _nativeH * 2;

    esp_err_t err = spi_bus_initialize(CYDR_TFT_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        Serial.printf("cydr display: spi_bus_initialize failed: 0x%x\n", err);
        return false;
    }

    spi_device_interface_config_t devcfg = {};
    devcfg.clock_speed_hz = CYDR_TFT_SPI_HZ;
    devcfg.mode = 0;
    devcfg.spics_io_num = CYDR_TFT_CS;   // hardware-managed CS
    devcfg.queue_size = 1;              // one async pixel transfer in flight
    devcfg.pre_cb = _cydr_spi_pre_cb;    // sets DC from each transaction's user flag
    devcfg.flags = SPI_DEVICE_NO_DUMMY;

    err = spi_bus_add_device(CYDR_TFT_SPI_HOST, &devcfg, &_spi);
    if (err != ESP_OK) {
        Serial.printf("cydr display: spi_bus_add_device failed: 0x%x\n", err);
        _spi = nullptr;
        return false;
    }

    switch (_panel) {
        case CYDR_PANEL_ST7789: _initST7789(); break;
        case CYDR_PANEL_ST7796: _initST7796(); break;
        default:                  _initILI9341(); break;
    }
    setRotation(rotation);

    // Backlight on at full brightness.
    setBacklight(100);

    return true;
}

void chipguy_CYD_resistive_display::setRotation(uint16_t rotation) {
    _waitPixelDone();           // bus must be idle before we send a command
    _rotation = cydr_normalize_rotation(rotation);
    uint8_t madctl = CYDR_MADCTL_BGR;

    switch (_rotation) {
        case 0:  // portrait, native
            madctl |= ILI9341_MADCTL_MX;
            _w = _nativeW;
            _h = _nativeH;
            break;
        case 1:  // landscape
            madctl |= ILI9341_MADCTL_MV;
            _w = _nativeH;
            _h = _nativeW;
            break;
        case 2:  // portrait flipped
            madctl |= ILI9341_MADCTL_MY;
            _w = _nativeW;
            _h = _nativeH;
            break;
        case 3:  // landscape flipped
            madctl |= ILI9341_MADCTL_MV | ILI9341_MADCTL_MX | ILI9341_MADCTL_MY;
            _w = _nativeH;
            _h = _nativeW;
            break;
    }
    // The ST7789 panel is RGB, and its columns run the other way.  The ST7796
    // uses the ILI9341 table as is.
    if (_panel == CYDR_PANEL_ST7789)
        madctl = (madctl & ~CYDR_MADCTL_BGR) ^ ILI9341_MADCTL_MX;

    _cmd(0x36);          // MADCTL
    _data8(madctl);
}

void chipguy_CYD_resistive_display::setBacklight(int percentage) {
    if (percentage < 0)   percentage = 0;
    if (percentage > 100) percentage = 100;
    // Active HIGH: higher duty = brighter.
    analogWrite(_blPin, percentage * 255 / 100);
}

// Asynchronous flush.  Returns once this region's pixel DMA has been *queued*,
// not when it finishes — the caller (LVGL via lv_setup) is then free to render
// the next region into the other buffer while this one transmits.  The bus is
// serialized by blocking on the *previous* flush's DMA at the top of this one.
void chipguy_CYD_resistive_display::flushRegion(int x1, int y1, int x2, int y2,
                                              uint16_t *pixels) {
    if (!_spi) return;

    int w = x2 - x1 + 1;
    int h = y2 - y1 + 1;
    size_t count = (size_t)w * h;

    // The swap below mutates THIS region's buffer.  With double buffering LVGL
    // hands us a different buffer than the one the previous flush is still
    // DMA'ing, so the swap safely overlaps that transfer.  Guard the (not
    // expected) case where we're handed the in-flight buffer: wait first.
    if (_pixelBusy && (const void *)pixels == _pixelTrans.tx_buffer) {
        _waitPixelDone();
    }

    // LVGL renders RGB565 little-endian (native ESP32); the panel expects
    // big-endian (MSB first).  Swap in place — overlaps the previous region's
    // in-flight DMA (different buffer) for free.
    for (size_t i = 0; i < count; i++) {
        pixels[i] = __builtin_bswap16(pixels[i]);
    }

    // Now wait for the previous flush's DMA to finish, so the SPI bus is free
    // for this region's address window and pixel transfer.
    _waitPixelDone();

    // Address window (synchronous commands), then queue the pixel blit.
    _setAddrWindow(x1, y1, x2, y2);

    _pixelTrans = {};
    _pixelTrans.length = count * 16;        // bits (2 bytes per pixel)
    _pixelTrans.tx_buffer = pixels;
    _pixelTrans.user = (void *)1;           // DC = data
    if (spi_device_queue_trans(_spi, &_pixelTrans, portMAX_DELAY) == ESP_OK) {
        _pixelBusy = true;
    }
}

void chipguy_CYD_resistive_display::waitFlush() {
    _waitPixelDone();
}

void chipguy_CYD_resistive_display::fillScreen(uint16_t color) {
    if (!_spi) return;
    _waitPixelDone();           // drain any queued async flush first
    uint16_t be = __builtin_bswap16(color);

    // One-row buffer pushed for every row.  Must be DMA-capable: the ESP-IDF
    // SPI driver requires DMA memory for transfers larger than the FIFO.
    uint16_t *line = (uint16_t *)heap_caps_malloc((size_t)_w * 2, MALLOC_CAP_DMA);
    if (!line) return;
    for (int i = 0; i < _w; i++) line[i] = be;

    _setAddrWindow(0, 0, _w - 1, _h - 1);
    for (int y = 0; y < _h; y++) {
        _pushPixels((const uint8_t *)line, (size_t)_w * 2);
    }
    heap_caps_free(line);
}
