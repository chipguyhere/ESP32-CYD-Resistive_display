/*
 * AudioOutputES8311S3.h - THIS FILE ONLY APPLIES TO THE ESP32-S3 (ES3C28P /
 * ES3N28P).  On any other chip it compiles to nothing.
 *
 * ESP8266Audio output for the ES3C28P's speaker: the S3's I2S port ->
 * ES8311 codec (I2C 0x18 on SDA 16 / SCL 15, shared with the touch chip) ->
 * SC8002B amplifier (SHUTDOWN on GPIO1, 10k pull-up: off until GPIO1 is
 * driven LOW) -> the SPEAKER connector.
 *   I2S:  MCLK 4   BCLK 5   WS 7   DOUT 8 (to the codec's DSDIN)
 *   GPIO6 is the codec's ASDOUT (microphone data), unused here.
 * See README-sound-ESP32-S3.md.
 *
 * Adapted from AudioOutputES8311.h in chipguy_ESP32-P4-Seeed-reTerminal-
 * D1001_display's LvglMP3Player example (itself from the Seengreat HUB75
 * S3's, where the codec sequence was first heard working): the same codec
 * register sequence and volume mapping, reshaped into this player's
 * CydAudioOut interface and its chunked, yield-per-chunk writing (as
 * AudioOutputC5Pdm.h).  What it keeps from that file:
 *   - The I2S channel is created once and kept running across tracks, so
 *     the codec's MCLK never stops between songs (no pop).
 *   - auto_clear: an empty DMA queue sends zeros -- silence, not a repeat
 *     of the last buffer.  Pause is simply not feeding samples.
 *   - Mono: stereo is downmixed to both slots; the speaker path is mono.
 *   - Volume is the codec's DAC volume register (0.5 dB steps), so the full
 *     16 bits reach the codec at every volume.
 *   - ConsumeSample() returns false once per chunk written, so
 *     AudioGeneratorMP3::loop() returns to its caller between chunks.
 */

#pragma once

#include <sdkconfig.h>
#if defined(CONFIG_IDF_TARGET_ESP32S3)

#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s_std.h>
#include "CydAudioOut.h"

class AudioOutputES8311S3 : public CydAudioOut {
public:
    static constexpr int kMclk = 4, kBclk = 5, kWs = 7, kDout = 8;
    static constexpr int kAmpPin = 1;            // SC8002B SHUTDOWN: LOW = on
    static constexpr int kSda = 16, kScl = 15;
    static constexpr uint8_t kAddr = 0x18;
    // ES8311 DAC volume register (0x32): 0xBF = 0 dB, 0.5 dB per step.
    // 100 % = 0 dB; 0.8 steps (0.4 dB) per percent below that, as the
    // reTerminal copy.  Clean, with a usable range on a small speaker
    // (bench, 2026-11-09).
    static constexpr uint8_t kMaxReg = 0xBF;
    static constexpr int kStepsPerPercent = 4;   // per 5 percent
    // DMA: kDescs buffers of kFrames stereo frames, ~140 ms at 44.1 kHz, to
    // ride out a card read.
    static constexpr int kDescs = 12;
    static constexpr int kFrames = 512;
    static constexpr int kChunk = 256;           // stereo frames per write

    AudioOutputES8311S3() {
        hertz = 44100;
        channels = 2;
    }
    ~AudioOutputES8311S3() override {
        if (_tx) {
            i2s_channel_disable(_tx);
            i2s_del_channel(_tx);
        }
    }

    void setVolume(uint8_t pct) override {
        _volume = pct > 100 ? 100 : pct;
        if (_codecOk) applyVolume();
    }
    uint8_t volume() const override { return _volume; }
    bool hasDither() const override { return false; }
    void setDither(bool) override {}
    bool dither() const override { return false; }
    void setAmpEnabled(bool on) override {
        pinMode(kAmpPin, OUTPUT);
        digitalWrite(kAmpPin, on ? LOW : HIGH);
    }
    int outputRate() const override { return _tx ? hertz : 0; }
    uint32_t underruns() const override { return _underruns; }

    // ---- AudioOutput interface --------------------------------------------

    bool begin() override { return ensureRunning(); }

    bool SetRate(int hz) override {
        if (hz == hertz) return true;
        if (hz < 8000 || hz > 48000) return false;
        hertz = hz;
        if (!_tx) return true;
        flush();
        // The codec's dividers are all relative to MCLK (256 fs), so it
        // follows the new rate without a register change.
        i2s_channel_disable(_tx);
        i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)hz);
        esp_err_t err = i2s_channel_reconfig_std_clock(_tx, &clk);
        if (err != ESP_OK) Serial.printf("AudioOutputES8311S3: rate %d: %s\n", hz, esp_err_to_name(err));
        i2s_channel_enable(_tx);
        return err == ESP_OK;
    }

    bool SetChannels(int ch) override {
        if (ch < 1 || ch > 2) return false;
        channels = ch;
        return true;
    }

    bool ConsumeSample(int16_t sample[2]) override {
        if (!_tx) return false;
        if (_yield) { _yield = false; return false; }   // see the header comment
        int16_t mono = channels == 2 ? (int16_t)(((int32_t)sample[0] + sample[1]) / 2) : sample[0];
        _buf[2 * _fill] = mono;
        _buf[2 * _fill + 1] = mono;
        if (++_fill == kChunk) {
            write(_buf, kChunk);
            _fill = 0;
            _yield = true;
        }
        return true;
    }

    // Send what's pending, padded with zeros.
    void flush() override {
        if (!_tx || !_fill) return;
        memset(_buf + 2 * _fill, 0, (kChunk - _fill) * 2 * sizeof(int16_t));
        write(_buf, kChunk);
        _fill = 0;
    }

    // End of a track: keep the port (and the codec's MCLK) running.
    bool stop() override { flush(); return true; }

private:
    i2s_chan_handle_t _tx = nullptr;
    int16_t _buf[2 * kChunk];
    int _fill = 0;
    bool _yield = false;
    bool _codecOk = false;
    volatile uint8_t _volume = 70;
    volatile uint32_t _underruns = 0;

    static bool IRAM_ATTR onSendOverflow(i2s_chan_handle_t, i2s_event_data_t *, void *arg) {
        ((AudioOutputES8311S3 *)arg)->_underruns++;
        return false;
    }

    void write(const int16_t *p, int frames) {
        size_t done = 0;
        // Generous: a chunk is ~6 ms; anything near a second means the
        // channel stalled, and dropping a chunk beats wedging the task.
        esp_err_t err = i2s_channel_write(_tx, p, frames * 2 * sizeof(int16_t), &done, 1000);
        if (err != ESP_OK) Serial.printf("AudioOutputES8311S3: write: %s\n", esp_err_to_name(err));
    }

    bool writeReg(uint8_t reg, uint8_t val) {
        Wire.beginTransmission(kAddr);
        Wire.write(reg);
        Wire.write(val);
        bool ok = Wire.endTransmission() == 0;
        if (!ok) Serial.printf("ES8311 write 0x%02X=0x%02X failed\n", reg, val);
        return ok;
    }

    void applyVolume() {
        if (_volume == 0) { writeReg(0x31, 0x60); return; }     // DAC mute
        int reg = (int)kMaxReg - (100 - _volume) * kStepsPerPercent / 5;
        writeReg(0x32, (uint8_t)(reg < 0 ? 0 : reg));
        writeReg(0x31, 0x00);                                    // unmute
    }

    // Slave DAC on an external 256 fs MCLK, 16-bit I2S in.  After esp-adf's
    // es8311.c (codec init + start, DAC path only); the sequence of the
    // reTerminal / Seengreat copies.
    bool codecInit() {
        bool ok = true;
        ok &= writeReg(0x00, 0x1F); delay(10);   // reset
        ok &= writeReg(0x00, 0x00); delay(10);
        ok &= writeReg(0x01, 0x30);              // clock manager: pre-config
        ok &= writeReg(0x02, 0x00);              // MCLK pre-div 1, mult 1 (MCLK = 256 fs)
        ok &= writeReg(0x03, 0x10);              // ADC OSR
        ok &= writeReg(0x16, 0x24);
        ok &= writeReg(0x04, 0x10);              // DAC OSR
        ok &= writeReg(0x05, 0x00);              // ADC/DAC clock dividers 1
        ok &= writeReg(0x0B, 0x00);
        ok &= writeReg(0x0C, 0x00);
        ok &= writeReg(0x10, 0x1F);
        ok &= writeReg(0x11, 0x7F);
        ok &= writeReg(0x00, 0x80);              // power on (CSM)
        ok &= writeReg(0x01, 0x3F);              // all clocks on, MCLK from the MCLK pin
        ok &= writeReg(0x06, 0x03);              // BCLK divider: MCLK/4 = 64 fs
        ok &= writeReg(0x07, 0x00);              // LRCK divider high
        ok &= writeReg(0x08, 0xFF);              // LRCK divider low: 256 -> fs
        ok &= writeReg(0x09, 0x0C);              // DAC serial port: I2S, 16-bit
        ok &= writeReg(0x0A, 0x0C);              // ADC serial port: I2S, 16-bit
        ok &= writeReg(0x0D, 0x01);              // power up analog
        ok &= writeReg(0x0E, 0x02);
        ok &= writeReg(0x12, 0x00);              // DAC power on
        ok &= writeReg(0x13, 0x10);
        ok &= writeReg(0x1C, 0x6A);
        ok &= writeReg(0x37, 0x08);
        ok &= writeReg(0x45, 0x00);
        ok &= writeReg(0x32, 0x00);              // volume: silent until applyVolume()
        ok &= writeReg(0x31, 0x00);              // DAC unmute
        return ok;
    }

    bool ensureRunning() {
        if (_tx) return true;
        // The touch driver has usually started Wire on these pins already;
        // then this is a no-op.  (The ES3N28P has no touch chip.)
        Wire.begin(kSda, kScl, 400000);

        // I2S first, so the codec has MCLK when it is configured.
        i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
        cc.auto_clear = true;              // underrun: zeros, silent
        cc.dma_desc_num = kDescs;
        cc.dma_frame_num = kFrames;
        esp_err_t err = i2s_new_channel(&cc, &_tx, nullptr);
        if (err != ESP_OK) {
            Serial.printf("AudioOutputES8311S3: i2s_new_channel: %s\n", esp_err_to_name(err));
            _tx = nullptr;
            return false;
        }
        i2s_std_config_t cfg = {};
        cfg.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)hertz);   // MCLK = 256 fs
        cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
        cfg.gpio_cfg.mclk = (gpio_num_t)kMclk;
        cfg.gpio_cfg.bclk = (gpio_num_t)kBclk;
        cfg.gpio_cfg.ws   = (gpio_num_t)kWs;
        cfg.gpio_cfg.dout = (gpio_num_t)kDout;
        cfg.gpio_cfg.din  = I2S_GPIO_UNUSED;
        err = i2s_channel_init_std_mode(_tx, &cfg);
        if (err == ESP_OK) {
            i2s_event_callbacks_t cbs = {};
            cbs.on_send_q_ovf = onSendOverflow;
            i2s_channel_register_event_callback(_tx, &cbs, this);
            err = i2s_channel_enable(_tx);
        }
        if (err != ESP_OK) {
            Serial.printf("AudioOutputES8311S3: start: %s\n", esp_err_to_name(err));
            i2s_del_channel(_tx);
            _tx = nullptr;
            return false;
        }
        _codecOk = codecInit();
        if (!_codecOk) Serial.println("AudioOutputES8311S3: codec init failed (no ES8311 at 0x18?)");
        else applyVolume();
        _fill = 0;
        _yield = false;
        return true;
    }
};

#endif  // CONFIG_IDF_TARGET_ESP32S3
