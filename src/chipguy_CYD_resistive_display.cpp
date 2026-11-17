/*
 * ILI9341 / ST7789 / ST7796 SPI display driver for the resistive-touch CYD
 * family (ESP32-2432S028; ESP32-32E 2.8", 3.2", 3.5", 4.0"; ESP32-024; ESP32-035;
 * ESP32-2432S032 / -C), and the capacitive ESP32-2432S024C, JC2432W328 and
 * ESP32-3248S035C; the ESP32-C5 NM-CYD-C5; and the ESP32-S3 ES3C28P (S3 board
 * identification is in chipguy_CYD_board_ESP32S3.cpp).
 * See chipguy_CYD_resistive_display.h for the wiring and usage overview.
 */

#include "chipguy_CYD_resistive_display.h"
#include "cydr_internal.h"
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp32-hal-periman.h>
#include <Wire.h>
#include <Preferences.h>

// ── Board pin assignments (fixed on every board of a chip) ─────────────────
#if CYDR_CHIP_C5
// NM-CYD-C5: one bus for the panel, the XPT2046 and the TF card.
#define CYDR_TFT_MOSI   7
#define CYDR_TFT_SCLK   6
#define CYDR_TFT_CS    23
#define CYDR_TFT_DC    24
#define CYDR_TFT_MISO   2   // panel SDO, the XPT2046's DOUT and the card's DAT0
#define CYDR_TFT_BL_OWN    25   // the one backlight pin (AO3400 low-side switch), active HIGH
#define CYDR_TFT_BL_SHARED 25
#define CYDR_TOUCH_CS   1   // held HIGH during detection so the XPT2046 can't drive MISO
#define CYDR_TF_CS     10   // the card, likewise (10k pull-up on the board too)
#elif CYDR_CHIP_S3
// ES3C28P: the panel alone on FSPI's IO_MUX pins (so 80 MHz needs no GPIO
// matrix).  Touch is I2C and the card is SDMMC: nothing else on this bus.
#define CYDR_TFT_MOSI  11
#define CYDR_TFT_SCLK  12
#define CYDR_TFT_CS    10
#define CYDR_TFT_DC    46
#define CYDR_TFT_MISO  13   // panel SDO
#define CYDR_TFT_BL_OWN    45   // the one backlight pin (BSS138 low-side switch,
#define CYDR_TFT_BL_SHARED 45   //   10k pull-down), active HIGH
#else
#define CYDR_TFT_MOSI  13
#define CYDR_TFT_SCLK  14
#define CYDR_TFT_CS    15
#define CYDR_TFT_DC     2
#define CYDR_TFT_MISO  12   // panel SDO; on shared-touch boards also the XPT2046's DOUT
#define CYDR_TFT_BL_OWN    21   // backlight on own-touch boards, active HIGH
#define CYDR_TFT_BL_SHARED 27   // backlight on shared-touch boards, active HIGH
#define CYDR_TOUCH_CS  33   // held HIGH during detection so the XPT2046 can't drive GPIO12
#endif
// No reset GPIO: panel reset is tied to EN, so the init sequences start with a
// software reset.

// The panels take 40 MHz comfortably over the short CYD traces.  On the
// shared-touch boards the touch device adds itself to this bus at a much slower clock;
// the SPI master driver re-clocks between them.
#define CYDR_TFT_SPI_HZ  40000000
// The same 40 MHz on the NM-CYD-C5.  80 MHz passed stressTest() on the
// boards tried, but these boards vary, and a beginner has no way to trace a
// garbled screen to the clock.  80 MHz is the SPI-clock override's value 3,
// which TouchCalibrate's speed test saves on a pass.  See
// README-engineering.md, "The SPI clock".

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
static cydr_model_t _cydr_activeModel = CYDR_MODEL_UNKNOWN;
cydr_model_t cydr_activeModel() { return _cydr_activeModel; }

// The touch-I2C / readback lock (see the header), created on first use (a
// function-local static: initialised once, even from two tasks at once).
static SemaphoreHandle_t _cydr_touchI2cMutex() {
    static StaticSemaphore_t buf;
    static SemaphoreHandle_t m = xSemaphoreCreateMutexStatic(&buf);
    return m;
}
void cydr_touchI2cLock()   { xSemaphoreTake(_cydr_touchI2cMutex(), portMAX_DELAY); }
void cydr_touchI2cUnlock() { xSemaphoreGive(_cydr_touchI2cMutex()); }
static cydr_readback_t _cydr_activeReadback = CYDR_READBACK_NONE;
static cydr_board_t _cydr_activeTouchBus = CYDR_BOARD_UNKNOWN;
cydr_board_t cydr_activeTouchBus() { return _cydr_activeTouchBus; }
static cydr_touch_chip_t _cydr_activeTouchChip = CYDR_TOUCH_XPT2046;
cydr_touch_chip_t cydr_activeTouchChip() { return _cydr_activeTouchChip; }
static uint8_t _cydr_activeTouchAddr = 0;
uint8_t cydr_activeTouchAddress() { return _cydr_activeTouchAddr; }

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
// (32E 3.2", 3.5", 4.0", ESP32-024).  On the other wiring the same pin floats
// (32E 2.8": GPIO27 is the SPI header's CS; 3.2": GPIO21) or is pulled up
// (2432S028: GPIO27; ESP32-024: GPIO21).  Measured on one board of each,
// 2026-10-13: the pulled-down pin reads 0.13 V with the internal pull-up on
// (0.77 V on the ESP32-024, 2026-10-18 — a weaker pull-down, still a clear LOW).

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
#if CYDR_CHIP_C5
    return CYDR_BOARD_SHARED_TOUCH;          // the NM-CYD-C5: named by the build
#elif CYDR_CHIP_S3
    // One backlight pin, GPIO45, with a 10k pull-down on the ES3C28P.  The
    // "wiring" only says which backlight pin; there is no XPT2046.
    return _cydr_readWithPullup(CYDR_TFT_BL_SHARED) == 0 ? CYDR_BOARD_SHARED_TOUCH
                                                         : CYDR_BOARD_UNKNOWN;
#endif
    bool low21 = _cydr_readWithPullup(CYDR_TFT_BL_OWN) == 0;
    bool low27 = _cydr_readWithPullup(CYDR_TFT_BL_SHARED) == 0;
    if (low21 && !low27) return CYDR_BOARD_OWN_TOUCH;
    if (low27 && !low21) return CYDR_BOARD_SHARED_TOUCH;
    return CYDR_BOARD_UNKNOWN;
}

// ── Red LED pin (GPIO4 vs GPIO22) ──────────────────────────────────────────
// With the internal pull-down on, GPIO4 reads ~1.64 V on the 2432S028 and
// ~1.60 V on the ESP32-024 (the red LED's diode drop from 3.3 V) and ~2.50 V
// on the 32E boards (the amp shutdown pull-up against the internal
// pull-down); 2.0 V splits them.  Measured on every wiring: the ESP32-024 has
// the shared-touch wiring and the red LED on GPIO4.
// See README-board-detection-ESP32.md.  A failed ADC2 read (WiFi running) comes
// back as 0, so anything under 500 mV is treated as no reading.

static int8_t   _cydr_redLed = -1;      // latched answer; -1 = not yet measured
static uint32_t _cydr_redLedMv = 0;

#if CYDR_CHIP_CLASSIC
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

#endif

int8_t cydr_redLedPin() {
#if !CYDR_CHIP_CLASSIC
    return -1;                                // a WS2812 instead: cydr_rgbLedPin()
#else
    if (_cydr_redLed >= 0) return _cydr_redLed;
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
#endif
}

int8_t cydr_rgbLedPin() {
#if CYDR_CHIP_S3
    return _cydr_s3RgbLedPin(_cydr_activeModel);
#else
    return CYDR_CHIP_C5 ? 27 : -1;
#endif
}

uint32_t cydr_redLedProbeMillivolts() { return _cydr_redLedMv; }

int8_t cydr_sdCsPin() {
#if CYDR_CHIP_C5
    return CYDR_TF_CS;          // 10, on the display's bus
#elif CYDR_CHIP_S3
    return -1;                  // SDMMC: no CS
#else
    return 5;                   // VSPI, its own bus
#endif
}

// Green / blue: 16 / 17 everywhere but the ESP32-024 and ESP32-035 (17 / 16;
// seen against the screen on all six boards, 2026-10-18; the -035 2026-10-23).  An unknown model is still answerable
// on own-touch wiring, where both candidates (2432S028, 32E 2.8") use 16 / 17.
static uint8_t _cydr_gbOverride = CYDR_OVR_AUTO;
static bool _cydr_gbKnown(bool &swapped) {
    if (!CYDR_CHIP_CLASSIC) return false;     // a WS2812: no separate green / blue pins
    if (_cydr_gbOverride) { swapped = _cydr_gbOverride == 2; return true; }
    swapped = _cydr_activeModel == CYDR_MODEL_024 || _cydr_activeModel == CYDR_MODEL_035;
    // The 2432S032 / -C are 16 / 17 (seen on the -C, 2026-10-29), and so are
    // the 2432S024C and JC2432W328 (2026-10-31).
    if (_cydr_activeModel != CYDR_MODEL_UNKNOWN) return true;
    return _cydr_activeBoard == CYDR_BOARD_OWN_TOUCH;
}
int8_t cydr_greenLedPin() { bool sw; return _cydr_gbKnown(sw) ? (sw ? 17 : 16) : -1; }
int8_t cydr_blueLedPin()  { bool sw; return _cydr_gbKnown(sw) ? (sw ? 16 : 17) : -1; }

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

// The pin replies are read on: GPIO12 (the panel's SDO) normally.  For the
// SDA fallback it is MOSI, turned around to an input after the command bytes.
static int _cydr_bbRx = CYDR_TFT_MISO;

static uint8_t _cydr_bbXfer(uint8_t out) {
    uint8_t in = 0;
    for (int i = 7; i >= 0; i--) {
        gpio_set_level((gpio_num_t)CYDR_TFT_MOSI, (out >> i) & 1);  // ignored while MOSI is an input
        delayMicroseconds(2);
        in = (in << 1) | gpio_get_level((gpio_num_t)_cydr_bbRx);   // end of low phase
        gpio_set_level((gpio_num_t)CYDR_TFT_SCLK, 1);
        delayMicroseconds(2);
        gpio_set_level((gpio_num_t)CYDR_TFT_SCLK, 0);
    }
    return in;
}

// The ESP32-2432S032 leaves the panel's SDO unconnected (R24, the 0R link to
// GPIO12, isn't fitted), so it is read the 3-wire way: after the command byte,
// MOSI is turned around and the ST7789 drives its bidirectional SDA.  RDDID
// returns 85 85 52 under both pulls there (bench, 2026-10-29, resistive and
// capacitive units).  Set by _detectPanel() whenever a panel answers only on
// SDA; with the wiring it names the board (or marks it as an unseen variant).
static bool _cydr_panelSdaOnly = false;

static void _cydr_bbDummyClock() {
    delayMicroseconds(2);
    gpio_set_level((gpio_num_t)CYDR_TFT_SCLK, 1);
    delayMicroseconds(2);
    gpio_set_level((gpio_num_t)CYDR_TFT_SCLK, 0);
}

// After the command (and any parameter) bytes, the read phase.  On SDA the
// ESP32 stops driving MOSI so the panel can.
static void _cydr_bbReadPhase() {
    gpio_set_level((gpio_num_t)CYDR_TFT_DC, 1);
    if (_cydr_bbRx == CYDR_TFT_MOSI)
        gpio_set_direction((gpio_num_t)CYDR_TFT_MOSI, GPIO_MODE_INPUT);
}

static void _cydr_bbEnd() {
    gpio_set_level((gpio_num_t)CYDR_TFT_CS, 1);
    if (_cydr_bbRx == CYDR_TFT_MOSI)
        gpio_set_direction((gpio_num_t)CYDR_TFT_MOSI, GPIO_MODE_OUTPUT);
    delayMicroseconds(5);
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
    _cydr_bbReadPhase();
    uint8_t v = _cydr_bbXfer(0x00);
    _cydr_bbEnd();
    return v;
}

// A command, one dummy clock, then three bytes: RDDID 0x04 and the ST7796's 0xD3.
static void _cydr_bbRead3(uint8_t cmd, uint8_t out[3]) {
    gpio_set_level((gpio_num_t)CYDR_TFT_CS, 0);
    gpio_set_level((gpio_num_t)CYDR_TFT_DC, 0);
    _cydr_bbXfer(cmd);
    _cydr_bbReadPhase();
    _cydr_bbDummyClock();
    for (int i = 0; i < 3; i++) out[i] = _cydr_bbXfer(0x00);
    _cydr_bbEnd();
}

// Every ID read, under the pull-up [0] and the pull-down [1] on the read pin.
struct _cydr_idReads {
    uint8_t id[2][2];       // ILI9341 index reads, ID bytes 2 and 3
    uint8_t d3[2][3];       // 0xD3 (ST7796)
    uint8_t rddid[2][3];    // 0x04 (ST7789)
};

static void _cydr_readIds(int rx, _cydr_idReads &r) {
    static const gpio_pull_mode_t pulls[2] = {GPIO_PULLUP_ONLY, GPIO_PULLDOWN_ONLY};
    _cydr_bbRx = rx;
    for (int p = 0; p < 2; p++) {
        gpio_set_pull_mode((gpio_num_t)rx, pulls[p]);
        delayMicroseconds(50);
        r.id[p][0] = _cydr_bbReadIdx(2);
        r.id[p][1] = _cydr_bbReadIdx(3);
        _cydr_bbRead3(0xD3, r.d3[p]);
        _cydr_bbRead3(0x04, r.rddid[p]);
    }
    _cydr_bbRx = CYDR_TFT_MISO;
}

// An ID that came back the same under both pulls; AUTO if none.  RDDID bytes 2
// and 3 (version, driver ID) for the ST7789; byte 1 is the module vendor and varies.
static cydr_panel_t _cydr_panelFromIds(const _cydr_idReads &r) {
    if (r.id[0][0] == 0x93 && r.id[0][1] == 0x41 && r.id[1][0] == 0x93 && r.id[1][1] == 0x41)
        return CYDR_PANEL_ILI9341;
    if (r.d3[0][1] == 0x77 && r.d3[0][2] == 0x96 && r.d3[1][1] == 0x77 && r.d3[1][2] == 0x96)
        return CYDR_PANEL_ST7796;
    static const uint8_t st7789Ids[][2] = {{0x85, 0x52}, {0x81, 0xB3}};
    for (const auto &st : st7789Ids) {
        if (r.rddid[0][1] == st[0] && r.rddid[0][2] == st[1] &&
            r.rddid[1][1] == st[0] && r.rddid[1][2] == st[1])
            return CYDR_PANEL_ST7789;
    }
    return CYDR_PANEL_AUTO;
}

// True if every byte just followed the pull: FF under the pull-up, 00 under
// the pull-down -- nothing drove the pin.
static bool _cydr_followedPull(const _cydr_idReads &r) {
    for (int p = 0; p < 2; p++) {
        uint8_t want = p == 0 ? 0xFF : 0x00;
        if (r.id[p][0] != want || r.id[p][1] != want) return false;
        for (int i = 0; i < 3; i++)
            if (r.d3[p][i] != want || r.rddid[p][i] != want) return false;
    }
    return true;
}

cydr_panel_t chipguy_CYD_resistive_display::_detectPanel(cydr_board_t boardHint) {
#ifdef CYDR_TOUCH_CS
    gpio_reset_pin((gpio_num_t)CYDR_TOUCH_CS);     // GPIO33 is an RTC pin: make it plain GPIO
    gpio_set_level((gpio_num_t)CYDR_TOUCH_CS, 1);
    gpio_set_direction((gpio_num_t)CYDR_TOUCH_CS, GPIO_MODE_OUTPUT);
#endif
#ifdef CYDR_TF_CS
    // The card shares MISO on the C5 board: keep it deselected.  Left driven
    // HIGH; an SD driver that mounts the card later takes the pin over.
    gpio_reset_pin((gpio_num_t)CYDR_TF_CS);
    gpio_set_level((gpio_num_t)CYDR_TF_CS, 1);
    gpio_set_direction((gpio_num_t)CYDR_TF_CS, GPIO_MODE_OUTPUT);
#endif
    gpio_set_direction((gpio_num_t)CYDR_TFT_MOSI, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)CYDR_TFT_SCLK, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)CYDR_TFT_CS, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)CYDR_TFT_SCLK, 0);
    gpio_set_level((gpio_num_t)CYDR_TFT_CS, 1);
    gpio_set_direction((gpio_num_t)CYDR_TFT_MISO, GPIO_MODE_INPUT);

    _cydr_idReads r;
    _cydr_readIds(CYDR_TFT_MISO, r);
    // Leave GPIO12 as the chip's reset default: an input with pull-down.
    // It is the MTDI strapping pin, and the panel only drives it during reads.
    cydr_panel_t found = _cydr_panelFromIds(r);
    if (found != CYDR_PANEL_AUTO) return found;

    // Nothing drove GPIO12 at all: the panel's SDO may not be wired.  Ask
    // again with the replies read back on SDA (the 2432S032's panel; any panel
    // in a bidirectional-SDA serial mode would answer the same way).
    if (_cydr_followedPull(r)) {
        _cydr_idReads s;
        _cydr_readIds(CYDR_TFT_MOSI, s);
        gpio_set_pull_mode((gpio_num_t)CYDR_TFT_MOSI, GPIO_FLOATING);
        found = _cydr_panelFromIds(s);
        if (found != CYDR_PANEL_AUTO) {
            _cydr_panelSdaOnly = true;
            return found;
        }
        if (!_cydr_followedPull(s))
            Serial.printf("cydr display: panel silent on GPIO12; on SDA it sent "
                          "idx %02X %02X, D3 %02X %02X %02X, RDDID %02X %02X %02X\n",
                          s.id[0][0], s.id[0][1], s.d3[0][0], s.d3[0][1], s.d3[0][2],
                          s.rddid[0][0], s.rddid[0][1], s.rddid[0][2]);
    }
    bool idxSilent = r.id[0][0] == 0xFF && r.id[0][1] == 0xFF &&
                     r.id[1][0] == 0x00 && r.id[1][1] == 0x00;
    _panelGuessed = true;              // from here on the type is inferred
    if (CYDR_CHIP_CLASSIC && idxSilent && boardHint != CYDR_BOARD_SHARED_TOUCH)
        return CYDR_PANEL_ST7789;      // the 2.8" ST7789 answers nothing to 0xD3

    // Nothing recognised: go by the board.  The only S3 board known has an
    // ILI9341.
    cydr_panel_t guess = CYDR_CHIP_S3 ? CYDR_PANEL_ILI9341 :
                         boardHint == CYDR_BOARD_SHARED_TOUCH ? CYDR_PANEL_ST7796
                                                              : CYDR_PANEL_ILI9341;
    Serial.printf("cydr display: unrecognised panel (idx %02X %02X / %02X %02X, "
                  "D3 %02X %02X %02X, RDDID %02X %02X %02X); assuming %s\n",
                  r.id[0][0], r.id[0][1], r.id[1][0], r.id[1][1],
                  r.d3[0][0], r.d3[0][1], r.d3[0][2], r.rddid[0][0], r.rddid[0][1], r.rddid[0][2],
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

const char *chipguy_CYD_resistive_display::modelName(cydr_model_t m, uint8_t sizeTenths) {
    switch (m) {
        case CYDR_MODEL_2432S028:  return "ESP32-2432S028";
        case CYDR_MODEL_32E_28:    return "ESP32-32E 2.8\"";
        case CYDR_MODEL_32E_32:    return "ESP32-32E 3.2\"";
        case CYDR_MODEL_32E_35_40: return sizeTenths == 35 ? "ESP32-32E 3.5\"" :
                                          sizeTenths == 40 ? "ESP32-32E 4.0\"" :
                                                             "ESP32-32E 3.5\"/4.0\"";
        case CYDR_MODEL_024:       return "ESP32-024 2.4\"";
        case CYDR_MODEL_035:       return "ESP32-035 3.5\"";
        case CYDR_MODEL_2432S032:  return "ESP32-2432S032";
        case CYDR_MODEL_2432S032C: return "ESP32-2432S032C";
        case CYDR_MODEL_2432S024C: return "ESP32-2432S024C";
        case CYDR_MODEL_JC2432W328: return "JC2432W328 2.8\"";
        case CYDR_MODEL_3248S035C: return "ESP32-3248S035C";
        case CYDR_MODEL_NM_CYD_C5: return "NM-CYD-C5";
        case CYDR_MODEL_ES3C28P:   return "ES3C28P 2.8\" S3";
        case CYDR_MODEL_ES3N28P:   return "ES3N28P 2.8\" S3";
        default:                   return "unknown";
    }
}

const char *chipguy_CYD_resistive_display::modelName() const {
    return modelName(_model, _sizeTenths);
}

// What a model implies, for the model override (and the size for every model).
struct _cydr_modelFacts {
    cydr_board_t board;          // backlight 21 (OWN) or 27 (SHARED), and the XPT2046 bus
    int8_t redLed;
    cydr_touch_chip_t chip;
    uint8_t sizeTenths;          // 0: not told by the model
    cydr_power_key_t powerKey;   // README-power-ESP32.md; one unit of each observed
};
static _cydr_modelFacts _cydr_factsOf(cydr_model_t m) {
    switch (m) {
        // The ESP32-2432S032 pair came one each way (IP5306 7021.1HA: press
        // twice; 8067.1HW: hold), so that PCB is EITHER; the 2432S024C and
        // JC2432W328 (8052 1HW, 8050 1HA) both held (bench, 2026-10-31).
        case CYDR_MODEL_2432S028:   return {CYDR_BOARD_OWN_TOUCH,    4, CYDR_TOUCH_XPT2046, 28, CYDR_PWRKEY_NONE};
        case CYDR_MODEL_32E_28:     return {CYDR_BOARD_OWN_TOUCH,   22, CYDR_TOUCH_XPT2046, 28, CYDR_PWRKEY_NONE};
        case CYDR_MODEL_32E_32:     return {CYDR_BOARD_SHARED_TOUCH,22, CYDR_TOUCH_XPT2046, 32, CYDR_PWRKEY_NONE};
        case CYDR_MODEL_32E_35_40:  return {CYDR_BOARD_SHARED_TOUCH,22, CYDR_TOUCH_XPT2046, 0,  CYDR_PWRKEY_NONE};
        case CYDR_MODEL_024:        return {CYDR_BOARD_SHARED_TOUCH, 4, CYDR_TOUCH_XPT2046, 24, CYDR_PWRKEY_DOUBLE_PRESS};
        case CYDR_MODEL_035:        return {CYDR_BOARD_SHARED_TOUCH, 4, CYDR_TOUCH_XPT2046, 35, CYDR_PWRKEY_DOUBLE_PRESS};
        case CYDR_MODEL_2432S032:   return {CYDR_BOARD_SHARED_TOUCH, 4, CYDR_TOUCH_XPT2046, 32, CYDR_PWRKEY_EITHER};
        case CYDR_MODEL_2432S032C:  return {CYDR_BOARD_SHARED_TOUCH, 4, CYDR_TOUCH_GT911,   32, CYDR_PWRKEY_EITHER};
        case CYDR_MODEL_2432S024C:  return {CYDR_BOARD_SHARED_TOUCH, 4, CYDR_TOUCH_CST820,  24, CYDR_PWRKEY_LONG_HOLD};
        case CYDR_MODEL_JC2432W328: return {CYDR_BOARD_SHARED_TOUCH, 4, CYDR_TOUCH_CST820,  28, CYDR_PWRKEY_LONG_HOLD};
        // No battery connector, charger or power key (bench, 2026-11-02).
        case CYDR_MODEL_3248S035C:  return {CYDR_BOARD_SHARED_TOUCH, 4, CYDR_TOUCH_GT911,   35, CYDR_PWRKEY_NONE};
        // No battery connector or charger: USB only.
        case CYDR_MODEL_NM_CYD_C5:  return {CYDR_BOARD_SHARED_TOUCH,-1, CYDR_TOUCH_XPT2046, 28, CYDR_PWRKEY_NONE};
        // A battery connector and charger, but no power key: nothing turns
        // the board off on battery (schematic).  "Board" here only names the
        // backlight pin, 45.
        case CYDR_MODEL_ES3C28P:    return {CYDR_BOARD_SHARED_TOUCH,-1, CYDR_TOUCH_FT6336,  28, CYDR_PWRKEY_NONE};
        case CYDR_MODEL_ES3N28P:    return {CYDR_BOARD_SHARED_TOUCH,-1, CYDR_TOUCH_NONE,    28, CYDR_PWRKEY_NONE};
        default:                   return {CYDR_BOARD_UNKNOWN,     -1, CYDR_TOUCH_XPT2046, 0,  CYDR_PWRKEY_UNKNOWN};
    }
}

// Capacitive touch chips on SDA 33 / SCL 32.  The ESP32-2432S032C's GT911 sits
// where the 2432S032 has its XPT2046 (CS on 33); it answers at 0x5D on the
// unit probed (bench, 2026-10-29), and 0x14 is its other strap.  The
// 2432S024C's and JC2432W328's CST820 answers at 0x15 (chip ID 0xB7 at
// register 0xA7; bench, 2026-10-31).  On the resistive boards the probe toggles
// the XPT2046's CS (33) and DIN (32) with its clock idle, which it ignores.
// Neither chip is reset: RST (GPIO25) is left alone -- on the own-touch boards
// GPIO25 is the XPT2046's clock.
//
// A soft restart (ESP.restart()) doesn't reset the touch chip, and the bus
// can come out of it with the chip mid-transfer: the CST820 then ignores its
// address, the board is taken for a resistive one, and the XPT2046 set-up
// that follows drives SDA / SCL as SPI and keeps it that way.  Seen on both
// CST820 boards with the library's own begin() -- every restart on the
// 2432S024C, every other one on the JC2432W328 (bench, 2026-10-31).  So
// each probe first recovers the bus -- SCL clocked until the chip lets go
// of SDA, then a STOP -- and asks a few times.  On the resistive boards that
// is more CS / DIN toggling with the XPT2046's clock idle.
void _cydr_i2cBusRecover(int sdaPin, int sclPin) {
    const gpio_num_t sda = (gpio_num_t)sdaPin, scl = (gpio_num_t)sclPin;
    gpio_reset_pin(sda);               // inputs with pull-ups, off any peripheral
    gpio_reset_pin(scl);
    gpio_set_level(scl, 1);
    gpio_set_direction(scl, GPIO_MODE_INPUT_OUTPUT_OD);
    delayMicroseconds(5);
    for (int i = 0; i < 9 && !gpio_get_level(sda); i++) {
        gpio_set_level(scl, 0); delayMicroseconds(5);
        gpio_set_level(scl, 1); delayMicroseconds(5);
    }
    // STOP: SDA rises while SCL is high.
    gpio_set_level(scl, 0); delayMicroseconds(5);
    gpio_set_level(sda, 0);
    gpio_set_direction(sda, GPIO_MODE_INPUT_OUTPUT_OD);
    delayMicroseconds(5);
    gpio_set_level(scl, 1); delayMicroseconds(5);
    gpio_set_level(sda, 1); delayMicroseconds(5);
    gpio_reset_pin(sda);
    gpio_reset_pin(scl);
}

cydr_touch_chip_t cydr_probeI2cTouch(uint8_t *addr) {
#if CYDR_CHIP_C5
    // No capacitive variant of the NM-CYD-C5 is known, and GPIO32 / 33 don't
    // exist on the C5.
    if (addr) *addr = 0;
    return CYDR_TOUCH_XPT2046;
#elif CYDR_CHIP_S3
    return _cydr_s3ProbeI2c(addr, nullptr);
#else
    cydr_touch_chip_t found = CYDR_TOUCH_XPT2046;
    uint8_t at = 0;
    static const struct { uint8_t a; cydr_touch_chip_t c; } cands[] = {
        {0x15, CYDR_TOUCH_CST820}, {0x5D, CYDR_TOUCH_GT911}, {0x14, CYDR_TOUCH_GT911}};
    for (int attempt = 0; attempt < 3 && !at; attempt++) {
        if (attempt) delay(10);
        _cydr_i2cBusRecover(33, 32);
        Wire1.begin(33, 32, 400000);
        for (const auto &c : cands) {
            Wire1.beginTransmission(c.a);
            if (Wire1.endTransmission() == 0) { found = c.c; at = c.a; break; }
        }
        Wire1.end();
    }
    gpio_reset_pin(GPIO_NUM_33);
    gpio_reset_pin(GPIO_NUM_32);
    if (addr) *addr = at;
    return found;
#endif
}

uint8_t cydr_gt911Address() {
    uint8_t a;
    return cydr_probeI2cTouch(&a) == CYDR_TOUCH_GT911 ? a : 0;
}

static int8_t _cydr_gpio34FromModel(cydr_model_t m);

// The wiring, panel and red LED pin together name the board.  -1 (no GPIO4
// reading) leaves the 240x320 boards unknown; a 320x480 one is taken for the
// 32E 3.5"/4.0", as before the ESP32-035 was known.
static cydr_model_t _cydr_modelFrom(cydr_board_t board, cydr_panel_t panel, int8_t redLed) {
    if (board == CYDR_BOARD_SHARED_TOUCH && panel == CYDR_PANEL_ST7796)
        return redLed == 4 ? CYDR_MODEL_035 : CYDR_MODEL_32E_35_40;
    if (redLed < 0) return CYDR_MODEL_UNKNOWN;
    if (board == CYDR_BOARD_SHARED_TOUCH)
        return redLed == 4 ? CYDR_MODEL_024 : CYDR_MODEL_32E_32;
    return redLed == 4 ? CYDR_MODEL_2432S028 : CYDR_MODEL_32E_28;
}

// ── Power-on init sequences ────────────────────────────────────────────────
// MADCTL is written here as a placeholder and then re-issued by setRotation().

// Standard ILI9341 startup (gamma, power, frame rate).
void chipguy_CYD_resistive_display::_initILI9341() {
    _cmd(0x01);                            // Software reset
    delay(150);
#if CYDR_CHIP_S3
    if (_cydr_s3PanelIsIps(_model)) {
        // The ES3C28P's IPS ILI9341V: the vendor's power, VCOM, interface
        // and gamma values (their ILI9341V_Init.txt / TFT_eSPI init).
        // Inversion is set after init, from _invert.
        _cmd(0xCF); { const uint8_t d[] = {0x00, 0xC1, 0x30}; _data(d, 3); }
        _cmd(0xED); { const uint8_t d[] = {0x64, 0x03, 0x12, 0x81}; _data(d, 4); }
        _cmd(0xE8); { const uint8_t d[] = {0x85, 0x00, 0x78}; _data(d, 3); }
        _cmd(0xCB); { const uint8_t d[] = {0x39, 0x2C, 0x00, 0x34, 0x02}; _data(d, 5); }
        _cmd(0xF7); _data8(0x20);
        _cmd(0xEA); { const uint8_t d[] = {0x00, 0x00}; _data(d, 2); }
        _cmd(0xC0); _data8(0x13);          // Power control 1
        _cmd(0xC1); _data8(0x13);          // Power control 2
        _cmd(0xC5); { const uint8_t d[] = {0x22, 0x35}; _data(d, 2); }  // VCOM 1
        _cmd(0xC7); _data8(0xBD);          // VCOM 2
        _cmd(0x36); _data8(CYDR_MADCTL_BGR);    // MADCTL (overwritten by setRotation)
        _cmd(0xB6); { const uint8_t d[] = {0x0A, 0xA2}; _data(d, 2); }  // Display func
        _cmd(0x3A); _data8(0x55);          // Pixel format: 16-bit RGB565
        _cmd(0xF6); { const uint8_t d[] = {0x01, 0x30}; _data(d, 2); }  // Interface control
        _cmd(0xB1); { const uint8_t d[] = {0x00, 0x1B}; _data(d, 2); }  // Frame rate 70 Hz
        _frmStockClocks = 0x1B;          // stock for setRefreshStep()
        _cmd(0xF2); _data8(0x00);          // 3Gamma disable
        _cmd(0x26); _data8(0x01);          // Gamma curve selected
        _cmd(0xE0); { const uint8_t d[] = {0x0F, 0x35, 0x31, 0x0B, 0x0E, 0x06, 0x49,
                      0xA7, 0x33, 0x07, 0x0F, 0x03, 0x0C, 0x0A, 0x00}; _data(d, 15); }
        _cmd(0xE1); { const uint8_t d[] = {0x00, 0x0A, 0x0F, 0x04, 0x11, 0x08, 0x36,
                      0x58, 0x4D, 0x07, 0x10, 0x0C, 0x32, 0x34, 0x0F}; _data(d, 15); }
        _cmd(0x11);                        // Sleep out
        delay(120);
        _cmd(0x29);                        // Display on
        delay(20);
        return;
    }
#endif
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
    _frmStockClocks = 0x18;                // stock for setRefreshStep()
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
// format and display state are set.  The 2.8" and 2.4" panels show correct
// colors with inversion OFF, unlike most ST7789 IPS modules; the 3.2" needs it
// ON — without INVON it shows inverse video (bench, 2026-10-13), and with it
// the 2.4" does (bench, 2026-10-18).  The 3.2" is the only shared-touch ST7789
// with an amplifier on GPIO4, so a board whose model is unknown (no GPIO4
// reading) keeps the 3.2"'s INVON as before.  The 2432S032 needs it ON too
// (bench, 2026-10-29).
void chipguy_CYD_resistive_display::_initST7789() {
    _cmd(0x01);                            // Software reset
    delay(150);
    _cmd(0x11);                            // Sleep out
    delay(120);
    _cmd(0x3A); _data8(0x55);              // Pixel format: 16-bit RGB565
    _cmd(0x36); _data8(0x00);              // MADCTL (overwritten by setRotation)
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
    while (_pixelBusy) {
        spi_transaction_t *result = nullptr;
        spi_device_get_trans_result(_spi, &result, portMAX_DELAY);
        _pixelBusy--;
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
    if (_useOverrides) cydr_loadOverrides(_ovr);
    else               _ovr = {};
    if (_panel == CYDR_PANEL_AUTO) _panel = _detectPanel(hint);
    // The panel override only stands in for a guess, never for an ID.
    if (_panelGuessed && _ovr.panel >= CYDR_PANEL_ILI9341 && _ovr.panel <= CYDR_PANEL_ST7796) {
        _auto.panel = _panel;
        _panel = (cydr_panel_t)_ovr.panel;
    }
    // A capacitive touch chip on I2C, on any board.
    uint8_t capAddr = 0;
#if CYDR_CHIP_S3
    bool s3Codec = false;
    cydr_touch_chip_t cap = _cydr_s3ProbeI2c(&capAddr, &s3Codec);
#else
    cydr_touch_chip_t cap = cydr_probeI2cTouch(&capAddr);
#endif
    // The wiring comes from the backlight pins; only if they are
    // inconclusive is it guessed -- backlight 27 where a capacitive chip
    // answers (on the 2432S024C GPIO21 is the CST820's INT, which spoils the
    // GPIO21 reading; bench, 2026-10-31), else from the panel.
    _board = hint;
    if (_board == CYDR_BOARD_UNKNOWN && !CYDR_CHIP_CLASSIC) {
        _board = CYDR_BOARD_SHARED_TOUCH;  // one backlight pin: nothing to choose
    } else if (_board == CYDR_BOARD_UNKNOWN) {
        _board = cap != CYDR_TOUCH_XPT2046 || _panel == CYDR_PANEL_ST7796
                     ? CYDR_BOARD_SHARED_TOUCH : CYDR_BOARD_OWN_TOUCH;
        Serial.printf("cydr display: backlight pins inconclusive; assuming "
                      "backlight GPIO%d from the %s\n",
                      _board == CYDR_BOARD_SHARED_TOUCH ? CYDR_TFT_BL_SHARED : CYDR_TFT_BL_OWN,
                      cap != CYDR_TOUCH_XPT2046 ? "capacitive touch chip" : panelName());
    }
    _cydr_activeBoard = _board;
    _cydr_activePanel = _panel;
    // Latch the red LED pin now, while ADC2 is still ours (before WiFi); it
    // names the board, and the ST7789 init depends on which.
#if CYDR_CHIP_C5
    if (true) {
        _model = CYDR_MODEL_NM_CYD_C5;     // the build names the board
    } else
#elif CYDR_CHIP_S3
    if (true) {
        // The panel and the I2C chips name the board: chipguy_CYD_board_ESP32S3.cpp.
        _model = _cydr_s3Model(_panel, _cydr_panelSdaOnly, cap, s3Codec);
    } else
#endif
    if (_cydr_panelSdaOnly && _board == CYDR_BOARD_SHARED_TOUCH && _panel == CYDR_PANEL_ST7789) {
        // The 2432S032 is named by its panel, and its schematic puts the red
        // LED on GPIO4 -- but a unit whose LED is dead reads ~130 mV there,
        // which the voltage test would take for a failed read (bench,
        // 2026-10-29).  GPIO4's pull-down is on from reset and lights the LED
        // dimly, so clear it as the voltage test would have.
        if (_cydr_redLed < 0) _cydr_redLed = 4;
        gpio_set_pull_mode((gpio_num_t)4, GPIO_FLOATING);
        _model = cap == CYDR_TOUCH_GT911 ? CYDR_MODEL_2432S032C : CYDR_MODEL_2432S032;
    } else if (cap == CYDR_TOUCH_CST820) {
        // CST820 boards: the panel names them.  The 2432S024C's backlight
        // pins read inconclusive (GPIO21 is the touch INT), the JC2432W328's
        // shared-touch, as the ESP32-024 it would otherwise be taken for.
        cydr_redLedPin();                  // measured: it answers redLedPin()
        if (_panel == CYDR_PANEL_ILI9341 && hint != CYDR_BOARD_OWN_TOUCH)
            _model = CYDR_MODEL_2432S024C;
        else if (_panel == CYDR_PANEL_ST7789 && hint == CYDR_BOARD_SHARED_TOUCH)
            _model = CYDR_MODEL_JC2432W328;
        else {
            _model = CYDR_MODEL_UNKNOWN;
            Serial.printf("cydr display: CST820 touch with a %s panel on %s wiring: "
                          "an unseen board variant; model unknown\n", panelName(),
                          hint == CYDR_BOARD_SHARED_TOUCH ? "shared-touch" :
                          hint == CYDR_BOARD_OWN_TOUCH ? "own-touch" : "undetermined");
        }
    } else if (cap == CYDR_TOUCH_GT911 && _panel == CYDR_PANEL_ST7796 &&
               _board == CYDR_BOARD_SHARED_TOUCH) {
        // The ESP32-3248S035C.  Its ST7796 answers no ID read on GPIO12 or on
        // SDA, so the panel is the shared-touch guess; the GT911 (0x5D) names
        // the board (bench, 2026-11-02).  The RGB LED is on 4 / 16 / 17; on
        // the unit seen it is dead (open), and GPIO4 then reads 0.13 V with
        // the pull-down, which the voltage test would call a failed read --
        // so, as on the 2432S032, the pin isn't measured.  Clear the reset
        // pull-down as the voltage test would have.
        if (_cydr_redLed < 0) _cydr_redLed = 4;
        gpio_set_pull_mode((gpio_num_t)4, GPIO_FLOATING);
        _model = CYDR_MODEL_3248S035C;
    } else if (cap == CYDR_TOUCH_GT911) {
        // A GT911 anywhere else: touch is driven, the board isn't named.
        cydr_redLedPin();
        _model = CYDR_MODEL_UNKNOWN;
        Serial.printf("cydr display: GT911 touch with a %s panel: an unseen board "
                      "variant; model unknown\n", panelName());
    } else if (_cydr_panelSdaOnly &&
               !(_panel == CYDR_PANEL_ST7789 && hint == CYDR_BOARD_OWN_TOUCH)) {
        // A panel that answers only on SDA, on no board seen so far.  The
        // panel (and so the geometry) is known; the board isn't.
        _model = CYDR_MODEL_UNKNOWN;
        cydr_redLedPin();                  // still measured, for the log
        Serial.printf("cydr display: %s answered only on SDA (GPIO13) on %s wiring: "
                      "an unseen board variant; model unknown\n", panelName(),
                      hint == CYDR_BOARD_SHARED_TOUCH ? "shared-touch" :
                      hint == CYDR_BOARD_OWN_TOUCH ? "own-touch" : "undetermined");
    } else {
        // An own-touch ST7789 answering only on SDA is the 2.8" panel that is
        // silent on GPIO12, already known; it is named as before.
        _model = _cydr_modelFrom(_board, _panel, cydr_redLedPin());
    }
    _touchChip = cap;
    _touchAddr = capAddr;
    // The model as detected, and the red LED as measured, before the model
    // override replaces them.  (Everything else's auto value is what follows
    // from the model in effect.)
    _auto.model  = _model;
    _auto.redLed = _cydr_redLed == 4 ? 1 : _cydr_redLed == 22 ? 2 : 0;

    // A forced model brings what it implies: the wiring (so the backlight and
    // the XPT2046's bus), the red LED, and the touch chip.  The panel stays
    // as detected.
    // A model of the other chip can't be forced: its pins don't exist here.
    if (_ovr.model > CYDR_MODEL_UNKNOWN && _ovr.model < CYDR_MODEL_COUNT &&
        cydr_modelChip((cydr_model_t)_ovr.model) == CYDR_THIS_CHIP) {
        _model = (cydr_model_t)_ovr.model;
        _cydr_modelFacts f = _cydr_factsOf(_model);
        _board = f.board;
        _cydr_redLed = f.redLed;
        _touchChip = f.chip;
        _touchAddr = cap == f.chip ? capAddr : 0;   // the touch driver probes if 0
    }
    _cydr_activeModel = _model;
    _cydr_activeBoard = _board;
    _cydr_activeTouchChip = _touchChip;
    _cydr_activeTouchAddr = _touchAddr;
    // Size and power key follow the model in effect (forced or detected),
    // and are what the auto values report.
    _sizeTenths = _cydr_factsOf(_model).sizeTenths;
    _auto.size = 0;
    for (uint8_t i = 1; i < sizeof cydr_sizeTenths; i++)
        if (cydr_sizeTenths[i] == _sizeTenths) _auto.size = i;
    if (_ovr.size) _sizeTenths = cydr_sizeTenths[_ovr.size];
    _powerKey = _cydr_factsOf(_model).powerKey;
    _auto.powerKey = _powerKey;
    if (_ovr.powerKey) _powerKey = (cydr_power_key_t)_ovr.powerKey;

    if (_panel == CYDR_PANEL_ST7796) { _nativeW = 320; _nativeH = 480; }
    else                               { _nativeW = 240; _nativeH = 320; }
    _blPin = _board == CYDR_BOARD_SHARED_TOUCH ? CYDR_TFT_BL_SHARED : CYDR_TFT_BL_OWN;
    _touchBus = _board;

    // What detection chose, before any override.  The ST7789 panels have RGB
    // order and the other column direction; inversion is on for the 3.2" and
    // the 2432S032 (and an unknown shared-touch ST7789, as the 3.2") and off
    // for the 2.8", the ST7789 ESP32-024 and the JC2432W328.
    _bgr = _panel != CYDR_PANEL_ST7789;
    _mx0 = _panel != CYDR_PANEL_ST7789;
    _my0 = false;
    _invert = _panel == CYDR_PANEL_ST7789 && _board == CYDR_BOARD_SHARED_TOUCH &&
              _model != CYDR_MODEL_024 && _model != CYDR_MODEL_JC2432W328 &&
              _model != CYDR_MODEL_NM_CYD_C5;     // TFT_INVERSION_OFF, per the vendor
#if CYDR_CHIP_S3
    // The ES3C28P's IPS ILI9341V wants INVON, as in the vendor's init, and
    // its columns run the other way from the classic ILI9341 modules': MX
    // clear in rotation 0, as in the vendor's portrait MADCTL (0x08).  With
    // MX set the picture is mirrored left to right (bench, 2026-11-09).
    if (_cydr_s3PanelIsIps(_model)) { _invert = true; _mx0 = false; }
#endif
    _spiHz = CYDR_TFT_SPI_HZ;
    _auto.colorOrder = _bgr ? 2 : 1;
    _auto.inversion  = _invert ? 2 : 1;
    _auto.mirrorX    = _mx0 ? 2 : 1;
    _auto.mirrorY    = _my0 ? 2 : 1;
    _auto.backlight  = _blPin == CYDR_TFT_BL_SHARED ? 2 : 1;
    _auto.touchBus   = _touchBus == CYDR_BOARD_SHARED_TOUCH ? 2 : 1;
    _cydr_gbOverride = CYDR_OVR_AUTO;
    { bool sw; _auto.greenBlue = _cydr_gbKnown(sw) ? (sw ? 2 : 1) : 0; }
    { int8_t g = _cydr_gpio34FromModel(_model); _auto.gpio34 = g < 0 ? 0 : g ? 2 : 1; }

    // Apply the overrides.
    if (_ovr.colorOrder) _bgr = _ovr.colorOrder == 2;
    if (_ovr.inversion)  _invert = _ovr.inversion == 2;
    if (_ovr.mirrorX)    _mx0 = _ovr.mirrorX == 2;
    if (_ovr.mirrorY)    _my0 = _ovr.mirrorY == 2;
    if (_ovr.spiClock)   _spiHz = _ovr.spiClock == 3 ? 80000000 :
                                  _ovr.spiClock == 2 ? 20000000 : 27000000;
    if (_spiHzForced)    _spiHz = _spiHzForced;
#if CYDR_CHIP_CLASSIC
    // The pin overrides name classic-board pins; the C5 and S3 boards have one wiring.
    if (_ovr.backlight)  _blPin = _ovr.backlight == 2 ? CYDR_TFT_BL_SHARED : CYDR_TFT_BL_OWN;
    if (_ovr.touchBus)   _touchBus = _ovr.touchBus == 2 ? CYDR_BOARD_SHARED_TOUCH : CYDR_BOARD_OWN_TOUCH;
    if (_ovr.redLed)     _cydr_redLed = _ovr.redLed == 2 ? 22 : 4;
    _cydr_gbOverride = _ovr.greenBlue;
#endif
    _cydr_activeTouchBus = _touchBus;
    if (cydr_anyOverride(_ovr))
        Serial.printf("cydr display: overrides from NVS in effect (panel %u, RGB/BGR %u, "
                      "inversion %u, MX %u, MY %u, SPI %u, backlight %u, touch bus %u, "
                      "touch MX %u MY %u swap %u, red %u, G/B %u, GPIO34 %u, model %u, size %u, "
                      "power key %u)\n",
                      _ovr.panel, _ovr.colorOrder, _ovr.inversion, _ovr.mirrorX, _ovr.mirrorY,
                      _ovr.spiClock, _ovr.backlight, _ovr.touchBus, _ovr.touchMirrorX,
                      _ovr.touchMirrorY, _ovr.touchSwapXY, _ovr.redLed, _ovr.greenBlue,
                      _ovr.gpio34, _ovr.model, _ovr.size, _ovr.powerKey);

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

    // The bus's interrupt is allocated on the core that initializes it; a
    // task that queues transfers should run there too (spiCore()).
    _spiCore = xPortGetCoreID();
    esp_err_t err = spi_bus_initialize(CYDR_TFT_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        Serial.printf("cydr display: spi_bus_initialize failed: 0x%x\n", err);
        return false;
    }

    spi_device_interface_config_t devcfg = {};
    devcfg.clock_speed_hz = (int)_spiHz;
    devcfg.mode = 0;
    devcfg.spics_io_num = CYDR_TFT_CS;   // hardware-managed CS
    devcfg.queue_size = 2;              // one async pixel transfer (+ its tail) in flight
    devcfg.pre_cb = _cydr_spi_pre_cb;    // sets DC from each transaction's user flag
    devcfg.flags = SPI_DEVICE_NO_DUMMY;
    // A panel that answers only on SDA (the 2432S032's: no SDO wire) is read
    // the 3-wire way: the SPI peripheral turns MOSI around for the read phase
    // of a half-duplex transaction.  Writes are unchanged.
    if (_cydr_panelSdaOnly) devcfg.flags |= SPI_DEVICE_3WIRE | SPI_DEVICE_HALFDUPLEX;

    err = spi_bus_add_device(CYDR_TFT_SPI_HOST, &devcfg, &_spi);
    if (err != ESP_OK) {
        Serial.printf("cydr display: spi_bus_add_device failed: 0x%x\n", err);
        _spi = nullptr;
        return false;
    }

    _refreshStep = 0;                      // every init leaves the stock rate
    switch (_panel) {
        case CYDR_PANEL_ST7789: _initST7789(); break;
        case CYDR_PANEL_ST7796: _initST7796(); break;
        default:                  _initILI9341(); break;
    }
    _cmd(_invert ? 0x21 : 0x20);           // INVON / INVOFF
    setRotation(rotation);
    _readback = _probeReadback();          // before the backlight: unseen
    _cydr_activeReadback = _readback;

    // Backlight on at full brightness.
    setBacklight(100);

    return true;
}

// ── Readback check ─────────────────────────────────────────────────────────
// Write four faint pixels -- red, green, blue, a mix -- at the top left, read
// them back with RAMRD (0x2E) at 2 MHz, and call readback available only on
// an exact match.  The read is a dummy byte, then 3 bytes a pixel (RGB666,
// ILI9341 / ST7789) or 2 (RGB565, ST7796); see README-engineering.md,
// "Readback".  On a 3-wire device the read is the half-duplex MISO phase,
// which the peripheral takes from MOSI.  Runs in begin(), before the
// backlight, so the pixels are never seen; the first clear covers them.
cydr_readback_t chipguy_CYD_resistive_display::_probeReadback() {
    static const uint16_t probe[4] = {0x1000, 0x0040, 0x0002, 0x5AA5};
    alignas(4) static uint8_t buf[16];     // static: DMA-capable internal RAM
    const int bpp = _panel == CYDR_PANEL_ST7796 ? 2 : 3;
    const int n = (1 + 4 * bpp + 3) & ~3;

    uint8_t px[8];
    for (int i = 0; i < 4; i++) { px[2 * i] = probe[i] >> 8; px[2 * i + 1] = probe[i]; }
    _setAddrWindow(0, 0, 3, 0);
    _data(px, sizeof px);
    _setAddrWindow(0, 0, 3, 0);            // ends with RAMWR, which RAMRD replaces

    memset(buf, 0, sizeof buf);
    spi_device_acquire_bus(_spi, portMAX_DELAY);
    spi_transaction_t t = {};
    t.length = 8;
    t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_CS_KEEP_ACTIVE;
    t.tx_data[0] = 0x2E;                   // RAMRD
    t.user = (void *)0;
    t.override_freq_hz = 2000000;
    spi_device_polling_transmit(_spi, &t);
    t = {};
    if (_cydr_panelSdaOnly) { t.length = 0; t.rxlength = n * 8; }
    else                      t.length = n * 8;
    t.rx_buffer = buf;
    t.user = (void *)1;
    t.override_freq_hz = 2000000;
    spi_device_polling_transmit(_spi, &t);
    // NOP at the write clock.  override_freq_hz sticks to the device for the
    // transactions after it, so the write clock has to be set back explicitly.
    t = {};
    t.length = 8;
    t.flags = SPI_TRANS_USE_TXDATA;
    t.tx_data[0] = 0x00;
    t.user = (void *)0;
    t.override_freq_hz = _spiHz;
    spi_device_polling_transmit(_spi, &t);
    spi_device_release_bus(_spi);

    bool ok = true;
    const uint8_t *in = buf + 1;
    for (int i = 0; i < 4; i++) {
        uint16_t v;
        if (bpp == 3) { v = (in[0] >> 3) << 11 | (in[1] >> 2) << 5 | in[2] >> 3; in += 3; }
        else          { v = in[0] << 8 | in[1]; in += 2; }
        ok &= v == probe[i];
    }
    if (ok) return _cydr_panelSdaOnly ? CYDR_READBACK_SDA : CYDR_READBACK_MISO;
    if (_cydr_panelSdaOnly) {              // expected to work: say what came back
        Serial.printf("cydr display: readback on SDA failed:");
        for (int i = 0; i < n; i++) Serial.printf(" %02X", buf[i]);
        Serial.println();
    }
    return CYDR_READBACK_NONE;
}

// One row of frame memory, read back at 2 MHz into buf (a dummy byte, then
// 3 bytes a pixel, or 2 on the ST7796), as _probeReadback() does.  The
// device's clock is left at _spiHz.
bool chipguy_CYD_resistive_display::_readRow(int x0, int x1, int y, uint8_t *buf, size_t bytes) {
    _setAddrWindow(x0, y, x1, y);          // ends with RAMWR, which RAMRD replaces
    bool lock = cydr_readbackNeedsTouchLock();
    if (lock) cydr_touchI2cLock();
    spi_device_acquire_bus(_spi, portMAX_DELAY);
    spi_transaction_t t = {};
    t.length = 8;
    t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_CS_KEEP_ACTIVE;
    t.tx_data[0] = 0x2E;                   // RAMRD
    t.user = (void *)0;
    t.override_freq_hz = 2000000;
    spi_device_polling_transmit(_spi, &t);
    t = {};
    if (_cydr_panelSdaOnly) { t.length = 0; t.rxlength = bytes * 8; }
    else                      t.length = bytes * 8;
    t.rx_buffer = buf;
    t.user = (void *)1;
    t.override_freq_hz = 2000000;
    spi_device_polling_transmit(_spi, &t);
    t = {};
    t.length = 8;
    t.flags = SPI_TRANS_USE_TXDATA;
    t.tx_data[0] = 0x00;                   // NOP, back at the write clock
    t.user = (void *)0;
    t.override_freq_hz = _spiHz;
    spi_device_polling_transmit(_spi, &t);
    spi_device_release_bus(_spi);
    if (lock) cydr_touchI2cUnlock();
    return true;
}

int chipguy_CYD_resistive_display::stressTest(uint32_t hz, uint32_t ms,
                                              void (*progress)(uint32_t, uint32_t), int *windows) {
    if (windows) *windows = 0;
    if (!_spi || _readback == CYDR_READBACK_NONE) return -1;
    const int W = _w, H = _h, maxRows = 40;
    const int bpp = _panel == CYDR_PANEL_ST7796 ? 2 : 3;
    const size_t rowBytes = (1 + (size_t)W * bpp + 3) & ~(size_t)3;
    uint16_t *px = (uint16_t *)heap_caps_malloc((size_t)W * maxRows * 2, MALLOC_CAP_DMA);
    uint8_t *rd = (uint8_t *)heap_caps_malloc(rowBytes, MALLOC_CAP_DMA);
    if (!px || !rd) { heap_caps_free(px); heap_caps_free(rd); return -1; }
    _waitPixelDone();
    const uint32_t savedHz = _spiHz;
    _spiHz = hz;                            // _readRow() returns the clock to this
    auto setClock = [this](uint32_t f) {    // a NOP at f: the clock sticks to the device
        spi_transaction_t t = {};
        t.length = 8; t.flags = SPI_TRANS_USE_TXDATA; t.tx_data[0] = 0;
        t.user = (void *)0; t.override_freq_hz = f;
        spi_device_polling_transmit(_spi, &t);
    };
    setClock(hz);
    // xorshift32, not an LCG: an LCG's low bit alternates, which would make
    // every window's width and height differ in parity -- always an even
    // pixel count -- and miss the classic ESP32's fault at 80 MHz, which
    // corrupts DMA transfers whose length is 2 mod 4 bytes.
    auto next = [](uint32_t &s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; };
    uint32_t rng = 0x2545F491u ^ hz ^ (uint32_t)esp_timer_get_time();

    // The whole screen to noise first, so the edges -- which random windows
    // reach less often -- don't sit black and jagged.  Not checked.
    for (int y = 0; y < H; y += maxRows) {
        int h = min(maxRows, H - y);
        for (int k = 0; k < W * h; k++) px[k] = (uint16_t)next(rng);
        flushRegion(0, y, W - 1, y + h - 1, px);
        _waitPixelDone();
    }

    int bad = 0, n = 0;
    const uint32_t t0 = millis();
    // UART noise: keep Serial's TX line toggling during the DMAs.  Only what
    // fits without waiting, so the test's pace doesn't change.
    static const char noise[] = "UUUU****????pppp~~~~UUUU";
    // LED noise: the RGB LED's three PWM channels share one timer, so they
    // all switch on together; every 50 ms, equal duties (they switch off
    // together too) or different ones (the off-edges spread out).  Active
    // LOW.  The pins come from detection: on the 32E boards GPIO4 is the
    // amplifier's shutdown, and cydr_redLedPin() is 22 there.
    const int8_t ledPins[3] = {cydr_redLedPin(), cydr_greenLedPin(), cydr_blueLedPin()};
    const int8_t rgbPin = cydr_rgbLedPin();
    int ledPhase = -1;
    auto uartNoise = [&]() {
#if !ARDUINO_USB_CDC_ON_BOOT
        if (Serial.availableForWrite() >= (int)sizeof noise - 1)
            Serial.write((const uint8_t *)noise, sizeof noise - 1);
#endif
        int phase = (millis() / 50) & 1;
        if (phase == ledPhase) return;
        ledPhase = phase;
        static const uint8_t same[3] = {128, 128, 128}, spread[3] = {40, 128, 215};
        const uint8_t *duty = phase ? spread : same;
        for (int i = 0; i < 3; i++)
            if (ledPins[i] >= 0) analogWrite(ledPins[i], 255 - duty[i]);
        if (rgbPin >= 0) {
            if (phase) rgbLedWrite(rgbPin, 20, 90, 200);
            else       rgbLedWrite(rgbPin, 120, 120, 120);
        }
    };
    while (millis() - t0 < ms) {
        uartNoise();
        if (n & 1) {
            // A bulk window, as a sketch that streams frames sends them: full
            // width, up to the whole screen tall, as back-to-back queued DMAs
            // of up to 40 rows each (the same 40 random rows, repeated), two
            // in flight.  An ESP32-32E 2.8" (ILI9341) that passed the
            // small windows below every time lost bytes in a stream like this
            // about once in 9 MB.  First, last and two random rows are read
            // back.  W is even, so every DMA is a multiple of 4 bytes.
            int h = 1 + next(rng) % H, y0 = next(rng) % (H - h + 1);
            for (int k = 0; k < W * maxRows; k++) px[k] = __builtin_bswap16((uint16_t)next(rng));
            _setAddrWindow(0, y0, W - 1, y0 + h - 1);
            static spi_transaction_t bt[2];
            int inflight = 0, slot = 0;
            for (int r = 0; r < h; r += maxRows) {
                int rows = min(maxRows, h - r);
                uartNoise();                // the stream runs for many ms: keep TX busy
                bt[slot] = {};
                bt[slot].length = (size_t)rows * W * 16;
                bt[slot].tx_buffer = px;
                bt[slot].user = (void *)1;
                spi_device_queue_trans(_spi, &bt[slot], portMAX_DELAY);
                slot ^= 1;
                if (++inflight == 2) {
                    spi_transaction_t *done;
                    spi_device_get_trans_result(_spi, &done, portMAX_DELAY);
                    inflight--;
                }
            }
            while (inflight) {
                spi_transaction_t *done;
                spi_device_get_trans_result(_spi, &done, portMAX_DELAY);
                inflight--;
            }
            const int rowsToCheck[4] = {0, h - 1, (int)(next(rng) % h), (int)(next(rng) % h)};
            bool wrong = false;
            for (int i = 0; i < 4 && !wrong; i++) {
                int r = rowsToCheck[i];
                _readRow(0, W - 1, y0 + r, rd, rowBytes);
                const uint8_t *in = rd + 1;
                const uint16_t *want = px + (size_t)(r % maxRows) * W;
                for (int c = 0; c < W; c++) {
                    uint16_t got;
                    if (bpp == 3) { got = (in[0] >> 3) << 11 | (in[1] >> 2) << 5 | in[2] >> 3; in += 3; }
                    else          { got = in[0] << 8 | in[1]; in += 2; }
                    if (got != __builtin_bswap16(want[c])) { wrong = true; break; }
                }
            }
            bad += wrong;
            n++;
            if (progress) progress(millis() - t0, ms);
            continue;
        }
        int w = 1 + next(rng) % W, h = 1 + next(rng) % maxRows;
        int x0 = next(rng) % (W - w + 1), y0 = next(rng) % (H - h + 1);
        uint32_t seed = next(rng) | 1;
        uint32_t s = seed;
        for (int k = 0; k < w * h; k++) px[k] = (uint16_t)next(s);
        flushRegion(x0, y0, x0 + w - 1, y0 + h - 1, px);   // byte-swaps px in place
        _waitPixelDone();
        // Rows to check: first, last, one in the middle; all of every fourth.
        bool all = (n & 3) == 3;
        int mid = h > 2 ? 1 + next(rng) % (h - 2) : 0;
        bool wrong = false;
        s = seed;
        for (int r = 0; r < h && !wrong; r++) {
            bool check = all || r == 0 || r == h - 1 || r == mid;
            if (check) {
                _readRow(x0, x0 + w - 1, y0 + r, rd, (1 + (size_t)w * bpp + 3) & ~(size_t)3);
                const uint8_t *in = rd + 1;
                for (int c = 0; c < w; c++) {
                    uint16_t want = (uint16_t)next(s), got;
                    if (bpp == 3) { got = (in[0] >> 3) << 11 | (in[1] >> 2) << 5 | in[2] >> 3; in += 3; }
                    else          { got = in[0] << 8 | in[1]; in += 2; }
                    if (got != want) { wrong = true; break; }
                }
            } else {
                for (int c = 0; c < w; c++) next(s);
            }
        }
        bad += wrong;
        n++;
        if (progress) progress(millis() - t0, ms);
    }
#if !ARDUINO_USB_CDC_ON_BOOT
    Serial.println();                       // end the noise line
#endif
    for (int8_t pin : ledPins) {            // LED off, PWM released
        if (pin < 0) continue;
        ledcDetach(pin);
        pinMode(pin, OUTPUT);
        digitalWrite(pin, HIGH);
    }
    if (rgbPin >= 0) rgbLedWrite(rgbPin, 0, 0, 0);
    _spiHz = savedHz;
    setClock(savedHz);
    heap_caps_free(px);
    heap_caps_free(rd);
    if (windows) *windows = n;
    return bad;
}

void chipguy_CYD_resistive_display::changeSpiClock(uint32_t hz) {
    if (!_spi || !hz) return;
    _waitPixelDone();
    _spiHz = hz;
    spi_transaction_t t = {};               // a NOP at hz: the clock sticks to the device
    t.length = 8;
    t.flags = SPI_TRANS_USE_TXDATA;
    t.tx_data[0] = 0x00;
    t.user = (void *)0;
    t.override_freq_hz = hz;
    spi_device_polling_transmit(_spi, &t);
}

// As _readRow(), for a register: the command, then n bytes read at hz, then
// a NOP that sets the write clock back.
bool chipguy_CYD_resistive_display::readCommand(uint8_t cmd, uint8_t *buf, size_t n, uint32_t hz) {
    if (!_spi || _readback == CYDR_READBACK_NONE || n < 1 || n > 4) return false;
    _waitPixelDone();
    bool lock = cydr_readbackNeedsTouchLock();
    if (lock) cydr_touchI2cLock();
    spi_device_acquire_bus(_spi, portMAX_DELAY);
    spi_transaction_t t = {};
    t.length = 8;
    t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_CS_KEEP_ACTIVE;
    t.tx_data[0] = cmd;
    t.user = (void *)0;
    t.override_freq_hz = hz;
    spi_device_polling_transmit(_spi, &t);
    t = {};
    t.flags = SPI_TRANS_USE_RXDATA;
    if (_cydr_panelSdaOnly) { t.length = 0; t.rxlength = n * 8; }
    else                      t.length = n * 8;
    t.user = (void *)1;
    t.override_freq_hz = hz;
    spi_device_polling_transmit(_spi, &t);
    memcpy(buf, t.rx_data, n);
    t = {};
    t.length = 8;
    t.flags = SPI_TRANS_USE_TXDATA;
    t.tx_data[0] = 0x00;                   // NOP, back at the write clock
    t.user = (void *)0;
    t.override_freq_hz = _spiHz;
    spi_device_polling_transmit(_spi, &t);
    spi_device_release_bus(_spi);
    if (lock) cydr_touchI2cUnlock();
    return true;
}

// ── Refresh-rate steps ─────────────────────────────────────────────────────
// ILI9341 FRMCTR1: rate = fosc / (DIVA * RTNA * lines), DIVA 1/2/4/8, RTNA
// 16..31 clocks a line; the init writes 00 18 (00 1B on the S3's IPS
// ILI9341V).  A pair (DIVA, RTNA) runs at stock / (DIVA * RTNA) of the stock
// rate; measured 94.53 Hz stock and 36.65 Hz at 01 1F on an ESP32-2432S028
// (94.53 * 24 / 62 = 36.59).  ST7789 FRCTRL2: a line of 250 + 16 * RTNA
// clocks, RTNA 0..31, 0x0F at power-on (the init leaves it).  Steps are the
// distinct ratios at or below 1, fastest first.

#define CYDR_MAX_REFRESH_STEPS 64

bool chipguy_CYD_resistive_display::_refreshStepRegs(int step, uint8_t &a, uint8_t &b,
                                                     float &ratio) const {
    if (step < 0) return false;
    if (_panel == CYDR_PANEL_ST7789) {
        int r = 15 + step;                  // 0x0F is stock
        if (r > 31) return false;
        a = (uint8_t)r; b = 0;
        ratio = 490.0f / (250 + 16 * r);
        return true;
    }
    if (_panel != CYDR_PANEL_ILI9341) return false;
    // Every DIVA x RTNA product at or above the stock clocks, distinct,
    // ascending: built once, at most 4 x 16.
    static uint8_t prodA[CYDR_MAX_REFRESH_STEPS], prodB[CYDR_MAX_REFRESH_STEPS];
    static uint16_t prod[CYDR_MAX_REFRESH_STEPS];
    static int count = -1;
    static uint8_t builtFor = 0;
    if (count < 0 || builtFor != _frmStockClocks) {
        count = 0;
        builtFor = _frmStockClocks;
        for (int d = 0; d < 4; d++)
            for (int r = 16; r <= 31; r++) {
                uint16_t p = (uint16_t)((1 << d) * r);
                if (p < _frmStockClocks) continue;
                bool dup = false;
                for (int i = 0; i < count; i++) dup |= prod[i] == p;
                if (dup) continue;
                int i = count++;
                while (i > 0 && prod[i - 1] > p) {   // insertion sort
                    prod[i] = prod[i - 1]; prodA[i] = prodA[i - 1]; prodB[i] = prodB[i - 1]; i--;
                }
                prod[i] = p; prodA[i] = (uint8_t)d; prodB[i] = (uint8_t)r;
            }
    }
    if (step >= count) return false;
    a = prodA[step]; b = prodB[step];
    ratio = (float)_frmStockClocks / prod[step];
    return true;
}

int chipguy_CYD_resistive_display::refreshStepCount() const {
    int n = 0;
    uint8_t a, b; float r;
    while (_refreshStepRegs(n, a, b, r)) n++;
    return n;
}

float chipguy_CYD_resistive_display::refreshStepRatio(int step) const {
    uint8_t a, b; float r;
    return _refreshStepRegs(step, a, b, r) ? r : 0;
}

bool chipguy_CYD_resistive_display::setRefreshStep(int step, float stockHz) {
    uint8_t a, b; float r;
    if (!_spi || stockHz <= 0 || !_refreshStepRegs(step, a, b, r)) return false;
    if (stockHz * r < CYDR_MIN_REFRESH_HZ) return false;
    _waitPixelDone();
    if (_panel == CYDR_PANEL_ST7789) { _cmd(0xC6); _data8(a); }
    else { _cmd(0xB1); const uint8_t d[] = {a, b}; _data(d, 2); }
    _refreshStep = step;
    return true;
}

bool chipguy_CYD_resistive_display::applyRefreshChoice() {
    cydr_refresh_choice_t c;
    if (!cydr_loadRefreshChoice(c) || c.panel != _panel) return false;
    return setRefreshStep(c.step, c.stockHz);
}

bool cydr_readbackNeedsTouchLock() {
    // Only the classic boards keep an XPT2046 whose CS is the touch chip's SDA.
    return CYDR_CHIP_CLASSIC && _cydr_activeReadback == CYDR_READBACK_MISO &&
           cydr_activeTouchChip() != CYDR_TOUCH_XPT2046;
}

const char *chipguy_CYD_resistive_display::readbackName() const {
    switch (_readback) {
        case CYDR_READBACK_MISO: return CYDR_CHIP_C5 ? "GPIO2" : CYDR_CHIP_S3 ? "GPIO13" : "GPIO12";
        case CYDR_READBACK_SDA:  return "shared GPIO13";
        default:                 return "unavailable";
    }
}

void chipguy_CYD_resistive_display::setRotation(uint16_t rotation) {
    _waitPixelDone();           // bus must be idle before we send a command
    _rotation = cydr_normalize_rotation(rotation);
    uint8_t madctl = 0;

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
    // The table is the ILI9341's, whose rotation 0 sets MX.  A panel whose
    // rotation 0 doesn't (the ST7789: its columns run the other way) flips MX
    // in every rotation, and one whose rows run the other way flips MY.  The
    // ST7796 uses the table as is.  _mx0 / _my0 / _bgr come from the panel or
    // the overrides (see begin()).
    if (!_mx0) madctl ^= ILI9341_MADCTL_MX;
    if (_my0)  madctl ^= ILI9341_MADCTL_MY;
    if (_bgr)  madctl |= CYDR_MADCTL_BGR;
    _madctl = madctl;

    _cmd(0x36);          // MADCTL
    _data8(madctl);
}

void chipguy_CYD_resistive_display::setColorOrder(bool bgr) {
    _bgr = bgr;
    setRotation(_rotation);
}

void chipguy_CYD_resistive_display::setInversion(bool on) {
    _waitPixelDone();
    _invert = on;
    _cmd(on ? 0x21 : 0x20);
}

void chipguy_CYD_resistive_display::setMirror(bool mx, bool my) {
    _mx0 = mx; _my0 = my;
    setRotation(_rotation);
}

void chipguy_CYD_resistive_display::autoValues(cydr_overrides_t &o) const {
    o = _auto;
    if (!_panelGuessed) o.panel = 0;
    else if (!o.panel) o.panel = _panel;
    o.spiClock = 0;                       // auto is 40 MHz, neither of the two choices
}

const char *chipguy_CYD_resistive_display::powerOffHint() const {
    switch (_powerKey) {
        case CYDR_PWRKEY_DOUBLE_PRESS: return "press SW1 twice quickly";
        case CYDR_PWRKEY_LONG_HOLD:    return "hold SW1 for 10 s";
        case CYDR_PWRKEY_EITHER:       return "hold SW1 for 10 s, or press it twice quickly";
        default:                       return nullptr;
    }
}

int8_t chipguy_CYD_resistive_display::gpio34IsBattery() const {
    if (!CYDR_CHIP_CLASSIC) return -1;
    if (_ovr.gpio34) return _ovr.gpio34 == 2;
    return _cydr_gpio34FromModel(_model);
}

static int8_t _cydr_gpio34FromModel(cydr_model_t m) {
    switch (m) {
        case CYDR_MODEL_NM_CYD_C5:          // no GPIO34 on the C5
        case CYDR_MODEL_ES3C28P:            // nor on the S3 board (its battery is GPIO9)
        case CYDR_MODEL_ES3N28P:
            return -1;
        case CYDR_MODEL_32E_28: case CYDR_MODEL_32E_32: case CYDR_MODEL_32E_35_40:
            return 1;
        case CYDR_MODEL_UNKNOWN:
            return -1;
        default:
            return 0;
    }
}

// ── Overrides in NVS ───────────────────────────────────────────────────────
// One blob, so the set is written and read as a unit.  Fields are only ever
// appended, so a shorter blob (saved by an earlier library version) is read
// as far as it goes and the rest is AUTO; a longer one (a later version) is
// ignored.
#define CYDR_OVR_NS  "cydrforce"
#define CYDR_OVR_KEY "v1"

bool cydr_anyOverride(const cydr_overrides_t &o) {
    const uint8_t *p = (const uint8_t *)&o;
    for (size_t i = 0; i < sizeof o; i++) if (p[i]) return true;
    return false;
}

bool cydr_loadOverrides(cydr_overrides_t &o) {
    o = {};
    Preferences prefs;
    if (!prefs.begin(CYDR_OVR_NS, /*readOnly=*/true)) return false;
    size_t n = prefs.getBytesLength(CYDR_OVR_KEY);
    if (n > 0 && n <= sizeof o) prefs.getBytes(CYDR_OVR_KEY, &o, n);
    prefs.end();
    // Anything out of range reads as AUTO: panel takes 1..3, model and size
    // their lists, the rest 1..2.
    uint8_t *p = (uint8_t *)&o;
    for (size_t i = 0; i < sizeof o; i++) {
        uint8_t max = i == offsetof(cydr_overrides_t, panel) ? 3 :
                      i == offsetof(cydr_overrides_t, spiClock) ? 3 :
                      i == offsetof(cydr_overrides_t, model) ? CYDR_MODEL_COUNT - 1 :
                      i == offsetof(cydr_overrides_t, size)  ? sizeof cydr_sizeTenths - 1 :
                      i == offsetof(cydr_overrides_t, powerKey) ? CYDR_PWRKEY_EITHER : 2;
        if (p[i] > max) p[i] = 0;
    }
    return cydr_anyOverride(o);
}

void cydr_saveOverrides(const cydr_overrides_t &o) {
    Preferences prefs;
    if (!prefs.begin(CYDR_OVR_NS, /*readOnly=*/false)) return;
    if (cydr_anyOverride(o)) prefs.putBytes(CYDR_OVR_KEY, &o, sizeof o);
    else                     prefs.remove(CYDR_OVR_KEY);
    prefs.end();
}

void cydr_clearOverrides() {
    Preferences prefs;
    if (!prefs.begin(CYDR_OVR_NS, /*readOnly=*/false)) return;
    prefs.remove(CYDR_OVR_KEY);
    prefs.end();
}

// The speed-test record: its own key in the overrides' namespace, so
// clearing the overrides leaves it.
#define CYDR_SPEED_KEY "speed"

bool cydr_loadSpeedTest(cydr_speed_test_t &r) {
    r = {};
    Preferences prefs;
    if (!prefs.begin(CYDR_OVR_NS, /*readOnly=*/true)) return false;
    bool ok = prefs.getBytesLength(CYDR_SPEED_KEY) == sizeof r &&
              prefs.getBytes(CYDR_SPEED_KEY, &r, sizeof r) == sizeof r;
    prefs.end();
    if (!ok || r.result > CYDR_SPEED_FAILED || r.version != CYDR_SPEED_TEST_VERSION) {
        r = {};
        return false;
    }
    return r.result != CYDR_SPEED_UNTESTED;
}

void cydr_saveSpeedTest(const cydr_speed_test_t &r) {
    Preferences prefs;
    if (!prefs.begin(CYDR_OVR_NS, /*readOnly=*/false)) return;
    cydr_speed_test_t v = r;
    v.version = CYDR_SPEED_TEST_VERSION;
    prefs.putBytes(CYDR_SPEED_KEY, &v, sizeof v);
    prefs.end();
}

// The refresh-rate choice: its own key, likewise kept by "Clear all".
#define CYDR_REFRESH_KEY "refresh"

bool cydr_loadRefreshChoice(cydr_refresh_choice_t &c) {
    c = {};
    Preferences prefs;
    if (!prefs.begin(CYDR_OVR_NS, /*readOnly=*/true)) return false;
    bool ok = prefs.getBytesLength(CYDR_REFRESH_KEY) == sizeof c &&
              prefs.getBytes(CYDR_REFRESH_KEY, &c, sizeof c) == sizeof c;
    prefs.end();
    if (!ok || c.stockHz <= 0) { c = {}; return false; }
    return true;
}

void cydr_saveRefreshChoice(const cydr_refresh_choice_t &c) {
    Preferences prefs;
    if (!prefs.begin(CYDR_OVR_NS, /*readOnly=*/false)) return;
    prefs.putBytes(CYDR_REFRESH_KEY, &c, sizeof c);
    prefs.end();
}

void cydr_clearRefreshChoice() {
    Preferences prefs;
    if (!prefs.begin(CYDR_OVR_NS, /*readOnly=*/false)) return;
    prefs.remove(CYDR_REFRESH_KEY);
    prefs.end();
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

    // The bus is idle: the moment to sample a shared-bus touch controller, so
    // its reader never waits on a pixel DMA (see setTouchSampler()).
    runTouchSampler();

    // Address window (synchronous commands), then queue the pixel blit.
    _setAddrWindow(x1, y1, x2, y2);

    // The classic ESP32 at 80 MHz loses a byte of any DMA transfer whose
    // length is 2 mod 4 -- an odd pixel count: byte 14 from the end arrives as
    // 00 (bench, 2026-11-09; README-engineering.md, "The SPI clock").  So
    // above 60 MHz an odd-count region goes out as the DMA of all but its
    // last pixel (a multiple of 4 bytes) and that pixel as a second, tiny
    // transfer.  Both are data (DC stays HIGH); the panel's RAMWR carries on
    // across the CS toggle between them.  At the default 40 MHz nothing
    // changes.
    const bool split = (count & 1) && count > 1 && _spiHz > 60000000;
    _pixelTrans = {};
    _pixelTrans.length = (count - split) * 16;   // bits (2 bytes per pixel)
    _pixelTrans.tx_buffer = pixels;
    _pixelTrans.user = (void *)1;           // DC = data
    if (spi_device_queue_trans(_spi, &_pixelTrans, portMAX_DELAY) == ESP_OK) {
        _pixelBusy = 1;
        if (split) {
            _tailTrans = {};
            _tailTrans.length = 16;
            _tailTrans.flags = SPI_TRANS_USE_TXDATA;
            memcpy(_tailTrans.tx_data, &pixels[count - 1], 2);   // already big-endian
            _tailTrans.user = (void *)1;
            if (spi_device_queue_trans(_spi, &_tailTrans, portMAX_DELAY) == ESP_OK) _pixelBusy++;
        }
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
