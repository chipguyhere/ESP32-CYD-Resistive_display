/*
 * chipguy_CYD_board_ESP32S3.cpp
 *
 *   ********************************************************************
 *   *  THIS FILE ONLY APPLIES TO THE ESP32-S3.  On every other chip it  *
 *   *  compiles to nothing.                                             *
 *   ********************************************************************
 *
 * Board identification for the ESP32-S3 CYD-style boards.  The classic
 * boards are told apart by backlight pull-downs and GPIO4's voltage; those
 * pins mean nothing here.  On the S3 the board is named by the panel's ID
 * and by which chips answer on the shared I2C bus:
 *
 *   ES3C28P  (black PCB, "2.8" LCD Display / ESP32-S3 240x320 / Capacitive
 *            Touch"): ILI9341V IPS panel answering 00 93 41 on MISO (GPIO13);
 *            FT6336G touch at 0x38 and ES8311 codec at 0x18, both on
 *            SDA 16 / SCL 15.
 *   ES3N28P  the same board without the touch panel: the ES8311 answers,
 *            the FT6336 doesn't.  (Not on the bench: inferred from the
 *            vendor's shared specification.)
 *
 * A new S3 variant gets a line in _cydr_s3Model() -- and, if its touch chip
 * or I2C pins differ, in _cydr_s3ProbeI2c() -- and its pin facts in the
 * display and touch sources' CYDR_CHIP_S3 blocks.
 */

#include "chipguy_CYD_resistive_display.h"

#if CYDR_CHIP_S3

#include "cydr_internal.h"
#include <Wire.h>
#include <driver/gpio.h>

#define CYDR_S3_I2C_SDA   16
#define CYDR_S3_I2C_SCL   15
#define CYDR_S3_CTP_RST   18   // FT6336 reset, active LOW
#define CYDR_S3_FT6336    0x38
#define CYDR_S3_ES8311    0x18

static bool _cydr_s3Ack(uint8_t a) {
    Wire.beginTransmission(a);
    return Wire.endTransmission() == 0;
}

cydr_touch_chip_t _cydr_s3ProbeI2c(uint8_t *addr, bool *codec) {
    // Hold the touch chip out of reset.  GPIO18 has no pull on the board, so
    // after a reset of the S3 the FT6336's RST pin sees whatever its own
    // pull gives it.
    gpio_reset_pin((gpio_num_t)CYDR_S3_CTP_RST);
    gpio_set_level((gpio_num_t)CYDR_S3_CTP_RST, 1);
    gpio_set_direction((gpio_num_t)CYDR_S3_CTP_RST, GPIO_MODE_OUTPUT);

    bool touch = false, es = false;
    // A soft restart can leave a chip mid-transfer holding SDA; recover the
    // bus before each look (as on the classic capacitive boards).
    for (int attempt = 0; attempt < 2 && !touch; attempt++) {
        if (attempt) {
            // Not answering: reset it, and give it the ~200 ms the FT6336
            // takes to start answering after reset.
            gpio_set_level((gpio_num_t)CYDR_S3_CTP_RST, 0);
            delay(10);
            gpio_set_level((gpio_num_t)CYDR_S3_CTP_RST, 1);
        }
        uint32_t t0 = millis();
        do {
            _cydr_i2cBusRecover(CYDR_S3_I2C_SDA, CYDR_S3_I2C_SCL);
            Wire.begin(CYDR_S3_I2C_SDA, CYDR_S3_I2C_SCL, 400000);
            touch = _cydr_s3Ack(CYDR_S3_FT6336);
            es = es || _cydr_s3Ack(CYDR_S3_ES8311);
            Wire.end();
            if (!touch && attempt) delay(20);
        } while (!touch && attempt && millis() - t0 < 400);
    }
    gpio_reset_pin((gpio_num_t)CYDR_S3_I2C_SDA);
    gpio_reset_pin((gpio_num_t)CYDR_S3_I2C_SCL);
    if (codec) *codec = es;
    if (addr) *addr = touch ? CYDR_S3_FT6336 : 0;
    return touch ? CYDR_TOUCH_FT6336 : CYDR_TOUCH_NONE;
}

cydr_model_t _cydr_s3Model(cydr_panel_t panel, bool panelSdaOnly,
                           cydr_touch_chip_t cap, bool codec) {
    if (panel == CYDR_PANEL_ILI9341 && !panelSdaOnly && codec)
        return cap == CYDR_TOUCH_FT6336 ? CYDR_MODEL_ES3C28P : CYDR_MODEL_ES3N28P;
    Serial.printf("cydr display: ESP32-S3 board with a %s panel%s, touch %s, "
                  "codec %s: an unseen board variant; model unknown\n",
                  panel == CYDR_PANEL_ILI9341 ? "ILI9341" :
                  panel == CYDR_PANEL_ST7789  ? "ST7789"  :
                  panel == CYDR_PANEL_ST7796  ? "ST7796"  : "unknown",
                  panelSdaOnly ? " (answering on SDA)" : "",
                  cap == CYDR_TOUCH_FT6336 ? "FT6336 at 0x38" : "none at 0x38",
                  codec ? "ES8311 at 0x18" : "none at 0x18");
    return CYDR_MODEL_UNKNOWN;
}

int8_t _cydr_s3RgbLedPin(cydr_model_t m) {
    return m == CYDR_MODEL_ES3C28P || m == CYDR_MODEL_ES3N28P ? 42 : -1;
}

bool _cydr_s3PanelIsIps(cydr_model_t m) {
    return m == CYDR_MODEL_ES3C28P || m == CYDR_MODEL_ES3N28P;
}

#endif  // CYDR_CHIP_S3
