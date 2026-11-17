/*
 * platform.h - what BoardDetect reads differently on each chip, behind one
 * interface.
 *
 * Shared by every build.  Implemented per chip, each file compiling to
 * nothing on the others:
 *   platform_ESP32.cpp    the classic ESP32 CYDs: the card on VSPI, GPIO34
 *                         as battery or light sensor, GPIO4 as amp switch.
 *   platform_ESP32C5.cpp  the NM-CYD-C5: USB only, no light sensor, the
 *                         amplifier always on; the card on the display's
 *                         SPI bus through ESP-IDF's SD-over-SPI driver.
 *   platform_ESP32S3.cpp  the ESP32-S3 ES3C28P: the card on SDMMC, the
 *                         battery on GPIO9, the amp switch on GPIO1.
 */

#pragma once

#include <Arduino.h>
#include <chipguy_CYD_resistive_display.h>

// What a board has besides the screen (see the board README for each chip).
struct BoardFacts {
    bool battery;          // battery connector and charger
    bool batteryReading;   // an ADC pin reads the cell (through a divider)
    bool lightSensor;      // photoresistor
    bool ampSwitch;        // the speaker amplifier can be switched off
};

// The facts for a model; *known false when the model is unknown and they
// are guesses.
BoardFacts platform_facts(cydr_model_t m, bool *known);

// The card: type and size, or why not.  Mounts and unmounts it.
String platform_sd_text();

// The battery cell voltage in volts (only where batteryReading).
float platform_battery_volts();

// The light sensor's reading in mV (only where lightSensor), and whether
// that reading means darkness.
uint32_t platform_light_mv();
bool platform_is_dark();

// Where the amplifier switch is, e.g. "GPIO4" (shown when ampSwitch).
const char *platform_amp_switch_pin();
