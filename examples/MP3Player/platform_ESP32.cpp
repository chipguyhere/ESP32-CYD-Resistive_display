/*
 * platform_ESP32.cpp - THIS FILE ONLY APPLIES TO THE CLASSIC ESP32 CYDs
 * (ESP32-2432S028, ESP32-32E, ESP32-024, ESP32-035, ESP32-2432S032, ...).
 * On any other chip it compiles to nothing; the ESP32-C5's version is
 * platform_ESP32C5.cpp.  See platform.h.
 *
 * The card: the microSD slot has its own bus here -- CS on GPIO5, data on the
 * default VSPI pins (18/19/23), the same on every classic board -- so
 * Arduino's SD library mounts it, independent of the display on HSPI.
 * The output: AudioOutputCydDac, the 8-bit DAC on GPIO26 (channel 1 only).
 */

#include <sdkconfig.h>
#if defined(CONFIG_IDF_TARGET_ESP32)

#include "platform.h"
#include "AudioOutputCydDac.h"
#include <SPI.h>
#include <SD.h>

// SD.begin() defaults to a 4 MHz clock; the on-board slot is a short trace
// and runs fine faster, and slow reads are what cause DAC underruns.
#include <chipguy_CYD_resistive_display.h>   // cydr_sdCsPin()
static constexpr uint32_t kSdHz = 20000000;

void platform_audio_idle() {
    // Amplifier off first (GPIO4, active LOW; the board's 10 k pull-up has it
    // off already, this just makes it explicit).  The output class turns it
    // on once the DAC is running and parked at mid-scale.  Not on the
    // ESP32-2432S028 or ESP32-024, where GPIO4 is the red LED.
    if (AudioOutputCydDac::hasAmpPin()) {
        pinMode(AudioOutputCydDac::kAmpEnablePin, OUTPUT);
        digitalWrite(AudioOutputCydDac::kAmpEnablePin, HIGH);
    }
    // Park the speaker input at 0 V now.  GPIO26 boots as a floating input,
    // and the DAC's first enable would otherwise be a float-to-0 V step
    // through the amplifier; from a driven low it's 0 V to 0 V, then the
    // output class ramps up smoothly.
    pinMode(26, OUTPUT);
    digitalWrite(26, LOW);
}

bool platform_card_mount() {
    if (SD.begin(cydr_sdCsPin(), SPI, kSdHz)) return true;
    Serial.println("player: SD mount failed at 20 MHz, retrying at 4 MHz");
    SD.end();
    return SD.begin(cydr_sdCsPin(), SPI, 4000000);
}

fs::FS &platform_card() { return SD; }

CydAudioOut *platform_new_output() { return new AudioOutputCydDac(); }

int platform_output_bits() { return 8; }

// Core 0 keeps LVGL's core (1) free -- right for a sketch with no WiFi.  A
// sketch that uses WiFi should move this to core 1 instead: WiFi and LwIP
// own core 0 at priorities 23 and 18, and the decoder at priority 3 would sit
// under them.  On core 1 it only outranks loopTask (1), which is the point.
// See README-sound-ESP32.md, rule 3.
int platform_audio_core() { return 0; }

#endif  // CONFIG_IDF_TARGET_ESP32
