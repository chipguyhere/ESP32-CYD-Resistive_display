/*
 * AudioOutputC5Pdm.h - THIS FILE ONLY APPLIES TO THE ESP32-C5 (NM-CYD-C5).
 * On any other chip it compiles to nothing; the classic ESP32's output is
 * AudioOutputCydDac.h.
 *
 * ESP8266Audio output for the NM-CYD-C5's speaker.  The ESP32-C5 has no DAC,
 * but its I2S peripheral has a PCM-to-PDM converter: DMA feeds it 16-bit
 * samples, and it upsamples them and sigma-delta modulates them into a 1-bit
 * stream at 128 x 48 kHz = 6.144 MHz on GPIO26 ("PDM DAC" line mode).  The
 * board's RC filter (two poles near 34 kHz) turns that back into audio for the
 * SC8002B amplifier.  No timer interrupt per sample, and far better than
 * 8-bit PWM: 16-bit samples, the quantization noise pushed into the MHz.
 * See README-sound-ESP32-C5.md.
 *
 *   - Mono: stereo is downmixed; the amplifier and speaker are mono.
 *   - Level: 100 % volume is full scale.  The top 9 dB or so is a little
 *     gritty (the modulator or the amplifier on 3.3 V), but not enough to be a
 *     problem (bench, 2026-11-09: a 440 Hz tone from full scale down in 3 dB
 *     steps); lower kMaxLevel to trade loudness for cleanness.
 *   - Pops: the line rests at 0 V before the first track.  The class ramps
 *     from the lowest code up to mid-scale (zero) over 300 ms with the
 *     converter's high-pass filter off, so the 330 nF coupling capacitor
 *     charges silently, and then leaves the channel running for good: between
 *     tracks and while paused it outputs zeros, which are mid-scale.
 *   - Underruns: the channel is set to auto-clear, so an empty DMA queue sends
 *     zeros (mid-scale): a gap, not a click.  underruns() counts them.
 *   - The amplifier has no enable pin on this board: setAmpEnabled() is a no-op.
 *   - No dither: the converter takes 16-bit samples.
 *
 * Like AudioOutputCydDac, ConsumeSample() returns false once per chunk
 * written, so AudioGeneratorMP3::loop() returns to its caller between chunks.
 */

#pragma once

#include <sdkconfig.h>
#if defined(CONFIG_IDF_TARGET_ESP32C5)

#include <Arduino.h>
#include <driver/i2s_pdm.h>
#include "CydAudioOut.h"

class AudioOutputC5Pdm : public CydAudioOut {
public:
    static constexpr int kPin = 26;
    // 100 % volume as a fraction of full scale, in 1/32768ths.  See above.
    static constexpr int32_t kMaxLevel = 32767;
    // DMA: kDescs buffers of kFrames samples.  12 x 512 = 6144 samples,
    // ~140 ms at 44.1 kHz, to ride out a card read on the shared bus.
    static constexpr int kDescs = 12;
    static constexpr int kFrames = 512;
    static constexpr int kChunk = 512;     // samples per i2s_channel_write()

    AudioOutputC5Pdm() {
        hertz = 44100;
        channels = 2;
    }
    ~AudioOutputC5Pdm() override {
        if (_tx) {
            i2s_channel_disable(_tx);
            i2s_del_channel(_tx);
        }
    }

    void setVolume(uint8_t pct) override { _volume = pct > 100 ? 100 : pct; }
    uint8_t volume() const override { return _volume; }
    bool hasDither() const override { return false; }
    void setDither(bool) override {}
    bool dither() const override { return false; }
    void setAmpEnabled(bool) override {}
    int outputRate() const override { return _tx ? hertz : 0; }
    uint32_t underruns() const override { return _underruns; }

    // ---- AudioOutput interface --------------------------------------------

    bool begin() override { return ensureRunning(); }

    bool SetRate(int hz) override {
        if (hz == hertz) return true;
        // The converter's upsampling divider is rate / 100 and may not exceed
        // 480: rates above 48 kHz aren't possible.
        if (hz < 8000 || hz > 48000) return false;
        hertz = hz;
        if (!_tx) return true;
        flush();
        i2s_channel_disable(_tx);
        i2s_pdm_tx_clk_config_t clk = I2S_PDM_TX_CLK_DAC_DEFAULT_CONFIG((uint32_t)hz);
        esp_err_t err = i2s_channel_reconfig_pdm_tx_clock(_tx, &clk);
        if (err != ESP_OK) Serial.printf("AudioOutputC5Pdm: rate %d: %s\n", hz, esp_err_to_name(err));
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
        int32_t mono = channels == 2 ? ((int32_t)sample[0] + sample[1]) / 2 : sample[0];
        _buf[_fill++] = (int16_t)((mono * kMaxLevel / 32768) * _volume / 100);
        if (_fill == kChunk) {
            write(_buf, kChunk);
            _fill = 0;
            _yield = true;
        }
        return true;
    }

    // Send what's pending, then a chunk of zeros: the line parks at mid-scale.
    void flush() override {
        if (!_tx) return;
        memset(_buf + _fill, 0, (kChunk - _fill) * sizeof(int16_t));
        write(_buf, kChunk);
        memset(_buf, 0, sizeof _buf);
        write(_buf, kChunk);
        _fill = 0;
    }

    bool stop() override { flush(); return true; }

private:
    i2s_chan_handle_t _tx = nullptr;
    int16_t _buf[kChunk];
    int _fill = 0;
    bool _yield = false;
    volatile uint8_t _volume = 70;
    volatile uint32_t _underruns = 0;

    static bool IRAM_ATTR onSendOverflow(i2s_chan_handle_t, i2s_event_data_t *, void *arg) {
        ((AudioOutputC5Pdm *)arg)->_underruns++;
        return false;
    }

    void write(const int16_t *p, int n) {
        size_t done = 0;
        // Generous: a chunk is ~12 ms; anything near a second means the
        // channel stalled, and dropping a chunk beats wedging the task.
        esp_err_t err = i2s_channel_write(_tx, p, n * sizeof(int16_t), &done, 1000);
        if (err != ESP_OK) Serial.printf("AudioOutputC5Pdm: write: %s\n", esp_err_to_name(err));
    }

    bool ensureRunning() {
        if (_tx) return true;
        i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
        cc.auto_clear = true;              // underrun: zeros = mid-scale, silent
        cc.dma_desc_num = kDescs;
        cc.dma_frame_num = kFrames;
        esp_err_t err = i2s_new_channel(&cc, &_tx, nullptr);
        if (err != ESP_OK) {
            Serial.printf("AudioOutputC5Pdm: i2s_new_channel: %s\n", esp_err_to_name(err));
            _tx = nullptr;
            return false;
        }
        i2s_pdm_tx_config_t pc = {
            .clk_cfg = I2S_PDM_TX_CLK_DAC_DEFAULT_CONFIG((uint32_t)hertz),
            .slot_cfg = I2S_PDM_TX_SLOT_DAC_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
            .gpio_cfg = { .clk = I2S_GPIO_UNUSED, .dout = (gpio_num_t)kPin, .dout2 = I2S_GPIO_UNUSED,
                          .invert_flags = { .clk_inv = false } },
        };
        // The converter's high-pass would take the start-up ramp's DC out and
        // put the step back; music has no DC, so it isn't needed.
        pc.slot_cfg.hp_en = false;
        err = i2s_channel_init_pdm_tx_mode(_tx, &pc);
        if (err == ESP_OK) {
            i2s_event_callbacks_t cbs = {};
            cbs.on_send_q_ovf = onSendOverflow;
            i2s_channel_register_event_callback(_tx, &cbs, this);
            err = i2s_channel_enable(_tx);
        }
        if (err != ESP_OK) {
            Serial.printf("AudioOutputC5Pdm: start: %s\n", esp_err_to_name(err));
            i2s_del_channel(_tx);
            _tx = nullptr;
            return false;
        }
        // Ramp from the lowest code (the line was at 0 V) up to zero.
        const int total = hertz * 3 / 10;
        for (int i = 0; i < total;) {
            int n = min(kChunk, total - i);
            for (int k = 0; k < n; k++, i++) _buf[k] = (int16_t)(-32767 + (int32_t)32767 * i / total);
            write(_buf, n);
        }
        _fill = 0;
        _yield = false;
        return true;
    }
};

#endif  // CONFIG_IDF_TARGET_ESP32C5
