/*
 * platform.h - what differs between the chips, behind one interface.
 *
 * Shared by every build.  Implemented twice, each file compiling to nothing on
 * the other chip:
 *   platform_ESP32.cpp    ESP32 (classic CYDs): the card on its own VSPI bus
 *                         through Arduino's SD library; the DAC output.
 *   platform_ESP32C5.cpp  ESP32-C5 (NM-CYD-C5): the card on the display's SPI
 *                         bus through ESP-IDF's SD-over-SPI driver; the PDM
 *                         output.
 *   platform_ESP32S3.cpp  ESP32-S3 (ES3C28P): the card on SDMMC through
 *                         SD_MMC; the ES8311 codec over I2S.
 * Everything else in the sketch is the same on both.
 */

#pragma once

#include <FS.h>
#include "CydAudioOut.h"


// Put the speaker path in its idle state: amplifier off where it can be
// switched, GPIO26 driven LOW.  Call once, early.
void platform_audio_idle();

// Mount the card.  Call after lv_setup.begin(): on the C5 the card shares the
// display's bus, which the display initializes.  False if it didn't mount.
bool platform_card_mount();

// The mounted card, for File / openNextFile() and AudioFileSourceFS.
fs::FS &platform_card();

// The chip's audio output.  Called once, from the audio task.
CydAudioOut *platform_new_output();

// The resolution the samples reach the speaker path at, for the corner
// digit and the sound screen: 8 (the classic ESP32's DAC), 1 (the C5's PDM
// stream) or 16 (the S3's ES8311 codec).
int platform_output_bits();

// Where the decoder task runs, and at what priority; see each platform file.
int platform_audio_core();
