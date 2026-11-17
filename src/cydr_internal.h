/*
 * cydr_internal.h - declarations shared between the library's own source
 * files (chipguy_CYD_resistive_display.cpp and the per-chip board files).
 * Not part of the API: sketches don't include it.
 */

#pragma once

#include "chipguy_CYD_resistive_display.h"

// Free a stuck I2C bus: clock SCL until the slave lets go of SDA, then a
// STOP.  Leaves both pins reset (inputs, off any peripheral).
void _cydr_i2cBusRecover(int sda, int scl);

#if CYDR_CHIP_S3
// chipguy_CYD_board_ESP32S3.cpp -- THESE ONLY APPLY TO THE ESP32-S3.

// Probe the S3 board's I2C bus (SDA 16 / SCL 15) for the touch chip
// (FT6336 at 0x38) and the audio codec (ES8311 at 0x18).  Resets the
// touch chip if it doesn't answer at first.  *codec: whether the ES8311
// answered.
cydr_touch_chip_t _cydr_s3ProbeI2c(uint8_t *addr, bool *codec);

// Which S3 board, from the panel and what answered on I2C.  UNKNOWN for a
// combination not seen yet (with a line on Serial saying what was found).
cydr_model_t _cydr_s3Model(cydr_panel_t panel, bool panelSdaOnly,
                           cydr_touch_chip_t cap, bool codec);

// The WS2812's data pin for an S3 model (-1 if none or unknown).
int8_t _cydr_s3RgbLedPin(cydr_model_t m);

// Whether an S3 model's panel is an IPS ILI9341V that wants the vendor's
// power / VCOM / gamma values and inversion ON.
bool _cydr_s3PanelIsIps(cydr_model_t m);
#endif
