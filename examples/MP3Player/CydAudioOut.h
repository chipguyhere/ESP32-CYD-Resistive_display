/*
 * CydAudioOut.h - the audio output the player drives, on both chips.
 *
 * Shared by every build.  One subclass per chip implements it, each in a file
 * that compiles to nothing on the other chip:
 *   AudioOutputCydDac.h  ESP32 (classic CYDs): the 8-bit DAC on GPIO26.
 *   AudioOutputC5Pdm.h   ESP32-C5 (NM-CYD-C5): I2S PCM-to-PDM on GPIO26.
 *   AudioOutputES8311S3.h ESP32-S3 (ES3C28P): I2S into the ES8311 codec.
 * platform.h's platform_new_output() returns the right one.
 *
 * Both are ESP8266Audio AudioOutput classes, so the MP3 decoder feeds them
 * directly; this adds the controls the player and UI need on top.
 */

#pragma once

#include <AudioOutput.h>

class CydAudioOut : public AudioOutput {
public:
    // 0..100.  Applied per sample, so it may be changed from another task.
    virtual void setVolume(uint8_t pct) = 0;
    virtual uint8_t volume() const = 0;

    // Dither on the output's quantizer, where it has one worth dithering
    // (the ESP32's 8-bit DAC).  hasDither() false: setDither() is a no-op.
    virtual bool hasDither() const = 0;
    virtual void setDither(bool on) = 0;
    virtual bool dither() const = 0;

    // Mute the amplifier while paused or stopped, where the board can (the
    // ESP32-32E's GPIO4).  A no-op where the amplifier is always on.
    virtual void setAmpEnabled(bool on) = 0;

    // The rate the output hardware is actually running at (after any
    // upsampling), for the UI; 0 when not running.
    virtual int outputRate() const = 0;

    // Times the output ran dry mid-stream since start-up.
    virtual uint32_t underruns() const = 0;
};
